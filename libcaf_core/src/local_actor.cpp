/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include <string>
#include <condition_variable>

#include "caf/sec.hpp"
#include "caf/atom.hpp"
#include "caf/logger.hpp"
#include "caf/exception.hpp"
#include "caf/scheduler.hpp"
#include "caf/resumable.hpp"
#include "caf/actor_cast.hpp"
#include "caf/exit_reason.hpp"
#include "caf/local_actor.hpp"
#include "caf/actor_system.hpp"
#include "caf/actor_ostream.hpp"
#include "caf/blocking_actor.hpp"
#include "caf/binary_serializer.hpp"
#include "caf/default_attachable.hpp"
#include "caf/binary_deserializer.hpp"

#include "caf/detail/sync_request_bouncer.hpp"

namespace caf {

class local_actor::private_thread {
public:
  enum worker_state {
    active,
    shutdown_requested,
    await_resume_or_shutdown
  };

  private_thread(local_actor* self)
      : self_destroyed_(false),
        self_(self),
        state_(active),
        system_(self->system()) {
    intrusive_ptr_add_ref(self->ctrl());
    system_.inc_detached_threads();
  }

  void run() {
    auto job = const_cast<local_actor*>(self_);
    CAF_SET_LOGGER_SYS(&job->system());
    CAF_PUSH_AID(job->id());
    CAF_LOG_TRACE("");
    scoped_execution_unit ctx{&job->system()};
    auto max_throughput = std::numeric_limits<size_t>::max();
    bool resume_later;
    for (;;) {
      state_ = await_resume_or_shutdown;
      do {
        resume_later = false;
        switch (job->resume(&ctx, max_throughput)) {
          case resumable::resume_later:
            resume_later = true;
            break;
          case resumable::done:
            intrusive_ptr_release(job->ctrl());
            return;
          case resumable::awaiting_message:
            intrusive_ptr_release(job->ctrl());
            break;
          case resumable::shutdown_execution_unit:
            return;
        }
      } while (resume_later);
      // wait until actor becomes ready again or was destroyed
      if (! await_resume())
        return;
    }
  }

  bool await_resume() {
    std::unique_lock<std::mutex> guard(mtx_);
    while (state_ == await_resume_or_shutdown)
      cv_.wait(guard);
    return state_ == active;
  }

  void resume() {
    std::unique_lock<std::mutex> guard(mtx_);
    state_ = active;
    cv_.notify_one();
  }

  void shutdown() {
    std::unique_lock<std::mutex> guard(mtx_);
    state_ = shutdown_requested;
    cv_.notify_one();
  }

  static void exec(private_thread* this_ptr) {
    this_ptr->run();
    // make sure to not destroy the private thread object before the
    // detached actor is destroyed and this object is unreachable
    this_ptr->await_self_destroyed();
    // signalize destruction of detached thread to registry
    this_ptr->system_.dec_detached_threads();
    // done
    delete this_ptr;
  }

  void notify_self_destroyed() {
    std::unique_lock<std::mutex> guard(mtx_);
    self_destroyed_ = true;
    cv_.notify_one();
  }

  void await_self_destroyed() {
    std::unique_lock<std::mutex> guard(mtx_);
    while (! self_destroyed_)
      cv_.wait(guard);
  }

