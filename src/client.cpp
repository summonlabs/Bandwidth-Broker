// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/client.hpp"

#include <string>
#include <utility>

namespace bandwidth_broker {

struct CoordinatorClient::Impl final {
  Socket socket;
  ClientOptions options;
  std::vector<std::uint8_t> buffer;
  FrameDecoder decoder;

  // Sends one request frame and blocks until the matching response arrives.
  template <typename Ack>
  [[nodiscard]] Result<Ack> exchange(MessageType request_type, MessageType ack_type, std::uint64_t correlation,
                                     const std::vector<std::uint8_t>& payload,
                                     Result<Ack> (*decoder_fn)(Reader&)) {
    const auto frame = encode_frame(request_type, correlation, payload);
    if (!frame.ok()) {
      return frame.error();
    }
    BB_RETURN_IF_ERROR(socket.send_all(frame.value().data(), frame.value().size()));

    for (;;) {
      MessageType type = MessageType::ErrorResponse;
      std::uint64_t reply_correlation = 0;
      std::vector<std::uint8_t> reply_payload;
      const auto next = decoder.next(type, reply_correlation, reply_payload);
      if (!next.ok()) {
        return next.error();
      }
      if (next.value()) {
        if (reply_correlation != correlation) {
          return make_error<Ack>(ErrorCode::ProtocolMalformed,
                                 "the coordinator answered a different correlation id");
        }
        Reader reader(reply_payload);
        if (type == MessageType::ErrorResponse) {
          const auto failure = decode_error_response(reader);
          if (!failure.ok()) {
            return failure.error();
          }
          return make_error<Ack>(failure.value().status.code,
                                 failure.value().status.message.empty() ? "the coordinator refused the request"
                                                                        : failure.value().status.message);
        }
        if (type != ack_type) {
          return make_error<Ack>(ErrorCode::ProtocolMalformed, "the coordinator answered with an unexpected type");
        }
        auto decoded = decoder_fn(reader);
        if (!decoded.ok()) {
          return decoded.error();
        }
        BB_RETURN_IF_ERROR(reader.expect_end());
        return decoded.take();
      }
      const auto readable = socket.wait_readable(50);
      if (!readable.ok()) {
        return readable.error();
      }
      if (!readable.value()) {
        continue;
      }
      const auto received = socket.receive_some(buffer.data(), buffer.size());
      if (!received.ok()) {
        return received.error();
      }
      if (received.value() == 0) {
        return make_error<Ack>(ErrorCode::ConnectionClosed, "the coordinator closed the connection");
      }
      BB_RETURN_IF_ERROR(decoder.feed(buffer.data(), received.value()));
    }
  }

