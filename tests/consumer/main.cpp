// Bandwidth Broker downstream consumer.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Exercises the installed public API only: install a policy, publish capacity,
// submit requests, arbitrate, inspect the accounting and explanation, release
// everything and verify that accounting returned to its baseline. It links
// against the installed package, never against the build tree.

#include <cstdio>
#include <string>
#include <vector>

#include <bandwidth_broker/bandwidth_broker.hpp>

using namespace bandwidth_broker;

namespace {

[[nodiscard]] Bandwidth bps(std::int64_t value) { return Bandwidth::from_bits_per_second(value).value(); }

[[nodiscard]] Policy consumer_policy() {
  Policy policy;
  policy.id = PolicyId::from_value(1);
  policy.generation = PolicyGeneration::initial();
  policy.fairness_config_generation = FairnessConfigGeneration::initial();
  policy.tenant_config_generation = TenantConfigGeneration::initial();
  policy.scheduling = SchedulingMode::StrictPriority;
  policy.headroom_permille = 100;

  PriorityClass low;
  low.id = PriorityClassId::from_value(1);
  low.name = "low";
  low.rank = 10;

  PriorityClass high;
  high.id = PriorityClassId::from_value(2);
  high.name = "high";
  high.rank = 20;

  policy.priority_classes = {low, high};

  FairnessGroupConfig group;
  group.id = FairnessGroupId::from_value(1);
  group.tenant = TenantId::from_value(1);
  group.name = "tenant-1";
  policy.fairness_groups = {group};
  return policy;
}

[[nodiscard]] BandwidthRequest consumer_request(std::uint64_t id,
                                               const CapacityTarget& target,
                                               FabricEpoch epoch,
                                               const Policy& policy,
                                               BootId boot,
                                               std::int64_t desired) {
  BandwidthRequest request;
  request.id = BandwidthRequestId::from_value(id);
  request.generation = BandwidthRequestGeneration::initial();
  request.target = target;
  request.minimum = Bandwidth::zero();
  request.desired = bps(desired);
  request.maximum = bps(desired);
  request.priority = PriorityClassId::from_value(2);
  request.tenant = TenantId::from_value(1);
  request.fairness_group = FairnessGroupId::from_value(1);
  request.authority.fabric_epoch = epoch;
  request.authority.resource = target.resource;
  request.authority.resource_generation = target.resource_generation;
  request.authority.policy = policy.id;
  request.authority.policy_generation = policy.generation;
  request.authority.request = request.id;
  request.authority.request_generation = request.generation;
  request.authority.fairness_group = request.fairness_group;
  request.authority.fairness_config_generation = policy.fairness_config_generation;
  request.authority.tenant_config_generation = policy.tenant_config_generation;
  request.authority.publisher = PublisherId::from_value(1);
  request.authority.publisher_boot = boot;
  request.authority.publisher_sequence = 1;
  return request;
}

}  // namespace

int main() {
  std::printf("bandwidth-broker version: %s (protocol %u, store format %u)\n", version_string(),
              protocol_version(), store_format_version());

  const FabricEpoch epoch = FabricEpoch::from_value(1);
  const CoordinatorIncarnation incarnation = CoordinatorIncarnation::from_value(1);
  const BootId boot = boot_id_from_u64(1, 1);

  BrokerConfig config;
  config.epoch = epoch;
  config.incarnation = incarnation;
  auto broker = Broker::open(config);
  if (!broker.ok()) {
    std::fprintf(stderr, "cannot open the coordinator: %s\n", broker.error().message.c_str());
    return 1;
  }

  const Policy policy = consumer_policy();
  if (!broker.value()->set_policy(policy).ok()) {
    std::fprintf(stderr, "cannot install the policy\n");
    return 1;
  }

  CapacityTarget target;
  target.resource = BandwidthResourceId::from_value(1);
  target.resource_generation = BandwidthResourceGeneration::initial();

  CapacitySnapshot snapshot;
  snapshot.target = target;
  snapshot.snapshot = CapacitySnapshotId::from_value(1);
  snapshot.generation = CapacitySnapshotGeneration::initial();
  snapshot.evidence = CapacityEvidenceState::Known;
  snapshot.physical_configured = bps(1'000'000);
  snapshot.provenance.source_kind = NodeKind::CapacityPublisher;
  snapshot.provenance.source_boot = boot;
  snapshot.provenance.epoch = epoch;
  snapshot.provenance.coordinator = incarnation;
  snapshot.authority.fabric_epoch = epoch;
  snapshot.authority.coordinator = incarnation;
  snapshot.authority.resource = target.resource;
  snapshot.authority.resource_generation = target.resource_generation;
  snapshot.authority.capacity_generation = snapshot.generation;
  snapshot.authority.publisher = PublisherId::from_value(2);
  snapshot.authority.publisher_boot = boot;
  if (!broker.value()->publish_capacity(snapshot).ok()) {
    std::fprintf(stderr, "cannot publish capacity\n");
    return 1;
  }

  const std::vector<BandwidthRequest> requests = {
      consumer_request(1, target, epoch, policy, boot, 300'000),
      consumer_request(2, target, epoch, policy, boot, 300'000)};
  for (const BandwidthRequest& request : requests) {
    const auto submitted = broker.value()->submit(request);
    if (!submitted.ok()) {
      std::fprintf(stderr, "cannot submit request %s: %s\n", request.id.to_string().c_str(),
                   submitted.error().message.c_str());
      return 1;
    }
  }

  const auto round = broker.value()->arbitrate(target);
  if (!round.ok()) {
    std::fprintf(stderr, "arbitration failed: %s\n", round.error().message.c_str());
    return 1;
  }
  std::printf("granted=%zu waiting=%zu\n", round.value().granted, round.value().waiting);
  std::printf("authorized=%lld headroom=%lld\n",
              static_cast<long long>(round.value().accounting.authorized_consumption.bits_per_second()),
              static_cast<long long>(round.value().accounting.headroom.bits_per_second()));

  const auto explanation = broker.value()->explain(BandwidthRequestId::from_value(1),
                                                   BandwidthRequestGeneration::initial());
  if (!explanation.ok()) {
    std::fprintf(stderr, "cannot explain request 1\n");
    return 1;
  }
  std::printf("explanation: %s\n", describe_explanation(explanation.value()).c_str());

  const auto live = broker.value()->grants(target, true);
  if (!live.ok()) {
    return 1;
  }
  for (const Grant& grant : live.value()) {
    if (!broker.value()->release(grant.id, grant.generation).ok()) {
      std::fprintf(stderr, "release failed\n");
      return 1;
    }
  }
  const auto accounting = broker.value()->accounting(target);
  if (!accounting.ok()) {
    return 1;
  }
  if (!accounting.value().validate().empty()) {
    std::fprintf(stderr, "accounting invariants violated after release\n");
    return 1;
  }
  std::printf("after release authorized=%lld unallocated=%lld\n",
              static_cast<long long>(accounting.value().authorized_consumption.bits_per_second()),
              static_cast<long long>(accounting.value().unallocated.bits_per_second()));
  if (accounting.value().authorized_consumption.bits_per_second() != 0) {
    std::fprintf(stderr, "accounting did not return to baseline\n");
    return 1;
  }
  std::printf("consumer OK\n");
  return 0;
}