  void start() {
    std::thread{exec, this}.detach();
  }

private:
  std::mutex mtx_;
  std::condition_variable cv_;
  volatile bool self_destroyed_;
  volatile local_actor* self_;
  volatile worker_state state_;
  actor_system& system_;
};

result<message> reflect(local_actor*, const type_erased_tuple* x) {
  return message::from(x);
}

result<message> reflect_and_quit(local_actor* ptr, const type_erased_tuple* x) {
  ptr->quit();
  return reflect(ptr, x);
}

result<message> print_and_drop(local_actor* ptr, const type_erased_tuple* x) {
  CAF_LOG_WARNING_IF(x, "unexpected message" << CAF_ARG(*x));
  CAF_LOG_WARNING_IF(! x, "unexpected message: *x = ()");
  aout(ptr) << "*** unexpected message [id: " << ptr->id()
             << ", name: " << ptr->name() << "]: "
             << (x ? x->stringify() : "()")
             << std::endl;
  return sec::unexpected_message;
}

result<message> drop(local_actor*, const type_erased_tuple*) {
  return sec::unexpected_message;
}

void default_error_handler(local_actor* ptr, error& x) {
  ptr->quit(std::move(x));
}

void default_down_handler(local_actor* ptr, down_msg& x) {
  aout(ptr) << "*** unhandled down message [id: " << ptr->id()
             << ", name: " << ptr->name() << "]: " << to_string(x)
             << std::endl;
}

void default_exit_handler(local_actor* ptr, exit_msg& x) {
  if (x.reason)
    ptr->quit(std::move(x.reason));
}

// local actors are created with a reference count of one that is adjusted
// later on in spawn(); this prevents subtle bugs that lead to segfaults,
// e.g., when calling address() in the ctor of a derived class
local_actor::local_actor(actor_config& cfg)
    : monitorable_actor(cfg),
      context_(cfg.host),
      timeout_id_(0),
      initial_behavior_fac_(std::move(cfg.init_fun)),
      default_handler_(print_and_drop),
      error_handler_(default_error_handler),
      down_handler_(default_down_handler),
      exit_handler_(default_exit_handler),
      open_credit_(50),
      low_watermark_(10),
      max_credit_(50),
      private_thread_(nullptr) {
  if (cfg.groups != nullptr)
    for (auto& grp : *cfg.groups)
      join(grp);
}

local_actor::~local_actor() {
  CAF_LOG_TRACE("");
  // signalize to the private thread object that it is
  // unrachable and can be destroyed as well
  if (private_thread_)
    private_thread_->notify_self_destroyed();
}

void local_actor::on_destroy() {
  CAF_LOG_TRACE(CAF_ARG(is_terminated()));
  if (! is_cleaned_up()) {
    on_exit();
    cleanup(exit_reason::unreachable, nullptr);
    monitorable_actor::on_destroy();
  }
}

void local_actor::intrusive_ptr_add_ref_impl() {
  intrusive_ptr_add_ref(ctrl());
}

void local_actor::intrusive_ptr_release_impl() {
  intrusive_ptr_release(ctrl());
}

void local_actor::monitor(abstract_actor* ptr) {
  if (! ptr)
    return;
  ptr->attach(default_attachable::make_monitor(ptr->address(), address()));
}

void local_actor::demonitor(const actor_addr& whom) {
  CAF_LOG_TRACE(CAF_ARG(whom));
  auto ptr = actor_cast<strong_actor_ptr>(whom);
  if (! ptr)
    return;
  default_attachable::observe_token tk{address(), default_attachable::monitor};
  ptr->get()->detach(tk);
}

void local_actor::join(const group& what) {
  CAF_LOG_TRACE(CAF_ARG(what));
  if (what == invalid_group)
    return;
  if (what->subscribe(ctrl()))
    subscriptions_.emplace(what);
}

void local_actor::leave(const group& what) {
  CAF_LOG_TRACE(CAF_ARG(what));
  if (subscriptions_.erase(what) > 0)
    what->unsubscribe(ctrl());
}

void local_actor::on_exit() {
  // nop
}

std::vector<group> local_actor::joined_groups() const {
  std::vector<group> result;
  for (auto& x : subscriptions_)
    result.emplace_back(x);
  return result;
}

uint32_t local_actor::request_timeout(const duration& d) {
  if (! d.valid()) {
    has_timeout(false);
    return 0;
  }
  has_timeout(true);
  auto result = ++timeout_id_;
  auto msg = make_message(timeout_msg{++timeout_id_});
  CAF_LOG_TRACE("send new timeout_msg, " << CAF_ARG(timeout_id_));
  if (d.is_zero())
    // immediately enqueue timeout message if duration == 0s
    enqueue(ctrl(), invalid_message_id, std::move(msg), context());
  else
    system().scheduler().delayed_send(d, ctrl(), strong_actor_ptr(ctrl()),
                                      message_id::make(), std::move(msg));
  return result;
}

void local_actor::request_sync_timeout_msg(const duration& d, message_id mid) {
  CAF_LOG_TRACE(CAF_ARG(d) << CAF_ARG(mid));
  if (! d.valid())
    return;
  system().scheduler().delayed_send(d, ctrl(),
                                    ctrl(),
                                    mid.response_id(),
                                    make_message(sec::request_timeout));
}

void local_actor::handle_timeout(behavior& bhvr, uint32_t timeout_id) {
  if (! is_active_timeout(timeout_id))
    return;
  bhvr.handle_timeout();
  if (bhvr_stack_.empty() || bhvr_stack_.back() != bhvr)
    return;
  // auto-remove behavior for blocking actors
  if (is_blocking()) {
    CAF_ASSERT(bhvr_stack_.back() == bhvr);
    bhvr_stack_.pop_back();
    return;
  }
}

void local_actor::reset_timeout(uint32_t timeout_id) {
  if (is_active_timeout(timeout_id)) {
    has_timeout(false);
  }
}

bool local_actor::is_active_timeout(uint32_t tid) const {
  return has_timeout() && timeout_id_ == tid;
}

uint32_t local_actor::active_timeout_id() const {
  return timeout_id_;
}

local_actor::msg_type local_actor::filter_msg(mailbox_element& x) {
  message& msg = x.msg;
  auto mid = x.mid;
  if (mid.is_response())
    return msg_type::response;
  switch (msg.type_token()) {
    // run-time access to various meta information
    case make_type_token<atom_value, atom_value, std::string>():
      if (msg.get_as<atom_value>(0) == sys_atom::value
          && msg.get_as<atom_value>(1) == get_atom::value) {
        auto& what = msg.get_as<std::string>(2);
        if (what == "info") {
          CAF_LOG_DEBUG("reply to 'info' message");
          x.sender->enqueue(
            mailbox_element::make(ctrl(), x.mid.response_id(),
                                  {}, ok_atom::value, std::move(what),
                                  strong_actor_ptr{ctrl()}, name()),
            context());
        } else {
          x.sender->enqueue(
            mailbox_element::make(ctrl(), x.mid.response_id(),
                                  {}, sec::unsupported_sys_key),
            context());
        }
        return msg_type::sys_message;
      }
      return msg_type::ordinary;
    // register a new source at a sink
    case make_type_token<atom_value, atom_value>():
      if (msg.get_as<atom_value>(0) == sys_atom::value
          && msg.get_as<atom_value>(1) == add_source_atom::value) {
        if (! x.sender) {
          CAF_LOG_ERROR("received ('sys', 'addSource', X) from anonymous");
          return msg_type::sys_message;
        }
        if (! x.stages.empty()) {
          CAF_LOG_ERROR("received multi-staged ('sys', 'addSource', X)");
          return msg_type::sys_message;
        }
        if (! sources_.emplace(actor_cast<actor_addr>(x.sender), open_credit_).second) {
          CAF_LOG_ERROR("multiple 'addSource'" << CAF_ARG(x.sender));
          return msg_type::sys_message;
        }
        if (open_credit_ > 0) {
          // give new source remaining credit
          x.sender->enqueue(mailbox_element::make(ctrl(), message_id::make(),
                                                  {}, sys_atom::value,
                                                  get_atom::value,
                                                  open_credit_),
                             context());
          open_credit_ = 0;
        }
        auto source_addr = actor_cast<actor_addr>(x.sender);
        weak_actor_ptr weak_this{ctrl()};
        x.sender->get()->attach_functor([=](const error&, execution_unit* ctx) {
          auto strong_this = actor_cast<strong_actor_ptr>(weak_this);
          if (! strong_this)
            return;
          strong_this->enqueue(mailbox_element::make(nullptr,
                                                     message_id::make(), {},
                                                     sys_atom::value,
                                                     del_source_atom::value,
                                                     source_addr),
                               ctx);
        });
        return msg_type::sys_message;
      }
      return msg_type::ordinary;
    case make_type_token<atom_value, atom_value, actor_addr>():
      if (msg.get_as<atom_value>(0) == sys_atom::value
          && msg.get_as<atom_value>(1) == del_source_atom::value) {
        // drop anonymous 'delSource' messages
        auto src = msg.get_as<actor_addr>(2);
        auto i = sources_.find(src);
        if (i == sources_.end())
          return msg_type::sys_message;
        auto released_credit = i->second;
        sources_.erase(i);
        grant_credit(released_credit, sources_.end());
        return msg_type::sys_message;
      }
      return msg_type::ordinary;
    case make_type_token<atom_value, atom_value, uint64_t>():
      if (msg.get_as<atom_value>(0) == sys_atom::value
          && msg.get_as<atom_value>(1) == get_atom::value) {
        auto num = msg.get_as<uint64_t>(2);
        if (! x.sender) {
          CAF_LOG_ERROR("received ('sys', 'get', X) from anonymous");
          return msg_type::sys_message;
        }
        auto dest = actor_cast<actor>(x.sender);
        auto i = generators_.find(dest);
        if (i == generators_.end()) {
          CAF_LOG_INFO("dropped ('sys', 'get', X) from unknown sink");
          return msg_type::sys_message;
        }
        auto& f = i->second.first;
        for (uint64_t n = 0; n < num; ++n)
          if (! f())
            return msg_type::sys_message;
        return msg_type::sys_message;
      }
      return msg_type::ordinary;
    case make_type_token<timeout_msg>(): {
      auto& tm = msg.get_as<timeout_msg>(0);
      auto tid = tm.timeout_id;
      CAF_ASSERT(! mid.valid());
      return is_active_timeout(tid) ? msg_type::timeout
                                    : msg_type::expired_timeout;
    }
    case make_type_token<exit_msg>(): {
      auto& em = msg.get_as_mutable<exit_msg>(0);
      // make sure to get rid of attachables if they're no longer needed
      unlink_from(em.source);
      // exit_reason::kill is always fatal
      if (em.reason == exit_reason::kill)
        quit(std::move(em.reason));
      else
        exit_handler_(this, em);
      return msg_type::sys_message;
    }
    case make_type_token<down_msg>(): {
      auto& dm = msg.get_as_mutable<down_msg>(0);
      down_handler_(this, dm);
      return msg_type::sys_message;
    }
    case make_type_token<error>(): {
      auto& err = msg.get_as_mutable<error>(0);
      error_handler_(this, err);
      return msg_type::sys_message;
    }
    default:
      return msg_type::ordinary;
  }
}

namespace {

class invoke_result_visitor_helper {
public:
  invoke_result_visitor_helper(response_promise x) : rp(x) {
    // nop
  }