  template <typename Request, typename Ack>
  [[nodiscard]] Result<Ack> call(MessageType request_type, MessageType ack_type, std::uint64_t correlation,
                                 const Request& request, Result<Ack> (*decoder_fn)(Reader&)) {
    const auto payload = encode_payload(request);
    if (!payload.ok()) {
      return payload.error();
    }
    return exchange<Ack>(request_type, ack_type, correlation, payload.value(), decoder_fn);
  }
};

CoordinatorClient::CoordinatorClient() : impl_(std::make_unique<Impl>()) {}
CoordinatorClient::~CoordinatorClient() = default;

void CoordinatorClient::close() noexcept { impl_->socket.close(); }

Result<std::unique_ptr<CoordinatorClient>> CoordinatorClient::connect(const ClientOptions& options) {
  BB_RETURN_IF_ERROR(initialise_networking());
  if (!options.publisher.valid() || !options.boot.valid()) {
    return make_error<std::unique_ptr<CoordinatorClient>>(ErrorCode::InvalidIdentity,
                                                          "a client requires a publisher id and a boot identity");
  }
  if (options.port == 0) {
    return make_error<std::unique_ptr<CoordinatorClient>>(ErrorCode::InvalidArgument, "port must not be zero");
  }
  if (options.session_token.size() > 256) {
    return make_error<std::unique_ptr<CoordinatorClient>>(ErrorCode::BoundsExceeded, "session token is too long");
  }
  auto socket = Socket::connect_to(options.host, options.port);
  if (!socket.ok()) {
    return socket.error();
  }
  auto client = std::unique_ptr<CoordinatorClient>(new CoordinatorClient());
  client->impl_->socket = socket.take();
  client->impl_->options = options;
  client->impl_->buffer.resize(64 * 1024);
  return client;
}

Result<HelloAck> CoordinatorClient::handshake() {
  HelloRequest request;
  request.publisher = impl_->options.publisher;
  request.boot = impl_->options.boot;
  request.protocol_version = static_cast<std::uint16_t>(BB_PROTOCOL_VERSION);
  request.session_token = impl_->options.session_token;
  request.nonce = impl_->options.boot.hash();
  correlation_ += 1;
  auto ack = impl_->call(MessageType::Hello, MessageType::HelloAck, correlation_, request, &decode_hello_ack);
  if (!ack.ok()) {
    return ack.error();
  }
  session_ = ack.value();
  if (session_.status.code != ErrorCode::Ok) {
    return make_error<HelloAck>(session_.status.code,
                                session_.status.message.empty() ? "the coordinator rejected the handshake"
                                                                : session_.status.message);
  }
  return session_;
}

Result<PublishCapacityAck> CoordinatorClient::publish_capacity(const CapacitySnapshot& snapshot) {
  PublishCapacityRequest request;
  request.snapshot = snapshot;
  correlation_ += 1;
  return impl_->call(MessageType::PublishCapacity, MessageType::PublishCapacityAck, correlation_, request,
                     &decode_publish_capacity_ack);
}

Result<SetPolicyAck> CoordinatorClient::set_policy(const Policy& policy) {
  SetPolicyRequest request;
  request.policy = policy;
  correlation_ += 1;
  return impl_->call(MessageType::SetPolicy, MessageType::SetPolicyAck, correlation_, request, &decode_set_policy_ack);
}

Result<SubmitRequestsAck> CoordinatorClient::submit(const std::vector<BandwidthRequest>& requests) {
  SubmitRequestsRequest request;
  request.requests = requests;
  correlation_ += 1;
  return impl_->call(MessageType::SubmitRequests, MessageType::SubmitRequestsAck, correlation_, request,
                     &decode_submit_requests_ack);
}

Result<ArbitrateAck> CoordinatorClient::arbitrate(const CapacityTarget& target) {
  ArbitrateRequest request;
  request.target = target;
  correlation_ += 1;
  return impl_->call(MessageType::Arbitrate, MessageType::ArbitrateAck, correlation_, request, &decode_arbitrate_ack);
}

Result<QueryGrantAck> CoordinatorClient::query_grant(BandwidthGrantId id, BandwidthGrantGeneration generation) {
  QueryGrantRequest request;
  request.grant = id;
  request.generation = generation;
  correlation_ += 1;
  return impl_->call(MessageType::QueryGrant, MessageType::QueryGrantAck, correlation_, request,
                     &decode_query_grant_ack);
}

Result<QueryAccountingAck> CoordinatorClient::query_accounting(const CapacityTarget& target) {
  QueryAccountingRequest request;
  request.target = target;
  correlation_ += 1;
  return impl_->call(MessageType::QueryAccounting, MessageType::QueryAccountingAck, correlation_, request,
                     &decode_query_accounting_ack);
}

Result<ReleaseAck> CoordinatorClient::release(BandwidthGrantId id, BandwidthGrantGeneration generation) {
  ReleaseRequest request;
  request.grant = id;
  request.generation = generation;
  correlation_ += 1;
  return impl_->call(MessageType::Release, MessageType::ReleaseAck, correlation_, request, &decode_release_ack);
}

Result<RevokeAck> CoordinatorClient::revoke(BandwidthGrantId id, BandwidthGrantGeneration generation,
                                            const std::string& reason) {
  RevokeRequest request;
  request.grant = id;
  request.generation = generation;
  request.reason = reason;
  correlation_ += 1;
  return impl_->call(MessageType::Revoke, MessageType::RevokeAck, correlation_, request, &decode_revoke_ack);
}

Result<FencePublisherAck> CoordinatorClient::fence_publisher(PublisherId publisher, BootId boot,
                                                             const std::string& reason) {
  FencePublisherRequest request;
  request.publisher = publisher;
  request.boot = boot;
  request.reason = reason;
  correlation_ += 1;
  return impl_->call(MessageType::FencePublisher, MessageType::FencePublisherAck, correlation_, request,
                     &decode_fence_publisher_ack);
}

Result<ExplainAck> CoordinatorClient::explain(BandwidthRequestId id, BandwidthRequestGeneration generation) {
  ExplainRequest request;
  request.request = id;
  request.generation = generation;
  correlation_ += 1;
  return impl_->call(MessageType::Explain, MessageType::ExplainAck, correlation_, request, &decode_explain_ack);
}

Result<ShutdownAck> CoordinatorClient::shutdown_coordinator(const std::string& reason) {
  ShutdownRequest request;
  request.reason = reason;
  correlation_ += 1;
  return impl_->call(MessageType::Shutdown, MessageType::ShutdownAck, correlation_, request, &decode_shutdown_ack);
}

}  // namespace bandwidth_broker
