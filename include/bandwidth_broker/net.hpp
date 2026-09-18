// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal portable TCP transport.
//
// This is deliberately small: real sockets, real framed bytes, bounded buffers.
// It is not a general networking library and it does not absorb any
// responsibility belonging to an adjacent runtime.

#ifndef BANDWIDTH_BROKER_NET_HPP
#define BANDWIDTH_BROKER_NET_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/export.hpp"

namespace bandwidth_broker {

// Initialises the platform networking stack. Safe to call repeatedly.
[[nodiscard]] BB_API Status initialise_networking();

class BB_API Socket final {
 public:
  Socket() noexcept = default;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  ~Socket();

  [[nodiscard]] static Result<Socket> connect_to(const std::string& host, std::uint16_t port);

  [[nodiscard]] Status send_all(const std::uint8_t* data, std::size_t length);
  // Returns the number of bytes read; zero means the peer closed cleanly.
  [[nodiscard]] Result<std::size_t> receive_some(std::uint8_t* buffer, std::size_t capacity);

  // Waits up to \p poll_milliseconds for readability. Used to make a receive
  // loop cancellable without ever imposing a timeout on user-visible work.
  [[nodiscard]] Result<bool> wait_readable(int poll_milliseconds) const;

  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;
  void disable_nagle() noexcept;

 private:
  explicit Socket(std::intptr_t handle) noexcept : handle_(handle) {}
  std::intptr_t handle_{-1};
  friend class Listener;
};

class BB_API Listener final {
 public:
  Listener() noexcept = default;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  ~Listener();

  // Binds the loopback interface. Port 0 asks the platform for a free port,
  // which is what the multiprocess tests use so they never collide.
  [[nodiscard]] static Result<Listener> bind_loopback(std::uint16_t port, std::size_t backlog);

  // Accepts at most one connection. Returns ErrorCode::NotFound when the poll
  // interval elapsed without a connection, so the caller can re-check its
  // shutdown flag.
  [[nodiscard]] Result<Socket> accept_one(int poll_milliseconds);

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;

 private:
  explicit Listener(std::intptr_t handle) noexcept : handle_(handle) {}
  std::intptr_t handle_{-1};
  std::uint16_t port_{0};
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_NET_HPP
