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

#ifndef CAF_ACTOR_SYSTEM_CONFIG_HPP
#define CAF_ACTOR_SYSTEM_CONFIG_HPP

#include <atomic>
#include <string>
#include <memory>
#include <typeindex>
#include <functional>
#include <type_traits>
#include <unordered_map>

#include "caf/fwd.hpp"
#include "caf/config_value.hpp"
#include "caf/config_option.hpp"
#include "caf/actor_factory.hpp"
#include "caf/is_typed_actor.hpp"
#include "caf/type_erased_value.hpp"
#include "caf/named_actor_config.hpp"

#include "caf/detail/safe_equal.hpp"
#include "caf/detail/type_traits.hpp"

namespace caf {

/// Configures an `actor_system` on startup.
class actor_system_config {
public:
  friend class actor_system;

  template <class K, class V>
  using hash_map = std::unordered_map<K, V>;

  using module_factory = std::function<actor_system::module* (actor_system&)>;

  using module_factories = std::vector<module_factory>;

  using value_factory = std::function<type_erased_value_ptr ()>;

  using value_factories_by_name = hash_map<std::string, value_factory>;

  using value_factories_by_rtti = hash_map<std::type_index, value_factory>;

  using actor_factories = hash_map<std::string, actor_factory>;

  using portable_names = hash_map<std::type_index, std::string>;

  using error_renderer = std::function<std::string (uint8_t, atom_value, const message&)>;

  using error_renderers = hash_map<atom_value, error_renderer>;

  using option_ptr = std::unique_ptr<config_option>;

  using options_vector = std::vector<option_ptr>;

  using named_actor_config_map = hash_map<std::string, named_actor_config>;

  class opt_group {
  public:
    opt_group(options_vector& xs, const char* category);

    template <class T>
    opt_group& add(T& storage, const char* name, const char* explanation) {
      xs_.emplace_back(make_config_option(storage, cat_, name, explanation));
      return *this;
    }

  private:
    options_vector& xs_;
    const char* cat_;
  };

  virtual ~actor_system_config();

  actor_system_config();

  actor_system_config(const actor_system_config&) = delete;
  actor_system_config& operator=(const actor_system_config&) = delete;

  actor_system_config& parse(int argc, char** argv,
                             const char* config_file_name = nullptr);

  /// Allows other nodes to spawn actors created by `fun`
  /// dynamically by using `name` as identifier.
  /// @experimental
  actor_system_config& add_actor_factory(std::string name, actor_factory fun);

  /// Allows other nodes to spawn actors of type `T`
  /// dynamically by using `name` as identifier.
  /// @experimental
  template <class T, class... Ts>
  actor_system_config& add_actor_type(std::string name) {
    return add_actor_factory(std::move(name), make_actor_factory<T, Ts...>());
  }

  /// Allows other nodes to spawn actors implemented by function `f`
  /// dynamically by using `name` as identifier.
  /// @experimental
  template <class F>
  actor_system_config& add_actor_type(std::string name, F f) {
    return add_actor_factory(std::move(name), make_actor_factory(std::move(f)));
  }

  /// Adds message type `T` with runtime type info `name`.
  template <class T>
  actor_system_config& add_message_type(std::string name) {
    static_assert(std::is_empty<T>::value
                  || is_typed_actor<T>::value
                  || (std::is_default_constructible<T>::value
                      && std::is_copy_constructible<T>::value),
                  "T must provide default and copy constructors");
    static_assert(detail::is_serializable<T>::value, "T must be serializable");
    type_names_by_rtti_.emplace(std::type_index(typeid(T)), name);
    value_factories_by_name_.emplace(std::move(name), &make_type_erased_value<T>);
    value_factories_by_rtti_.emplace(std::type_index(typeid(T)),
                                     &make_type_erased_value<T>);
    return *this;
  }

  /// Enables the actor system to convert errors of this error category
  /// to human-readable strings via `renderer`.
  actor_system_config& add_error_category(atom_value category,
                                          error_renderer renderer);

  /// Enables the actor system to convert errors of this error category
  /// to human-readable strings via `to_string(T)`.
  template <class T>
  actor_system_config& add_error_category(atom_value category) {
    auto f = [=](uint8_t val, const std::string& ctx) -> std::string {
      std::string result;
      result = to_string(category);
      result += ": ";
      result += to_string(static_cast<T>(val));
      if (! ctx.empty()) {
        result += " (";
        result += ctx;
        result += ")";
      }
      return result;
    };
    return add_error_category(category, f);
  }

  /// Loads module `T` with optional template parameters `Ts...`.
  template <class T, class... Ts>
  actor_system_config& load() {
    module_factories_.push_back([](actor_system& sys) -> actor_system::module* {
      return T::make(sys, detail::type_list<Ts...>{});
    });
    return *this;
  }

  /// Stores whether the help text for this config object was
  /// printed. If set to `true`, the application should not use
  /// this config object to initialize an `actor_system` and
  /// return from `main` immediately.
  bool cli_helptext_printed;

  /// Stores whether this node was started in slave mode.
  bool slave_mode;

  /// Stores the name of this node when started in slave mode.
  std::string slave_name;

  /// Stores credentials for connecting to the bootstrap node
  /// when using the caf-run launcher.
  std::string bootstrap_node;

  /// Stores CLI arguments that were not consumed by CAF.
  message args_remainder;

  /// Sets a config by using its INI name `config_name` to `config_value`.
  actor_system_config& set(const char* config_name, config_value config_value);

  // Config parameters of scheduler.
  atom_value scheduler_policy;
  size_t scheduler_max_threads;
  size_t scheduler_max_throughput;
  bool scheduler_enable_profiling;
  size_t scheduler_profiling_ms_resolution;
  std::string scheduler_profiling_output_file;

  // Config parameters of middleman.
  atom_value middleman_network_backend;
  bool middleman_enable_automatic_connections;
  size_t middleman_max_consecutive_reads;
  size_t middleman_heartbeat_interval;

  // Config parameters of RIAC probes.
  std::string nexus_host;
  uint16_t nexus_port;

  // Config parameters of the OpenCL module.
  std::string opencl_device_ids;

  // System parameters that are set while initializing modules.
  node_id network_id;
  proxy_registry* network_proxies;

  // Config parameter for individual actor types.
  named_actor_config_map named_actor_configs;

  int (*slave_mode_fun)(actor_system&, const actor_system_config&);

protected:
  virtual std::string make_help_text(const std::vector<message::cli_arg>&);

  options_vector custom_options_;

private:
  static std::string render_sec(uint8_t, atom_value, const message&);

  static std::string render_exit_reason(uint8_t, atom_value, const message&);

  value_factories_by_name value_factories_by_name_;
  value_factories_by_rtti value_factories_by_rtti_;
  portable_names type_names_by_rtti_;
  actor_factories actor_factories_;
  module_factories module_factories_;
  error_renderers error_renderers_;
  options_vector options_;
};

} // namespace caf

#endif //CAF_ACTOR_SYSTEM_CONFIG_HPP