  void operator()(error& x) {
    CAF_LOG_DEBUG("report error back to requesting actor");
    rp.deliver(std::move(x));
  }

  void operator()(message& x) {
    CAF_LOG_DEBUG("respond via response_promise");
    // suppress empty messages for asynchronous messages
    if (x.empty() && rp.async())
      return;
    rp.deliver(std::move(x));
  }

  void operator()(const none_t&) {
    error err = sec::unexpected_response;
    (*this)(err);
  }

private:
  response_promise rp;
};

class local_actor_invoke_result_visitor : public detail::invoke_result_visitor {
public:
  local_actor_invoke_result_visitor(local_actor* ptr) : self_(ptr) {
    // nop
  }

  void operator()() override {
    // nop
  }

  template <class T>
  void delegate(T& x) {
    auto rp = self_->make_response_promise();
    if (! rp.pending()) {
      CAF_LOG_DEBUG("suppress response message: invalid response promise");
      return;
    }
    invoke_result_visitor_helper f{std::move(rp)};
    f(x);
  }

  void operator()(error& x) override {
    CAF_LOG_TRACE(CAF_ARG(x));
    delegate(x);
  }

  void operator()(message& x) override {
    CAF_LOG_TRACE(CAF_ARG(x));
    delegate(x);
  }

