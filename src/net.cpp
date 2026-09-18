// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/net.hpp"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace bandwidth_broker {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket kInvalidSocket = INVALID_SOCKET;
#else
using native_socket = int;
constexpr native_socket kInvalidSocket = -1;
#endif

// Returns an Error rather than a Status so that it can be returned directly
// from functions whose result type carries a value.
[[nodiscard]] Error last_socket_error(const char* what) {
#if defined(_WIN32)
  const int code = WSAGetLastError();
  return Error(ErrorCode::IoError, std::string(what) + " failed with socket error " + std::to_string(code));
#else
  return Error(ErrorCode::IoError, std::string(what) + " failed");
#endif
}

void close_native(native_socket handle) noexcept {
#if defined(_WIN32)
  ::closesocket(handle);
#else
  ::close(handle);
#endif
}

}  // namespace

Status initialise_networking() {
#if defined(_WIN32)
  static const Status result = [] {
    WSADATA data{};
    const int status = WSAStartup(MAKEWORD(2, 2), &data);
    if (status != 0) {
      return make_error_status(ErrorCode::IoError, "WSAStartup failed");
    }
    return Status::success();
  }();
  return result;
#else
  return Status::success();
#endif
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

Socket::~Socket() { close(); }

void Socket::close() noexcept {
  if (handle_ != -1) {
    close_native(static_cast<native_socket>(handle_));
    handle_ = -1;
  }
}

bool Socket::valid() const noexcept { return handle_ != -1; }

void Socket::disable_nagle() noexcept {
#if defined(_WIN32)
  const char enabled = 1;
  ::setsockopt(static_cast<native_socket>(handle_), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#else
  const int enabled = 1;
  ::setsockopt(static_cast<native_socket>(handle_), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#endif
}

Result<Socket> Socket::connect_to(const std::string& host, std::uint16_t port) {
  BB_RETURN_IF_ERROR(initialise_networking());
  if (host.empty() || host.size() > 253) {
    return make_error<Socket>(ErrorCode::InvalidArgument, "host must be a non-empty name or address");
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
    return make_error<Socket>(ErrorCode::IoError, "cannot resolve the coordinator address");
  }
  Socket socket;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    const native_socket handle =
        ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocket) {
      continue;
    }
    if (::connect(handle, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
      socket.handle_ = static_cast<std::intptr_t>(handle);
      break;
    }
    close_native(handle);
  }
  ::freeaddrinfo(results);
  if (!socket.valid()) {
    return make_error<Socket>(ErrorCode::IoError, "cannot connect to the coordinator");
  }
  socket.disable_nagle();
  return socket;
}

Status Socket::send_all(const std::uint8_t* data, std::size_t length) {
  if (!valid()) {
    return make_error_status(ErrorCode::ConnectionClosed, "socket is not connected");
  }
  std::size_t sent = 0;
  while (sent < length) {
    const int chunk = static_cast<int>(length - sent > 1u << 20 ? 1u << 20 : length - sent);
    const int written = ::send(static_cast<native_socket>(handle_),
                               reinterpret_cast<const char*>(data + sent), chunk, 0);
    if (written <= 0) {
      return last_socket_error("send");
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::success();
}

Result<std::size_t> Socket::receive_some(std::uint8_t* buffer, std::size_t capacity) {
  if (!valid()) {
    return make_error<std::size_t>(ErrorCode::ConnectionClosed, "socket is not connected");
  }
  if (capacity == 0) {
    return std::size_t{0};
  }
  const int chunk = static_cast<int>(capacity > 1u << 20 ? 1u << 20 : capacity);
  const int read = ::recv(static_cast<native_socket>(handle_), reinterpret_cast<char*>(buffer), chunk, 0);
  if (read == 0) {
    return std::size_t{0};
  }
  if (read < 0) {
    return last_socket_error("recv");
  }
  return static_cast<std::size_t>(read);
}

Result<bool> Socket::wait_readable(int poll_milliseconds) const {
  if (!valid()) {
    return make_error<bool>(ErrorCode::ConnectionClosed, "socket is not connected");
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(static_cast<native_socket>(handle_), &read_set);
  timeval interval{};
  interval.tv_sec = poll_milliseconds / 1000;
  interval.tv_usec = (poll_milliseconds % 1000) * 1000;
  const int ready = ::select(static_cast<int>(handle_) + 1, &read_set, nullptr, nullptr, &interval);
  if (ready < 0) {
    return last_socket_error("select");
  }
  return ready > 0;
}

Listener::Listener(Listener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
  other.handle_ = -1;
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = -1;
    other.port_ = 0;
  }
  return *this;
}

Listener::~Listener() { close(); }

void Listener::close() noexcept {
  if (handle_ != -1) {
    close_native(static_cast<native_socket>(handle_));
    handle_ = -1;
  }
}

bool Listener::valid() const noexcept { return handle_ != -1; }

Result<Listener> Listener::bind_loopback(std::uint16_t port, std::size_t backlog) {
  BB_RETURN_IF_ERROR(initialise_networking());
  if (backlog == 0 || backlog > 1024) {
    return make_error<Listener>(ErrorCode::InvalidArgument, "listen backlog must be between 1 and 1024");
  }
  const native_socket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (handle == kInvalidSocket) {
    return make_error<Listener>(ErrorCode::IoError, "cannot create a listening socket");
  }
  const char reuse = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(handle);
    return make_error<Listener>(ErrorCode::IoError, "cannot bind the listening socket");
  }
  if (::listen(handle, static_cast<int>(backlog)) != 0) {
    close_native(handle);
    return make_error<Listener>(ErrorCode::IoError, "cannot listen on the bound socket");
  }
  sockaddr_in bound{};
#if defined(_WIN32)
  int length = sizeof(bound);
#else
  socklen_t length = sizeof(bound);
#endif
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    close_native(handle);
    return make_error<Listener>(ErrorCode::IoError, "cannot read the bound port");
  }
  Listener listener(static_cast<std::intptr_t>(handle));
  listener.port_ = ntohs(bound.sin_port);
  return listener;
}

Result<Socket> Listener::accept_one(int poll_milliseconds) {
  if (!valid()) {
    return make_error<Socket>(ErrorCode::ConnectionClosed, "listener is closed");
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(static_cast<native_socket>(handle_), &read_set);
  timeval interval{};
  interval.tv_sec = poll_milliseconds / 1000;
  interval.tv_usec = (poll_milliseconds % 1000) * 1000;
  const int ready = ::select(static_cast<int>(handle_) + 1, &read_set, nullptr, nullptr, &interval);
  if (ready < 0) {
    return last_socket_error("select");
  }
  if (ready == 0) {
    return make_error<Socket>(ErrorCode::NotFound, "no connection is waiting");
  }
  sockaddr_in peer{};
#if defined(_WIN32)
  int length = sizeof(peer);
#else
  socklen_t length = sizeof(peer);
#endif
  const native_socket accepted = ::accept(static_cast<native_socket>(handle_), reinterpret_cast<sockaddr*>(&peer), &length);
  if (accepted == kInvalidSocket) {
    return last_socket_error("accept");
  }
  Socket socket(static_cast<std::intptr_t>(accepted));
  socket.disable_nagle();
  return socket;
}

}  // namespace bandwidth_broker
