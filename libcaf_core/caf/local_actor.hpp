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

#ifndef CAF_LOCAL_ACTOR_HPP
#define CAF_LOCAL_ACTOR_HPP

#include <atomic>
#include <cstdint>
#include <utility>
#include <exception>
#include <functional>
#include <type_traits>
#include <forward_list>
#include <unordered_map>

#include "caf/fwd.hpp"

#include "caf/actor.hpp"
#include "caf/error.hpp"
#include "caf/extend.hpp"
#include "caf/logger.hpp"
#include "caf/message.hpp"
#include "caf/duration.hpp"
#include "caf/behavior.hpp"
#include "caf/delegated.hpp"
#include "caf/resumable.hpp"
#include "caf/actor_cast.hpp"
#include "caf/message_id.hpp"
#include "caf/exit_reason.hpp"
#include "caf/typed_actor.hpp"
#include "caf/actor_config.hpp"
#include "caf/actor_system.hpp"
#include "caf/spawn_options.hpp"
#include "caf/stream_handle.hpp"
#include "caf/abstract_actor.hpp"
#include "caf/abstract_group.hpp"
#include "caf/execution_unit.hpp"
#include "caf/message_handler.hpp"
#include "caf/response_promise.hpp"
#include "caf/message_priority.hpp"
#include "caf/check_typed_input.hpp"
#include "caf/monitorable_actor.hpp"
#include "caf/invoke_message_result.hpp"
#include "caf/typed_response_promise.hpp"

#include "caf/scheduler/abstract_coordinator.hpp"

#include "caf/detail/disposer.hpp"
#include "caf/detail/behavior_stack.hpp"
#include "caf/detail/typed_actor_util.hpp"
#include "caf/detail/single_reader_queue.hpp"
#include "caf/detail/memory_cache_flag_type.hpp"

namespace caf {

namespace detail {

template <class... Ts>
struct make_response_promise_helper {
  using type = typed_response_promise<Ts...>;
};

template <class... Ts>
struct make_response_promise_helper<typed_response_promise<Ts...>>
    : make_response_promise_helper<Ts...> {};

template <>
struct make_response_promise_helper<response_promise> {
  using type = response_promise;
};

} // namespace detail

/// @relates local_actor
/// Default handler function that sends the message back to the sender.
result<message> reflect(local_actor*, const type_erased_tuple*);

/// @relates local_actor
/// Default handler function that sends
/// the message back to the sender and then quits.
result<message> reflect_and_quit(local_actor*, const type_erased_tuple*);

/// @relates local_actor
/// Default handler function that prints messages
/// message via `aout` and drops them afterwards.
result<message> print_and_drop(local_actor*, const type_erased_tuple*);

/// @relates local_actor
/// Default handler function that simply drops messages.
result<message> drop(local_actor*, const type_erased_tuple*);

/// Base class for actors running on this node, either
/// living in an own thread or cooperatively scheduled.
class local_actor : public monitorable_actor, public resumable {
public:
  // -- member types -----------------------------------------------------------

  using mailbox_type = detail::single_reader_queue<mailbox_element,
                                                   detail::disposer>;

  /// Function object for handling unmatched messages.
  using default_handler
    = std::function<result<message> (local_actor* self,
                                     const type_erased_tuple*)>;

  /// Function object for handling error messages.
  using error_handler = std::function<void (local_actor* self, error&)>;

  /// Function object for handling down messages.
  using down_handler = std::function<void (local_actor* self, down_msg&)>;

  /// Function object for handling exit messages.
  using exit_handler = std::function<void (local_actor* self, exit_msg&)>;

  // -- constructors, destructors, and assignment operators --------------------

  ~local_actor();

  void on_destroy() override;

  // -- spawn functions --------------------------------------------------------

  template <class T, spawn_options Os = no_spawn_options, class... Ts>
  typename infer_handle_from_class<T>::type
  spawn(Ts&&... xs) {
    actor_config cfg{context()};
    return eval_opts(Os, system().spawn_class<T, make_unbound(Os)>(cfg, std::forward<Ts>(xs)...));
  }

  template <spawn_options Os = no_spawn_options, class F, class... Ts>
  typename infer_handle_from_fun<F>::type
  spawn(F fun, Ts&&... xs) {
    actor_config cfg{context()};
    return eval_opts(Os, system().spawn_functor<make_unbound(Os)>(cfg, fun, std::forward<Ts>(xs)...));
  }

