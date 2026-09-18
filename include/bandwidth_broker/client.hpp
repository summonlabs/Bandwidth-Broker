// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Coordinator client.
//
// One client owns one connection and one correlation-id sequence. A client is a
// process incarnation: its BootId is its fencing identity, and a fenced boot
// can never authorise capacity again.

#ifndef BANDWIDTH_BROKER_CLIENT_HPP
#define BANDWIDTH_BROKER_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bandwidth_broker/net.hpp"
#include "bandwidth_broker/wire.hpp"

namespace bandwidth_broker {

struct BB_API ClientOptions final {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  std::string session_token;
  PublisherId publisher{};
  BootId boot{};
  // Bounded wait for the handshake response, in milliseconds. Only the
  // handshake uses it; request/response pairs block until answered.
  int handshake_poll_milliseconds{100};
};

class BB_API CoordinatorClient final {
 public:
  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;
  ~CoordinatorClient();

  [[nodiscard]] static Result<std::unique_ptr<CoordinatorClient>> connect(const ClientOptions& options);

  [[nodiscard]] Result<HelloAck> handshake();
  [[nodiscard]] const HelloAck& session() const noexcept { return session_; }

  [[nodiscard]] Result<PublishCapacityAck> publish_capacity(const CapacitySnapshot& snapshot);
  [[nodiscard]] Result<SetPolicyAck> set_policy(const Policy& policy);
  [[nodiscard]] Result<SubmitRequestsAck> submit(const std::vector<BandwidthRequest>& requests);
  [[nodiscard]] Result<ArbitrateAck> arbitrate(const CapacityTarget& target);
  [[nodiscard]] Result<QueryGrantAck> query_grant(BandwidthGrantId id, BandwidthGrantGeneration generation);
  [[nodiscard]] Result<QueryAccountingAck> query_accounting(const CapacityTarget& target);
  [[nodiscard]] Result<ReleaseAck> release(BandwidthGrantId id, BandwidthGrantGeneration generation);
  [[nodiscard]] Result<RevokeAck> revoke(BandwidthGrantId id, BandwidthGrantGeneration generation,
                                         const std::string& reason);
  [[nodiscard]] Result<FencePublisherAck> fence_publisher(PublisherId publisher, BootId boot,
                                                          const std::string& reason);
  [[nodiscard]] Result<ExplainAck> explain(BandwidthRequestId id, BandwidthRequestGeneration generation);
  [[nodiscard]] Result<ShutdownAck> shutdown_coordinator(const std::string& reason);

  void close() noexcept;
  [[nodiscard]] std::uint64_t messages_exchanged() const noexcept { return correlation_; }

 private:
  CoordinatorClient();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  HelloAck session_{};
  std::uint64_t correlation_{0};
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_CLIENT_HPP
