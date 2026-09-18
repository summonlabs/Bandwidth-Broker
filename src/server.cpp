// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/server.hpp"

#include <atomic>
#include "bandwidth_broker/wire.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace bandwidth_broker {
namespace {

constexpr int kPollMilliseconds = 20;

struct Connection final {
  Socket socket;
  SessionId session{};
  PublisherId publisher{};
  BootId boot{};
  bool greeted{false};
};

// Sends one framed response. Failures are reported to the caller, which then
// closes the connection: a partially written frame is never retried.
[[nodiscard]] Status send_message(Socket& socket, MessageType type, std::uint64_t correlation,
                                  const std::vector<std::uint8_t>& payload) {
  const auto frame = encode_frame(type, correlation, payload);
  if (!frame.ok()) {
    return frame.error();
  }
  return socket.send_all(frame.value().data(), frame.value().size());
}

template <typename Ack>
[[nodiscard]] Status send_ack(Socket& socket, MessageType type, std::uint64_t correlation,
                              const Ack& ack) {
  const auto payload = encode_payload(ack);
  if (!payload.ok()) {
    return payload.error();
  }
  return send_message(socket, type, correlation, payload.value());
}

[[nodiscard]] Status send_error(Socket& socket, std::uint64_t correlation, ErrorCode code,
                                const std::string& message) {
  ErrorResponse response;
  response.status.code = code;
  response.status.message = message;
  return send_ack(socket, MessageType::ErrorResponse, correlation, response);
}

}  // namespace

struct CoordinatorServer::Impl final {
  Broker* broker{nullptr};
  ServerOptions options;
  Listener listener;
  mutable std::mutex mutex;
  std::condition_variable idle;
  std::size_t active{0};
  std::atomic<bool> stopping{false};
  std::atomic<std::uint64_t> handled{0};
  std::thread acceptor;
};

CoordinatorServer::CoordinatorServer() : impl_(std::make_shared<Impl>()) {}

CoordinatorServer::~CoordinatorServer() { stop(); }