  template <class T, spawn_options Os = no_spawn_options, class Groups,
            class... Ts>
  actor spawn_in_groups(const Groups& gs, Ts&&... xs) {
    actor_config cfg{context()};
    return eval_opts(Os, system().spawn_in_groups_impl<T, make_unbound(Os)>(cfg, gs.begin(), gs.end(), std::forward<Ts>(xs)...));
  }

  template <class T, spawn_options Os = no_spawn_options, class... Ts>
  actor spawn_in_groups(std::initializer_list<group> gs, Ts&&... xs) {
    actor_config cfg{context()};
    return eval_opts(Os, system().spawn_in_groups_impl<T, make_unbound(Os)>(cfg, gs.begin(), gs.end(), std::forward<Ts>(xs)...));
  }

  template <class T, spawn_options Os = no_spawn_options, class... Ts>
  actor spawn_in_group(const group& grp, Ts&&... xs) {
    actor_config cfg{context()};
    auto first = &grp;
    return eval_opts(Os, system().spawn_in_groups_impl<T, make_unbound(Os)>(cfg, first, first + 1, std::forward<Ts>(xs)...));
  }

  template <spawn_options Os = no_spawn_options, class Groups, class F, class... Ts>
  actor spawn_in_groups(const Groups& gs, F fun, Ts&&... xs) {
    actor_config cfg{context()};
    return eval_opts(Os, system().spawn_in_groups_impl<make_unbound(Os)>(cfg, gs.begin(), gs.end(), fun, std::forward<Ts>(xs)...));
  }

  template <spawn_options Os = no_spawn_options, class F, class... Ts>
  actor spawn_in_groups(std::initializer_list<group> gs, F fun, Ts&&... xs) {
    actor_config cfg{context()};
    return eval_opts(Os, system().spawn_in_groups_impl<make_unbound(Os)>(cfg, gs.begin(), gs.end(), fun, std::forward<Ts>(xs)...));
  }

  template <spawn_options Os = no_spawn_options, class F, class... Ts>
  actor spawn_in_group(const group& grp, F fun, Ts&&... xs) {
    actor_config cfg{context()};
    auto first = &grp;
    return eval_opts(Os, system().spawn_in_groups_impl<make_unbound(Os)>(cfg, first, first + 1, fun, std::forward<Ts>(xs)...));
  }

  // -- sending asynchronous messages ------------------------------------------

  /// Sends an exit message to `dest`.
  void send_exit(const actor_addr& dest, error reason);

  void send_exit(const strong_actor_ptr& dest, error reason);

  /// Sends an exit message to `dest`.
  template <class ActorHandle>
  void send_exit(const ActorHandle& dest, error reason) {
    dest->eq_impl(message_id::make(), nullptr, context(),
                  exit_msg{address(), std::move(reason)});
  }

  // -- miscellaneous actor operations -----------------------------------------

  /// Sets a custom handler for unexpected messages.
  inline void set_default_handler(default_handler fun) {
    default_handler_ = std::move(fun);
  }

  /// Sets a custom handler for error messages.
  inline void set_error_handler(error_handler fun) {
    error_handler_ = std::move(fun);
  }

  /// Sets a custom handler for error messages.
  template <class T>
  auto set_error_handler(T fun) -> decltype(fun(std::declval<error&>())) {
    set_error_handler([fun](local_actor*, error& x) { fun(x); });
  }

  /// Sets a custom handler for down messages.
  inline void set_down_handler(down_handler fun) {
    down_handler_ = std::move(fun);
  }

  /// Sets a custom handler for down messages.
  template <class T>
  auto set_down_handler(T fun) -> decltype(fun(std::declval<down_msg&>())) {
    set_down_handler([fun](local_actor*, down_msg& x) { fun(x); });
  }

  /// Sets a custom handler for error messages.
  inline void set_exit_handler(exit_handler fun) {
    exit_handler_ = std::move(fun);
  }

  /// Sets a custom handler for exit messages.
  template <class T>
  auto set_exit_handler(T fun) -> decltype(fun(std::declval<exit_msg&>())) {
    set_exit_handler([fun](local_actor*, exit_msg& x) { fun(x); });
  }

  /// Returns the execution unit currently used by this actor.
  inline execution_unit* context() const {
    return context_;
  }

  /// Sets the execution unit for this actor.
  inline void context(execution_unit* x) {
    context_ = x;
  }

