// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Coordinator server.
//
// Real framed TCP over real OS processes. Every connection is handled by its
// own thread with bounded buffers and a bounded connection count. The server
// invokes no user callback, takes no broker lock of its own, and never calls
// into the broker while holding its own state lock.

#ifndef BANDWIDTH_BROKER_SERVER_HPP
#define BANDWIDTH_BROKER_SERVER_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "bandwidth_broker/broker.hpp"
#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/net.hpp"

namespace bandwidth_broker {

struct BB_API ServerOptions final {
  // Zero asks the platform for a free port.
  std::uint16_t port{0};
  // Shared bearer token required in the handshake. Empty disables the check;
  // the transport is not encrypted and this is not a substitute for mTLS.
  std::string session_token;
  std::size_t max_connections{limits::kMaxConnections};
  // Bounded per-connection receive buffer.
  std::size_t receive_buffer_bytes{64 * 1024};
};

class BB_API CoordinatorServer final {
 public:
  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;
  ~CoordinatorServer();

  [[nodiscard]] static Result<std::unique_ptr<CoordinatorServer>> start(Broker& broker,
                                                                       const ServerOptions& options);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::size_t active_connections() const;
  [[nodiscard]] std::uint64_t handled_messages() const;

  // Stops accepting, signals every connection to finish, joins every thread and
  // returns once no handler is running. Safe to call more than once.
  void stop();

  // Implementation state. Public so that the translation unit's free functions
  // can take it directly; it is never part of the API.
  struct Impl;

 private:
  CoordinatorServer();
  std::shared_ptr<Impl> impl_;
  std::uint16_t port_{0};
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_SERVER_HPP