namespace {

// Handles exactly one connection until it closes or the server stops. All
// broker calls happen outside any server lock.
void serve_connection(const std::shared_ptr<CoordinatorServer::Impl>& server, std::unique_ptr<Connection> connection) {
  std::vector<std::uint8_t> buffer(server->options.receive_buffer_bytes);
  FrameDecoder decoder;
  bool greeted = false;

  const auto finish = [&]() {
    connection->socket.close();
    std::lock_guard<std::mutex> guard(server->mutex);
    if (server->active > 0) {
      --server->active;
    }
    server->idle.notify_all();
  };

  while (!server->stopping.load()) {
    const auto readable = connection->socket.wait_readable(kPollMilliseconds);
    if (!readable.ok()) {
      finish();
      return;
    }
    if (!readable.value()) {
      continue;
    }
    const auto received = connection->socket.receive_some(buffer.data(), buffer.size());
    if (!received.ok() || received.value() == 0) {
      finish();
      return;
    }
    const auto fed = decoder.feed(buffer.data(), received.value());
    if (!fed.ok()) {
      (void)send_error(connection->socket, 0, fed.code(), fed.message());
      finish();
      return;
    }

    bool keep_going = true;
    while (keep_going) {
      MessageType type = MessageType::ErrorResponse;
      std::uint64_t correlation = 0;
      std::vector<std::uint8_t> payload;
      const auto next = decoder.next(type, correlation, payload);
      if (!next.ok()) {
        (void)send_error(connection->socket, correlation, next.code(), next.error().message);
        finish();
        return;
      }
      if (!next.value()) {
        break;
      }
      server->handled.fetch_add(1);
      Reader reader(payload);

      if (!greeted && type != MessageType::Hello) {
        (void)send_error(connection->socket, correlation, ErrorCode::HandshakeRejected,
                         "the first frame on a connection must be a handshake");
        keep_going = false;
        break;
      }

      switch (type) {
        case MessageType::Hello: {
          const auto request = decode_hello_request(reader);
          HelloAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
            (void)send_ack(connection->socket, MessageType::HelloAck, correlation, ack);
            keep_going = false;
            break;
          }
          if (request.value().protocol_version != static_cast<std::uint16_t>(BB_PROTOCOL_VERSION)) {
            ack.status.code = ErrorCode::ProtocolVersionUnsupported;
            ack.status.message = "protocol version is not supported by this coordinator";
            (void)send_ack(connection->socket, MessageType::HelloAck, correlation, ack);
            keep_going = false;
            break;
          }
          if (!server->options.session_token.empty() &&
              request.value().session_token != server->options.session_token) {
            ack.status.code = ErrorCode::HandshakeRejected;
            ack.status.message = "session token rejected";
            (void)send_ack(connection->socket, MessageType::HelloAck, correlation, ack);
            keep_going = false;
            break;
          }
          if (!request.value().publisher.valid() || !request.value().boot.valid()) {
            ack.status.code = ErrorCode::InvalidIdentity;
            ack.status.message = "handshake requires a publisher id and a boot identity";
            (void)send_ack(connection->socket, MessageType::HelloAck, correlation, ack);
            keep_going = false;
            break;
          }
          const auto fenced = server->broker->is_fenced(request.value().publisher, request.value().boot);
          if (fenced.ok() && fenced.value()) {
            ack.status.code = ErrorCode::BootFenced;
            ack.status.message = "this boot identity is permanently fenced";
            (void)send_ack(connection->socket, MessageType::HelloAck, correlation, ack);
            keep_going = false;
            break;
          }
          const CoordinatorStatus status = server->broker->status();
          connection->publisher = request.value().publisher;
          connection->boot = request.value().boot;
          connection->greeted = true;
          greeted = true;
          const auto session = SessionId::make(correlation + 1);
          connection->session = session.ok() ? session.value() : SessionId::from_value(1);
          ack.status.code = ErrorCode::Ok;
          ack.status.message = "accepted";
          ack.coordinator = status.incarnation;
          ack.epoch = status.epoch;
          ack.session = connection->session;
          ack.next_grant_id = 1;
          if (!send_ack(connection->socket, MessageType::HelloAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::PublishCapacity: {
          const auto request = decode_publish_capacity_request(reader);
          PublishCapacityAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto applied = server->broker->publish_capacity(request.value().snapshot);
            ack.status.code = applied.code();
            ack.status.message = applied.ok() ? "published" : applied.message();
            ack.generation = request.value().snapshot.generation;
          }
          if (!send_ack(connection->socket, MessageType::PublishCapacityAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::SetPolicy: {
          const auto request = decode_set_policy_request(reader);
          SetPolicyAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto applied = server->broker->set_policy(request.value().policy);
            ack.status.code = applied.code();
            ack.status.message = applied.ok() ? "installed" : applied.message();
            ack.generation = request.value().policy.generation;
          }
          if (!send_ack(connection->socket, MessageType::SetPolicyAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::SubmitRequests: {
          const auto request = decode_submit_requests_request(reader);
          SubmitRequestsAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            for (const BandwidthRequest& entry : request.value().requests) {
              const auto outcome = server->broker->submit(entry);
              if (outcome.ok()) {
                ack.accepted += 1;
              } else {
                ack.refused.push_back(entry.id);
              }
            }
            ack.status.code = ErrorCode::Ok;
            ack.status.message = "submitted";
          }
          if (!send_ack(connection->socket, MessageType::SubmitRequestsAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::Arbitrate: {
          const auto request = decode_arbitrate_request(reader);
          ArbitrateAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto round = server->broker->arbitrate(request.value().target);
            if (!round.ok()) {
              ack.status.code = round.code();
              ack.status.message = round.error().message;
            } else {
              ack.status.code = ErrorCode::Ok;
              ack.status.message = "committed";
              ack.decision = round.value().decision;
              ack.accounting = round.value().accounting;
              const auto grants = server->broker->grants(request.value().target, false);
              if (grants.ok()) {
                for (const Grant& grant : grants.value()) {
                  ArbitrationDecision decision;
                  decision.request = grant.request;
                  decision.request_generation = grant.request_generation;
                  decision.state = grant.state;
                  decision.reason = grant.reason;
                  decision.allocation = grant.allocation;
                  decision.requested_minimum = grant.requested_minimum;
                  decision.requested_desired = grant.requested_desired;
                  decision.requested_maximum = grant.requested_maximum;
                  decision.denied = grant.denied;
                  decision.satisfied = grant.satisfied;
                  decision.grant = grant;
                  ack.decisions.push_back(std::move(decision));
                }
              }
            }
          }
          if (!send_ack(connection->socket, MessageType::ArbitrateAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::QueryGrant: {
          const auto request = decode_query_grant_request(reader);
          QueryGrantAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto view = server->broker->query_grant(request.value().grant, request.value().generation);
            if (!view.ok()) {
              ack.status.code = view.code();
              ack.status.message = view.error().message;
            } else {
              ack.status.code = ErrorCode::Ok;
              ack.status.message = view.value().note;
              ack.found = view.value().found;
              ack.grant = view.value().grant;
            }
          }
          if (!send_ack(connection->socket, MessageType::QueryGrantAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::QueryAccounting: {
          const auto request = decode_query_accounting_request(reader);
          QueryAccountingAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto current = server->broker->accounting(request.value().target);
            ack.status.code = current.code();
            ack.status.message = current.ok() ? "current" : current.error().message;
            if (current.ok()) {
              ack.accounting = current.value();
            }
          }
          if (!send_ack(connection->socket, MessageType::QueryAccountingAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::Release: {
          const auto request = decode_release_request(reader);
          ReleaseAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto outcome = server->broker->release(request.value().grant, request.value().generation);
            ack.status.code = outcome.code();
            ack.status.message = outcome.ok() ? "released" : outcome.error().message;
            if (outcome.ok()) {
              ack.state = outcome.value().state;
              ack.already_released = outcome.value().already_released;
              ack.released = outcome.value().released;
            }
          }
          if (!send_ack(connection->socket, MessageType::ReleaseAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::Revoke: {
          const auto request = decode_revoke_request(reader);
          RevokeAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto outcome = server->broker->revoke(request.value().grant, request.value().generation,
                                                        OutcomeReason::Revoked, request.value().reason);
            ack.status.code = outcome.code();
            ack.status.message = outcome.ok() ? "revoked" : outcome.error().message;
            if (outcome.ok()) {
              ack.state = outcome.value().grant.state;
            }
          }
          if (!send_ack(connection->socket, MessageType::RevokeAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::FencePublisher: {
          const auto request = decode_fence_publisher_request(reader);
          FencePublisherAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto before = server->broker->is_fenced(request.value().publisher, request.value().boot);
            const auto applied = server->broker->fence_publisher(request.value().publisher, request.value().boot,
                                                                 request.value().reason);
            ack.status.code = applied.code();
            ack.status.message = applied.ok() ? "fenced" : applied.message();
            ack.already_fenced = before.ok() && before.value();
          }
          if (!send_ack(connection->socket, MessageType::FencePublisherAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::Explain: {
          const auto request = decode_explain_request(reader);
          ExplainAck ack;
          if (!request.ok()) {
            ack.status.code = request.code();
            ack.status.message = request.error().message;
          } else {
            const auto explanation = server->broker->explain(request.value().request, request.value().generation);
            ack.status.code = explanation.code();
            ack.status.message = explanation.ok() ? "explained" : explanation.error().message;
            if (explanation.ok()) {
              ack.explanation = explanation.value();
            }
          }
          if (!send_ack(connection->socket, MessageType::ExplainAck, correlation, ack).ok()) {
            keep_going = false;
          }
          break;
        }
        case MessageType::Shutdown: {
          const auto request = decode_shutdown_request(reader);
          ShutdownAck ack;
          ack.status.code = request.ok() ? ErrorCode::Ok : request.code();
          ack.status.message = request.ok() ? "stopping" : request.error().message;
          (void)send_ack(connection->socket, MessageType::ShutdownAck, correlation, ack);
          // The shutdown request is the only message that ends the whole server.
          server->stopping.store(true);
          keep_going = false;
          break;
        }
        default: {
          (void)send_error(connection->socket, correlation, ErrorCode::ProtocolMalformed,
                           "message type is not handled by a coordinator");
          keep_going = false;
          break;
        }
      }
      const auto trailing = reader.expect_end();
      if (!trailing.ok()) {
        (void)send_error(connection->socket, correlation, trailing.code(), trailing.message());
        keep_going = false;
      }
    }
  }
  finish();
}

}  // namespace

Result<std::unique_ptr<CoordinatorServer>> CoordinatorServer::start(Broker& broker, const ServerOptions& options) {
  BB_RETURN_IF_ERROR(initialise_networking());
  if (options.max_connections == 0 || options.max_connections > limits::kMaxConnections) {
    return make_error<std::unique_ptr<CoordinatorServer>>(ErrorCode::OutOfRange,
                                                          "max_connections is outside the supported range");
  }
  if (options.receive_buffer_bytes < 4096 || options.receive_buffer_bytes > limits::kMaxOutboundQueueBytes) {
    return make_error<std::unique_ptr<CoordinatorServer>>(ErrorCode::OutOfRange,
                                                          "receive buffer size is outside the supported range");
  }
  if (options.session_token.size() > 256) {
    return make_error<std::unique_ptr<CoordinatorServer>>(ErrorCode::BoundsExceeded, "session token is too long");
  }

  auto server = std::unique_ptr<CoordinatorServer>(new CoordinatorServer());
  server->impl_->broker = &broker;
  server->impl_->options = options;

  auto listener = Listener::bind_loopback(options.port, options.max_connections);
  if (!listener.ok()) {
    return listener.error();
  }
  server->impl_->listener = listener.take();
  server->port_ = server->impl_->listener.port();

  const std::shared_ptr<Impl> impl = server->impl_;
  impl->acceptor = std::thread([impl]() {
    while (!impl->stopping.load()) {
      auto accepted = impl->listener.accept_one(kPollMilliseconds);
      if (!accepted.ok()) {
        continue;
      }
      {
        std::lock_guard<std::mutex> guard(impl->mutex);
        if (impl->active >= impl->options.max_connections) {
          // Connection budget exhausted: refuse rather than grow without bound.
          continue;
        }
        impl->active += 1;
      }
      auto connection = std::make_unique<Connection>();
      connection->socket = accepted.take();
      // Handlers are detached and tracked by the active counter: the server
      // never accumulates finished thread objects, and shutdown waits for the
      // counter rather than joining a thread that might need server state.
      std::thread([impl, connection = std::move(connection)]() mutable {
        serve_connection(impl, std::move(connection));
      }).detach();
    }
  });
  return server;
}

std::size_t CoordinatorServer::active_connections() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->active;
}

std::uint64_t CoordinatorServer::handled_messages() const { return impl_->handled.load(); }

void CoordinatorServer::stop() {
  if (!impl_) {
    return;
  }
  impl_->stopping.store(true);
  impl_->listener.close();
  if (impl_->acceptor.joinable()) {
    impl_->acceptor.join();
  }
  // Connection handlers poll the stop flag every few milliseconds, so the wait
  // below is a bounded drain, not a watchdog: it can only elapse if a handler
  // is stuck inside a broker call, in which case shutdown must wait for it.
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->idle.wait(lock, [this]() { return impl_->active == 0; });
}

}  // namespace bandwidth_broker