  /// Returns the hosting actor system.
  inline actor_system& system() const {
    CAF_ASSERT(context_);
    return context_->system();
  }

  /// Causes this actor to subscribe to the group `what`.
  /// The group will be unsubscribed if the actor finishes execution.
  void join(const group& what);

  /// Causes this actor to leave the group `what`.
  void leave(const group& what);

  /// Finishes execution of this actor after any currently running
  /// message handler is done.
  /// This member function clears the behavior stack of the running actor
  /// and invokes `on_exit()`. The actors does not finish execution
  /// if the implementation of `on_exit()` sets a new behavior.
  /// When setting a new behavior in `on_exit()`, one has to make sure
  /// to not produce an infinite recursion.
  ///
  /// If `on_exit()` did not set a new behavior, the actor sends an
  /// exit message to all of its linked actors, sets its state to exited
  /// and finishes execution.
  ///
  /// In case this actor uses the blocking API, this member function unwinds
  /// the stack by throwing an `actor_exited` exception.
  /// @warning This member function throws immediately in thread-based actors
  ///          that do not use the behavior stack, i.e., actors that use
  ///          blocking API calls such as {@link receive()}.
  void quit(error reason = error{});

  /// @cond PRIVATE

  void monitor(abstract_actor* whom);

  /// @endcond

  /// Returns a pointer to the sender of the current message.
  inline strong_actor_ptr current_sender() {
    return current_element_ ? current_element_->sender : nullptr;
  }

  /// Adds a unidirectional `monitor` to `whom`.
  /// @note Each call to `monitor` creates a new, independent monitor.
  template <class Handle>
  void monitor(const Handle& whom) {
    monitor(actor_cast<abstract_actor*>(whom));
  }

  /// Removes a monitor from `whom`.
  void demonitor(const actor_addr& whom);

  /// Removes a monitor from `whom`.
  inline void demonitor(const actor& whom) {
    demonitor(whom.address());
  }

  /// Can be overridden to perform cleanup code after an actor
  /// finished execution.
  virtual void on_exit();

  /// Returns all joined groups.
  std::vector<group> joined_groups() const;

  /// Creates a `typed_response_promise` to respond to a request later on.
  /// `make_response_promise<typed_response_promise<int, int>>()`
  /// is equivalent to `make_response_promise<int, int>()`.
  template <class... Ts>
  typename detail::make_response_promise_helper<Ts...>::type
  make_response_promise() {
    auto& ptr = current_element_;
    if (! ptr)
      return {};
    auto& mid = ptr->mid;
    if (mid.is_answered())
      return {};
    return {this, *ptr};
  }

  /// Creates a `response_promise` to respond to a request later on.
  inline response_promise make_response_promise() {
    return make_response_promise<response_promise>();
  }

  /// Creates a `typed_response_promise` and responds immediately.
  /// Return type is deduced from arguments.
  /// Return value is implicitly convertible to untyped response promise.
  template <class... Ts,
            class R =
              typename detail::make_response_promise_helper<
                typename std::decay<Ts>::type...
              >::type>
  R response(Ts&&... xs) {
    auto promise = make_response_promise<R>();
    promise.deliver(std::forward<Ts>(xs)...);
    return promise;
  }

  /// Sets a custom exception handler for this actor. If multiple handlers are
  /// defined, only the functor that was added *last* is being executed.
  template <class F>
  void set_exception_handler(F f) {
    struct functor_attachable : attachable {
      F functor_;
      functor_attachable(F arg) : functor_(std::move(arg)) {
        // nop
      }
      optional<exit_reason> handle_exception(const std::exception_ptr& eptr) {
        return functor_(eptr);
      }
    };
    attach(attachable_ptr{new functor_attachable(std::move(f))});
  }

  const char* name() const override;

  /// Serializes the state of this actor to `sink`. This function is
  /// only called if this actor has set the `is_serializable` flag.
  /// The default implementation throws a `std::logic_error`.
  virtual void save_state(serializer& sink, const unsigned int version);

  /// Deserializes the state of this actor from `source`. This function is
  /// only called if this actor has set the `is_serializable` flag.
  /// The default implementation throws a `std::logic_error`.
  virtual void load_state(deserializer& source, const unsigned int version);

  // -- overridden member functions of resumable -------------------------------

  subtype_t subtype() const override;

  resume_result resume(execution_unit*, size_t) override;

  // -- flow control messaging -------------------------------------------------