  void operator()(const none_t& x) override {
    CAF_LOG_TRACE(CAF_ARG(x));
    delegate(x);
  }

private:
  local_actor* self_;
};

class local_actor_flow_visitor : public detail::invoke_result_visitor {
public:
  using iterator = local_actor::sources_map::iterator;
  local_actor_flow_visitor(local_actor* ptr, iterator src)
      : self_(ptr),
        src_(src) {
    // nop
  }

  void operator()() override {
    // nop
  }

  void operator()(error& x) override {
    CAF_LOG_WARNING("flow-controlled message handler returned an error: "
                    << CAF_ARG(self_->system().render(x)));
    CAF_IGNORE_UNUSED(x);
  }

  void operator()(message& x) override {
    if (x.empty()) {
      self_->grant_credit(1, src_);
    } else {
      CAF_LOG_WARNING("flow-controlled message handler returned a message: "
                      << CAF_ARG(x));
    }
  }

  void operator()(const none_t&) override {
    CAF_LOG_WARNING("flow-controlled message handler returned none_t");
  }

private:
  local_actor* self_;
  iterator src_;
};

} // namespace <anonymous>

void local_actor::handle_response(mailbox_element_ptr& ptr,
                                  local_actor::pending_response& pr) {
  CAF_ASSERT(ptr != nullptr);
  auto& ref_fun = pr.second;
  ptr.swap(current_element_);
  auto& msg = current_element_->msg;
  auto guard = detail::make_scope_guard([&] {
    ptr.swap(current_element_);
  });
  local_actor_invoke_result_visitor visitor{this};
  auto invoke_error = [&](error err) {
    auto tmp = make_type_erased_tuple_view(err);
    if (ref_fun(visitor, tmp) == match_case::no_match) {
      CAF_LOG_WARNING("multiplexed response failure occured:"
                      << CAF_ARG(id()));
      error_handler_(this, err);
    }
  };
  if (msg.type_token() == make_type_token<sync_timeout_msg>()) {
    // TODO: check if condition can ever be true
    if (ref_fun.timeout().valid())
      ref_fun.handle_timeout();
    invoke_error(sec::request_timeout);
  } else if (ref_fun(visitor, msg) == match_case::no_match) {
    if (msg.type_token() == make_type_token<error>()) {
      error_handler_(this, msg.get_as_mutable<error>(0));
    } else {
      // wrap unhandled message into an error object and try invoke again
      invoke_error(make_error(sec::unexpected_response, current_element_->msg));
    }
  }
}

invoke_message_result local_actor::invoke_message(mailbox_element_ptr& ptr,
                                                  behavior& fun,
                                                  message_id awaited_id) {
  CAF_ASSERT(ptr != nullptr);
  CAF_LOG_TRACE(CAF_ARG(*ptr) << CAF_ARG(awaited_id));
  switch (filter_msg(*ptr)) {
    case msg_type::expired_timeout:
      CAF_LOG_DEBUG("dropped expired timeout message");
      return im_dropped;
    case msg_type::sys_message:
      CAF_LOG_DEBUG("handled system message");
      return im_dropped;
    case msg_type::timeout: {
      if (awaited_id == invalid_message_id) {
        CAF_LOG_DEBUG("handle timeout message");
        auto& tm = ptr->msg.get_as<timeout_msg>(0);
        handle_timeout(fun, tm.timeout_id);
        return im_success;
      }
      // ignore "async" timeout
      CAF_LOG_DEBUG("async timeout ignored while in sync mode");
      return im_dropped;
    }
    case msg_type::response: {
      auto mid = ptr->mid;
      auto response_handler = find_multiplexed_response(mid);
      if (response_handler) {
        CAF_LOG_DEBUG("handle as multiplexed response:" << CAF_ARG(ptr->msg)
                      << CAF_ARG(mid) << CAF_ARG(awaited_id));
        if (! awaited_id.valid()) {
          handle_response(ptr, *response_handler);
          mark_multiplexed_arrived(mid);
          return im_success;
        }
        CAF_LOG_DEBUG("skipped multiplexed response:" << CAF_ARG(awaited_id));
        return im_skipped;
      }
      response_handler = find_awaited_response(mid);
      if (response_handler) {
        if (awaited_id.valid() && mid == awaited_id) {
          handle_response(ptr, *response_handler);
          mark_awaited_arrived(mid);
          return im_success;
        }
        return im_skipped;
      }
      CAF_LOG_DEBUG("dropped expired response");
      return im_dropped;
    }
    case msg_type::ordinary: {
      if (awaited_id.valid()) {
        CAF_LOG_DEBUG("skipped asynchronous message:" << CAF_ARG(awaited_id));
        return im_skipped;
      }
      bool skipped = false;
      auto had_timeout = has_timeout();
      if (had_timeout)
        has_timeout(false);
      ptr.swap(current_element_);
      if (! current_element_->mid.is_flow_controlled()) {
        local_actor_invoke_result_visitor visitor{this};
        switch (fun(visitor, current_element_->msg)) {
          case match_case::skip:
            skipped = true;
            break;
          default:
            break;
          case match_case::no_match: {
            if (had_timeout)
              has_timeout(true);
            auto sres = default_handler_(this,
                                         current_element_->msg.cvals().get());
            if (sres.flag != rt_skip)
              visitor.visit(sres);
            else
              skipped = true;
          }
        }
      } else {
        if (! current_element_->sender) {
          CAF_LOG_ERROR("received flow-controlled message from anonymous");
        } else {
          auto src = sources_.find(actor_cast<actor_addr>(current_element_->sender));
          if (src == sources_.end()) {
            CAF_LOG_ERROR("received flow-controlled message from unknown source");
          } else {
            local_actor_flow_visitor visitor{this, src};
            switch (fun(visitor, current_element_->msg)) {
              case match_case::skip:
                skipped = true;
                break;
              default:
                break;
              case match_case::no_match: {
                if (had_timeout)
                  has_timeout(true);
                auto sres = default_handler_(this,
                                             current_element_->msg.cvals().get());
                if (sres.flag != rt_skip)
                  visitor.visit(sres);
                else
                  skipped = true;
              }
            }
          }
        }
      }
      ptr.swap(current_element_);
      if (skipped) {
        if (had_timeout)
          has_timeout(true);
        return im_skipped;
      }
      return im_success;
    }
  }
  // should be unreachable
  CAF_CRITICAL("invalid message type");
}

struct awaited_response_predicate {
public:
  explicit awaited_response_predicate(message_id mid) : mid_(mid) {
    // nop
  }