  template <class F>
  stream_handle new_stream(actor sink, F generator) {
    if (generators_.count(sink) > 0) {
      CAF_LOG_WARNING("multiple new_stream calls for the same sink");
      return {};
    }
    auto f = [=]() -> bool {
      auto x = generator();
      static_assert(is_result<decltype(x)>::value,
                    "Generator function must return a `result<Ts...>`");
      if (x.value.empty())
        return false;
      auto mid = message_id::from_integer_value(message_id::flow_controlled_flag_mask);
      sink->enqueue(mailbox_element::make(ctrl(), mid, {}, std::move(x.value)),
                    context());
      return true;
    };
    generators_.emplace(sink, std::make_pair(std::move(f), ctrl()));
    sink->enqueue(mailbox_element::make(ctrl(), message_id::make(), {},
                                        sys_atom::value, add_source_atom::value),
                  context());
    return {this, sink};
  }

  // -- here be dragons: end of public interface -------------------------------

  /// @cond PRIVATE

  // handle `ptr` in an event-based actor
  std::pair<resumable::resume_result, invoke_message_result>
  exec_event(mailbox_element_ptr& ptr);

  // handle `ptr` in an event-based actor, not suitable to be called in a loop
  virtual void exec_single_event(execution_unit* ctx, mailbox_element_ptr& ptr);

  local_actor(actor_config& sys);

  void intrusive_ptr_add_ref_impl() override;

  void intrusive_ptr_release_impl() override;

  template <class ActorHandle>
  inline ActorHandle eval_opts(spawn_options opts, ActorHandle res) {
    if (has_monitor_flag(opts)) {
      monitor(res->address());
    }
    if (has_link_flag(opts)) {
      link_to(res->address());
    }
    return res;
  }

  inline mailbox_element_ptr& current_mailbox_element() {
    return current_element_;
  }

  void request_sync_timeout_msg(const duration& dr, message_id mid);

  // returns 0 if last_dequeued() is an asynchronous or sync request message,
  // a response id generated from the request id otherwise
  inline message_id get_response_id() const {
    auto mid = current_element_->mid;
    return (mid.is_request()) ? mid.response_id() : message_id();
  }

  template <message_priority P = message_priority::normal,
            class Handle = actor, class... Ts>
  typename detail::deduce_output_type<
    Handle,
    detail::type_list<
      typename detail::implicit_conversions<
        typename std::decay<Ts>::type
      >::type...
    >
  >::delegated_type
  delegate(const Handle& dest, Ts&&... xs) {
    static_assert(sizeof...(Ts) > 0, "nothing to delegate");
    using token =
      detail::type_list<
        typename detail::implicit_conversions<
          typename std::decay<Ts>::type
        >::type...>;
    static_assert(actor_accepts_message<typename signatures_of<Handle>::type, token>::value,
                  "receiver does not accept given message");
    auto mid = current_element_->mid;
    current_element_->mid = P == message_priority::high
                            ? mid.with_high_priority()
                            : mid.with_normal_priority();
    // make sure our current message is not
    // destroyed before the end of the scope
    auto next = make_message(std::forward<Ts>(xs)...);
    next.swap(current_element_->msg);
    dest->enqueue(std::move(current_element_), context());
    return {};
  }

  inline detail::behavior_stack& bhvr_stack() {
    return bhvr_stack_;
  }

  inline mailbox_type& mailbox() {
    return mailbox_;
  }

  inline bool has_behavior() const {
    return ! bhvr_stack_.empty()
           || ! awaited_responses_.empty()
           || ! multiplexed_responses_.empty();
  }

  virtual void initialize() = 0;

  // clear behavior stack and call cleanup if actor either has no
  // valid behavior left or has set a planned exit reason
  bool finished();

  bool cleanup(error&& reason, execution_unit* host) override;

  // an actor can have multiple pending timeouts, but only
  // the latest one is active (i.e. the pending_timeouts_.back())

  uint32_t request_timeout(const duration& d);

  void handle_timeout(behavior& bhvr, uint32_t timeout_id);

  void reset_timeout(uint32_t timeout_id);

  // @pre has_timeout()
  bool is_active_timeout(uint32_t tid) const;

  // @pre has_timeout()
  uint32_t active_timeout_id() const;

  invoke_message_result invoke_message(mailbox_element_ptr& node,
                                       behavior& fun,
                                       message_id awaited_response);

  using pending_response = std::pair<const message_id, behavior>;

  message_id new_request_id(message_priority mp);

  void mark_awaited_arrived(message_id mid);

  bool awaits_response() const;

  bool awaits(message_id mid) const;

  pending_response* find_awaited_response(message_id mid);

  void set_awaited_response_handler(message_id response_id, behavior bhvr);

  behavior& awaited_response_handler();

  message_id awaited_response_id();

  void mark_multiplexed_arrived(message_id mid);

  bool multiplexes(message_id mid) const;

  pending_response* find_multiplexed_response(message_id mid);

  void set_multiplexed_response_handler(message_id response_id, behavior bhvr);

  // these functions are dispatched via the actor policies table

  void launch(execution_unit* eu, bool lazy, bool hide);

  using abstract_actor::enqueue;

  void enqueue(mailbox_element_ptr, execution_unit*) override;

  mailbox_element_ptr next_message();

  bool has_next_message();

  void push_to_cache(mailbox_element_ptr);

  bool invoke_from_cache();

  bool invoke_from_cache(behavior&, message_id);

  void do_become(behavior bhvr, bool discard_old);

  /// Returns the maximum credit per source.
  uint64_t max_credit_per_source() const {
    return max_credit_ / sources_.size();
  }

  /// Returns how many messages are currently assumbed to be in-flight.
  uint64_t in_flight() const {
    return max_credit_ - open_credit_;
  }

  /// Denotes at which point an actors grants more credit to its sources
  /// in order to receive more work items.
  uint64_t low_watermark() const {
    return low_watermark_;
  }

  // Stores registered sources.
  using sources_map = std::unordered_map<actor_addr, uint64_t>;

  /// Allows sources to send more work item if low watermark is reached
  /// or if `cause` ran out of credit.
  virtual void grant_credit(uint64_t newly_available,
                            sources_map::iterator cause);

protected:
  // used by both event-based and blocking actors
  mailbox_type mailbox_;

  // identifies the execution unit this actor is currently executed by
  execution_unit* context_;

  // identifies the ID of the last sent synchronous request
  message_id last_request_id_;

  // identifies all IDs of sync messages waiting for a response
  std::forward_list<pending_response> awaited_responses_;

  // identifies all IDs of async messages waiting for a response
  std::unordered_map<message_id, behavior> multiplexed_responses_;

  // points to dummy_node_ if no callback is currently invoked,
  // points to the node under processing otherwise
  mailbox_element_ptr current_element_;

  // identifies the timeout messages we are currently waiting for
  uint32_t timeout_id_;

  // used by both event-based and blocking actors
  detail::behavior_stack bhvr_stack_;

  // used by functor-based actors to implemented make_behavior() or act()
  std::function<behavior (local_actor*)> initial_behavior_fac_;

  // used for group management
  std::set<group> subscriptions_;

  // used for setting custom functions for handling unexpected messages
  default_handler default_handler_;

  // used for setting custom error handlers
  error_handler error_handler_;

  // used for setting custom down message handlers
  down_handler down_handler_;

  // used for setting custom exit message handlers
  exit_handler exit_handler_;

  // Unassigned credit.
  uint64_t open_credit_;

  // Threshold for demanding more work.
  uint64_t low_watermark_;

  // Maximum number of allowed "pending" messages.
  uint64_t max_credit_;

  // Registered sources.
  sources_map sources_;

  using generator_function = std::function<bool ()>;

  using generators_value = std::pair<generator_function, strong_actor_ptr>;

  // Generator functions of open Streams, the second mapped value is
  // a strong pointer to `this` in order to keep this actor alive as long
  // as it has at least one open stream.
  std::unordered_map<actor, generators_value> generators_;

  /// @endcond

private:
  enum class msg_type {
    expired_timeout,       // an 'old & obsolete' timeout
    timeout,               // triggers currently active timeout
    ordinary,              // an asynchronous message or sync. request
    response,              // a response
    sys_message            // a system message, e.g., exit_msg or down_msg
  };

  msg_type filter_msg(mailbox_element& node);

  void handle_response(mailbox_element_ptr&, local_actor::pending_response&);

  class private_thread;

  private_thread* private_thread_;
};

/// A smart pointer to a {@link local_actor} instance.
/// @relates local_actor
using local_actor_ptr = intrusive_ptr<local_actor>;

} // namespace caf

#endif // CAF_LOCAL_ACTOR_HPP