  bool operator()(const local_actor::pending_response& pr) const {
    return pr.first == mid_;
  }

private:
  message_id mid_;
};

message_id local_actor::new_request_id(message_priority mp) {
  auto result = ++last_request_id_;
  return mp == message_priority::normal ? result : result.with_high_priority();
}

void local_actor::mark_awaited_arrived(message_id mid) {
  CAF_ASSERT(mid.is_response());
  awaited_response_predicate predicate{mid};
  awaited_responses_.remove_if(predicate);
}

bool local_actor::awaits_response() const {
  return ! awaited_responses_.empty();
}

bool local_actor::awaits(message_id mid) const {
  CAF_ASSERT(mid.is_response());
  awaited_response_predicate predicate{mid};
  return std::any_of(awaited_responses_.begin(), awaited_responses_.end(),
                     predicate);
}

local_actor::pending_response*
local_actor::find_awaited_response(message_id mid) {
  awaited_response_predicate predicate{mid};
  auto last = awaited_responses_.end();
  auto i = std::find_if(awaited_responses_.begin(), last, predicate);
  if (i != last)
    return &(*i);
  return nullptr;
}

void local_actor::set_awaited_response_handler(message_id response_id, behavior bhvr) {
  auto opt_ref = find_awaited_response(response_id);
  if (opt_ref)
    opt_ref->second = std::move(bhvr);
  else
    awaited_responses_.emplace_front(response_id, std::move(bhvr));
}

behavior& local_actor::awaited_response_handler() {
  return awaited_responses_.front().second;
}

message_id local_actor::awaited_response_id() {
  return awaited_responses_.empty()
         ? message_id::make()
         : awaited_responses_.front().first;
}

void local_actor::mark_multiplexed_arrived(message_id mid) {
  CAF_ASSERT(mid.is_response());
  multiplexed_responses_.erase(mid);
}

bool local_actor::multiplexes(message_id mid) const {
  CAF_ASSERT(mid.is_response());
  auto it = multiplexed_responses_.find(mid);
  return it != multiplexed_responses_.end();
}

local_actor::pending_response*
local_actor::find_multiplexed_response(message_id mid) {
  auto it = multiplexed_responses_.find(mid);
  if (it != multiplexed_responses_.end())
    return &(*it);
  return nullptr;
}

void local_actor::set_multiplexed_response_handler(message_id response_id, behavior bhvr) {
  if (bhvr.timeout().valid()) {
    request_sync_timeout_msg(bhvr.timeout(), response_id);
  }
  auto opt_ref = find_multiplexed_response(response_id);
  if (opt_ref)
    opt_ref->second = std::move(bhvr);
  else
    multiplexed_responses_.emplace(response_id, std::move(bhvr));
}

void local_actor::launch(execution_unit* eu, bool lazy, bool hide) {
  CAF_LOG_TRACE(CAF_ARG(lazy) << CAF_ARG(hide));
  is_registered(! hide);
  if (is_detached()) {
    if (is_blocking()) {
      std::thread([](strong_actor_ptr ptr) {
        // actor lives in its own thread
        auto this_ptr = ptr->get();
        CAF_ASSERT(dynamic_cast<blocking_actor*>(this_ptr) != 0);
        auto self = static_cast<blocking_actor*>(this_ptr);
        error rsn;
        std::exception_ptr eptr = nullptr;
        try {
          self->act();
          rsn = self->fail_state_;
        }
        catch (actor_exited& e) {
          rsn = e.reason();
        }
        catch (...) {
          rsn = exit_reason::unhandled_exception;
          eptr = std::current_exception();
        }
        if (eptr) {
          auto opt_reason = self->handle(eptr);
          rsn = opt_reason ? *opt_reason
                           : exit_reason::unhandled_exception;
        }
        try {
          self->on_exit();
        }
        catch (...) {
          // simply ignore exception
        }
        self->cleanup(std::move(rsn), self->context());
      }, ctrl()).detach();
      return;
    }
    private_thread_ = new private_thread(this);
    private_thread_->start();
    return;
  }
  CAF_ASSERT(eu != nullptr);
  // do not schedule immediately when spawned with `lazy_init`
  // mailbox could be set to blocked
  if (lazy && mailbox().try_block())
    return;
  // scheduler has a reference count to the actor as long as
  // it is waiting to get scheduled
  intrusive_ptr_add_ref(ctrl());
  eu->exec_later(this);
}

void local_actor::enqueue(mailbox_element_ptr ptr, execution_unit* eu) {
  CAF_PUSH_AID(id());
  CAF_LOG_TRACE(CAF_ARG(*ptr));
  CAF_ASSERT(ptr != nullptr);
  CAF_ASSERT(! is_blocking());
  auto mid = ptr->mid;
  auto sender = ptr->sender;
  switch (mailbox().enqueue(ptr.release())) {
    case detail::enqueue_result::unblocked_reader: {
      // add a reference count to this actor and re-schedule it
      intrusive_ptr_add_ref(ctrl());
      if (is_detached()) {
        CAF_ASSERT(private_thread_ != nullptr);
        private_thread_->resume();
      } else {
        if (eu)
          eu->exec_later(this);
        else
          home_system().scheduler().enqueue(this);
      }
      break;
    }
    case detail::enqueue_result::queue_closed: {
      if (mid.is_request()) {
        detail::sync_request_bouncer f{exit_reason()};
        f(sender, mid);
      }
      break;
    }
    case detail::enqueue_result::success:
      // enqueued to a running actors' mailbox; nothing to do
      break;
  }
}

resumable::subtype_t local_actor::subtype() const {
  return scheduled_actor;
}

resumable::resume_result local_actor::resume(execution_unit* eu,
                                             size_t max_throughput) {
  CAF_PUSH_AID(id());
  CAF_LOG_TRACE("");
  CAF_ASSERT(eu != nullptr);
  CAF_ASSERT(! is_blocking());
  context(eu);
  if (is_initialized() && (! has_behavior() || is_terminated())) {
    CAF_LOG_DEBUG_IF(! has_behavior(),
                     "resume called on an actor without behavior");
    CAF_LOG_DEBUG_IF(is_terminated(),
                     "resume called on a terminated actor");
    return resumable::done;
  }
  std::exception_ptr eptr = nullptr;
  try {
    if (! is_initialized()) {
      initialize();
      if (finished()) {
        CAF_LOG_DEBUG("actor_done() returned true right "
                      << "after make_behavior()");
        return resumable::resume_result::done;
      } else {
        CAF_LOG_DEBUG("initialized actor:" << CAF_ARG(name()));
      }
    }
    int handled_msgs = 0;
    auto reset_timeout_if_needed = [&] {
      if (handled_msgs > 0 && ! bhvr_stack_.empty()) {
        request_timeout(bhvr_stack_.back().timeout());
      }
    };
    for (size_t i = 0; i < max_throughput; ++i) {
      auto ptr = next_message();
      if (ptr) {
        auto res = exec_event(ptr);
        if (res.first == resumable::resume_result::done)
          return resumable::resume_result::done;
        if (res.second == im_success)
          ++handled_msgs;
      } else {
        CAF_LOG_DEBUG("no more element in mailbox; going to block");
        reset_timeout_if_needed();
        if (mailbox().try_block())
          return resumable::awaiting_message;
        CAF_LOG_DEBUG("try_block() interrupted by new message");
      }
    }
    reset_timeout_if_needed();
    if (! has_next_message() && mailbox().try_block())
      return resumable::awaiting_message;
    // time's up
    return resumable::resume_later;
  }
  catch (actor_exited& what) {
    CAF_LOG_INFO("actor died because of exception:" << CAF_ARG(what.reason()));
    if (! is_terminated())
      quit(what.reason());
  }
  catch (std::exception& e) {
    CAF_LOG_INFO("actor died because of an exception, what: " << e.what());
    static_cast<void>(e); // keep compiler happy when not logging
    if (! is_terminated())
      quit(exit_reason::unhandled_exception);
    eptr = std::current_exception();
  }
  catch (...) {
    CAF_LOG_INFO("actor died because of an unknown exception");
    if (! is_terminated())
      quit(exit_reason::unhandled_exception);
    eptr = std::current_exception();
  }
  if (eptr) {
    auto opt_reason = handle(eptr);
    if (opt_reason) {
      // use exit reason defined by custom handler
      quit(*opt_reason);
    }
  }
  if (! finished()) {
    // actor has been "revived", try running it again later
    return resumable::resume_later;
  }
  return resumable::done;
}

std::pair<resumable::resume_result, invoke_message_result>
local_actor::exec_event(mailbox_element_ptr& ptr) {
  CAF_LOG_TRACE(CAF_ARG(*ptr));
  behavior empty_bhvr;
  auto& bhvr =
    awaits_response() ? awaited_response_handler()
                      : bhvr_stack().empty() ? empty_bhvr
                                             : bhvr_stack().back();
  auto mid = awaited_response_id();
  auto res = invoke_message(ptr, bhvr, mid);
  CAF_LOG_DEBUG(CAF_ARG(mid) << CAF_ARG(res));
  switch (res) {
    case im_success:
      bhvr_stack().cleanup();
      if (finished()) {
        CAF_LOG_DEBUG("actor exited");
        return {resumable::resume_result::done, res};
      }
      // continue from cache if current message was
      // handled, because the actor might have changed
      // its behavior to match 'old' messages now
      while (invoke_from_cache()) {
        if (finished()) {
          CAF_LOG_DEBUG("actor exited");
          return {resumable::resume_result::done, res};
        }
      }
      break;
    case im_skipped:
      CAF_ASSERT(ptr != nullptr);
      push_to_cache(std::move(ptr));
      break;
    case im_dropped:
      // system messages are reported as dropped but might
      // still terminate the actor
      bhvr_stack().cleanup();
      if (finished()) {
        CAF_LOG_DEBUG("actor exited");
        return {resumable::resume_result::done, res};
      }
      break;
  }
  return {resumable::resume_result::resume_later, res};
}

void local_actor::exec_single_event(execution_unit* ctx,
                                    mailbox_element_ptr& ptr) {
  CAF_ASSERT(ctx != nullptr);
  context(ctx);
  if (! is_initialized()) {
    CAF_LOG_DEBUG("initialize actor");
    initialize();
    if (finished()) {
      CAF_LOG_DEBUG("actor_done() returned true right "
                    << "after make_behavior()");
      return;
    }
  }
  if (! has_behavior() || is_terminated()) {
    CAF_LOG_DEBUG_IF(! has_behavior(),
                     "resume called on an actor without behavior");
    CAF_LOG_DEBUG_IF(is_terminated(),
                     "resume called on a terminated actor");
    return;
  }
  try {
    exec_event(ptr);
  }
  catch (...) {
    CAF_LOG_INFO("broker died because of an exception");
    auto eptr = std::current_exception();
    auto opt_reason = this->handle(eptr);
    if (opt_reason)
      quit(*opt_reason);
  }
}

mailbox_element_ptr local_actor::next_message() {
  if (! is_priority_aware())
    return mailbox_element_ptr{mailbox().try_pop()};
  // we partition the mailbox into four segments in this case:
  // <-------- ! was_skipped --------> | <--------  was_skipped -------->
  // <-- high prio --><-- low prio -->|<-- high prio --><-- low prio -->
  auto& cache = mailbox().cache();
  auto i = cache.first_begin();
  auto e = cache.first_end();
  if (i == e || ! i->is_high_priority()) {
    // insert points for high priority
    auto hp_pos = i;
    // read whole mailbox at once
    auto tmp = mailbox().try_pop();
    while (tmp) {
      cache.insert(tmp->is_high_priority() ? hp_pos : e, tmp);
      // adjust high priority insert point on first low prio element insert
      if (hp_pos == e && ! tmp->is_high_priority()) {
        --hp_pos;
      }
      tmp = mailbox().try_pop();
    }
  }
  mailbox_element_ptr result;
  if (! cache.first_empty())
    result.reset(cache.take_first_front());
  return result;
}

bool local_actor::has_next_message() {
  if (! is_priority_aware()) {
    return mailbox_.can_fetch_more();
  }
  auto& mbox = mailbox();
  auto& cache = mbox.cache();
  return ! cache.first_empty() || mbox.can_fetch_more();
}

void local_actor::push_to_cache(mailbox_element_ptr ptr) {
  if (! is_priority_aware()) {
    mailbox().cache().push_second_back(ptr.release());
    return;
  }
  auto high_prio = [](const mailbox_element& val) {
    return val.is_high_priority();
  };
  auto& cache = mailbox().cache();
  auto e = cache.second_end();
  auto i = ptr->is_high_priority()
           ? std::partition_point(cache.second_begin(), e, high_prio)
           : e;
  cache.insert(i, ptr.release());
}

bool local_actor::invoke_from_cache() {
  behavior empty_bhvr;
  auto& bhvr =
    awaits_response() ? awaited_response_handler()
                      : bhvr_stack().empty() ? empty_bhvr
                                             : bhvr_stack().back();
  return invoke_from_cache(bhvr, awaited_response_id());
}

bool local_actor::invoke_from_cache(behavior& bhvr, message_id mid) {
  auto& cache = mailbox().cache();
  auto i = cache.second_begin();
  auto e = cache.second_end();
  CAF_LOG_DEBUG(CAF_ARG(std::distance(i, e)));
  return cache.invoke(this, i, e, bhvr, mid);
}

void local_actor::do_become(behavior bhvr, bool discard_old) {
  if (discard_old) {
    bhvr_stack_.pop_back();
  }
  // request_timeout simply resets the timeout when it's invalid
  request_timeout(bhvr.timeout());
  bhvr_stack_.push_back(std::move(bhvr));
}

void local_actor::send_exit(const actor_addr& whom, error reason) {
  send_exit(actor_cast<strong_actor_ptr>(whom), std::move(reason));
}

void local_actor::send_exit(const strong_actor_ptr& dest, error reason) {
  if (! dest)
    return;
  dest->get()->eq_impl(message_id::make(), nullptr, context(),
                       exit_msg{address(), std::move(reason)});
}

const char* local_actor::name() const {
  return "actor";
}

void local_actor::save_state(serializer&, const unsigned int) {
  throw std::logic_error("local_actor::serialize called");
}

void local_actor::load_state(deserializer&, const unsigned int) {
  throw std::logic_error("local_actor::deserialize called");
}

bool local_actor::finished() {
  if (has_behavior() && ! is_terminated())
    return false;
  CAF_LOG_DEBUG("actor either has no behavior or has set an exit reason");
  on_exit();
  bhvr_stack().clear();
  bhvr_stack().cleanup();
  cleanup(std::move(fail_state_), context());
  return true;
}

bool local_actor::cleanup(error&& fail_state, execution_unit* host) {
  CAF_LOG_TRACE(CAF_ARG(fail_state));
  if (is_detached() && ! is_blocking()) {
    CAF_ASSERT(private_thread_ != nullptr);
    private_thread_->shutdown();
  }
  generators_.clear();
  current_mailbox_element().reset();
  if (! mailbox_.closed()) {
    detail::sync_request_bouncer f{fail_state};
    mailbox_.close(f);
  }
  awaited_responses_.clear();
  multiplexed_responses_.clear();
  auto me = ctrl();
  for (auto& subscription : subscriptions_)
    subscription->unsubscribe(me);
  subscriptions_.clear();
  // tell registry we're done
  is_registered(false);
  monitorable_actor::cleanup(std::move(fail_state), host);
  return true;
}

void local_actor::quit(error x) {
  CAF_LOG_TRACE(CAF_ARG(x));
  fail_state_ = std::move(x);
  is_terminated(true);
  if (is_blocking())
    throw actor_exited(fail_state_);
}

void local_actor::grant_credit(uint64_t newly_available,
                               sources_map::iterator cause) {
  CAF_LOG_TRACE(CAF_ARG(newly_available));
  open_credit_ += newly_available;
//printf("open_credit_: %d, in_flight: %d, low_watermark: %d, sources_: %s\n", (int) open_credit_, (int) in_flight(), (int) low_watermark(), deep_to_string(sources_).c_str());
  bool above_low_watermark = in_flight() > low_watermark();
  // assign new credit to cause if it ran out of credit
  // but we wouldn't assign it new credit otherwise
  if (cause != sources_.end()) {
    cause->second -= newly_available;
    if (cause->second == 0 && above_low_watermark) {
      auto ptr = actor_cast<strong_actor_ptr>(cause->first);
      if (ptr) {
        cause->second = open_credit_;
        ptr->enqueue(mailbox_element::make(ctrl(), message_id::make(),
                                           {}, sys_atom::value,
                                           get_atom::value, open_credit_),
                      context());
        open_credit_ = 0;
      }
      return;
    }
  }
  if (above_low_watermark || sources_.empty())
    return;
  // convert weak pointers to strong ones
  std::vector<actor> src_vec;
  auto src_iter = sources_.begin();
  auto src_end = sources_.end();
  while (src_iter != src_end) {
    auto ptr = actor_cast<strong_actor_ptr>(src_iter->first);
    if (! ptr) {
      open_credit_ += src_iter->second;
      src_iter = sources_.erase(src_iter);
    } else {
      src_vec.emplace_back(actor_cast<actor>(std::move(ptr)));
      ++src_iter;
    }
  }
  // bail out if no source remains
  if (sources_.empty())
    return;
  CAF_ASSERT(src_vec.size() == sources_.size());
  // calculate how much new credit we can hand out per source
  auto credit = open_credit_ / src_vec.size();
  // make sure we advance at least *some* sources if we can't split
  // available credit among all sources
  while (credit == 0) {
    src_vec.pop_back();
    credit = open_credit_ / src_vec.size();
  }
  CAF_LOG_DEBUG("grant more credit to sources"
                << CAF_ARG(credit) << CAF_ARG(src_vec));
//printf("grant more credit to sources: %d %s\n", (int) credit, deep_to_string(src_vec).c_str());
  // iterate both ranges, update open credit per source and send messages
  src_iter = sources_.begin();
  auto i = src_vec.begin();
  while (i != src_vec.end()) {
    CAF_ASSERT(src_iter->first == *i);
    src_iter->second += credit;
    (*i)->enqueue(mailbox_element::make(ctrl(), message_id::make(),
                                        {}, sys_atom::value,
                                        get_atom::value, credit),
                  context());
    ++src_iter;
    ++i;
  }
  open_credit_ -= credit * src_vec.size();
}

} // namespace caf
