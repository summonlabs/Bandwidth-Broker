// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end example: open a coordinator, install a policy, publish capacity,
// submit three competing requests, arbitrate, print the explanations and
// release everything. Every number printed is produced by the runtime.

#include <cstdio>
#include <string>
#include <vector>

#include "bandwidth_broker/broker.hpp"

namespace {

using namespace bandwidth_broker;

[[nodiscard]] Bandwidth bps(std::int64_t value) { return Bandwidth::from_bits_per_second(value).value(); }

[[nodiscard]] Policy example_policy() {
  Policy policy;
  policy.id = PolicyId::from_value(1);
  policy.generation = PolicyGeneration::initial();
  policy.scheduling = SchedulingMode::StrictPriority;
  policy.fairness_config_generation = FairnessConfigGeneration::initial();
  policy.tenant_config_generation = TenantConfigGeneration::initial();
  policy.headroom_permille = 100;
  policy.minimum_allocation_quantum = bps(1000);

  PriorityClass background;
  background.id = PriorityClassId::from_value(1);
  background.name = "background";
  background.rank = 10;
  background.weight = 1;

  PriorityClass interactive;
  interactive.id = PriorityClassId::from_value(2);
  interactive.name = "interactive";
  interactive.rank = 20;
  interactive.weight = 4;

  policy.priority_classes = {background, interactive};

  FairnessGroupConfig team_a;
  team_a.id = FairnessGroupId::from_value(1);
  team_a.tenant = TenantId::from_value(1);
  team_a.name = "team-a";
  team_a.weight = 1;

  FairnessGroupConfig team_b;
  team_b.id = FairnessGroupId::from_value(2);
  team_b.tenant = TenantId::from_value(2);
  team_b.name = "team-b";
  team_b.weight = 3;

  policy.fairness_groups = {team_a, team_b};
  return policy;
}

[[nodiscard]] BandwidthRequest make_request(std::uint64_t id,
                                            std::uint64_t group,
                                            std::uint64_t tenant,
                                            std::uint64_t priority,
                                            std::int64_t minimum,
                                            std::int64_t desired,
                                            const CapacityTarget& target,
                                            FabricEpoch epoch,
                                            const Policy& policy,
                                            BootId boot) {
  BandwidthRequest request;
  request.id = BandwidthRequestId::from_value(id);
  request.generation = BandwidthRequestGeneration::initial();
  request.target = target;
  request.minimum = bps(minimum);
  request.desired = bps(desired);
  request.maximum = bps(desired);
  request.priority = PriorityClassId::from_value(priority);
  request.tenant = TenantId::from_value(tenant);
  request.fairness_group = FairnessGroupId::from_value(group);
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

  const Policy policy = example_policy();
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
  snapshot.physical_configured = bps(10'000'000);
  snapshot.headroom_target = Bandwidth::zero();
  snapshot.provenance.source_kind = NodeKind::CapacityPublisher;
  snapshot.provenance.source_id = 1;
  snapshot.provenance.source_boot = boot_id_from_u64(2, 2);
  snapshot.provenance.epoch = epoch;
  snapshot.provenance.coordinator = incarnation;
  snapshot.authority.fabric_epoch = epoch;
  snapshot.authority.coordinator = incarnation;
  snapshot.authority.resource = target.resource;
  snapshot.authority.resource_generation = target.resource_generation;
  snapshot.authority.capacity_generation = snapshot.generation;
  snapshot.authority.publisher = PublisherId::from_value(2);
  snapshot.authority.publisher_boot = boot_id_from_u64(2, 2);
  if (!broker.value()->publish_capacity(snapshot).ok()) {
    std::fprintf(stderr, "cannot publish capacity\n");
    return 1;
  }

  const std::vector<BandwidthRequest> requests = {
      make_request(1, 1, 1, 2, 2'000'000, 6'000'000, target, epoch, policy, boot),
      make_request(2, 2, 2, 2, 500'000, 4'000'000, target, epoch, policy, boot),
      make_request(3, 1, 1, 1, 0, 5'000'000, target, epoch, policy, boot)};
  for (const BandwidthRequest& request : requests) {
    const auto outcome = broker.value()->submit(request);
    if (!outcome.ok()) {
      std::fprintf(stderr, "cannot submit request %s: %s\n", request.id.to_string().c_str(),
                   outcome.error().message.c_str());
      return 1;
    }
  }

  const auto round = broker.value()->arbitrate(target);
  if (!round.ok()) {
    std::fprintf(stderr, "arbitration failed: %s\n", round.error().message.c_str());
    return 1;
  }

  std::printf("round: granted=%zu waiting=%zu refused=%zu tick=%llu\n", round.value().granted,
              round.value().waiting, round.value().refused,
              static_cast<unsigned long long>(round.value().tick));
  std::printf("accounting: %s\n", describe_accounting(round.value().accounting).c_str());
  for (const BandwidthRequest& request : requests) {
    const auto explanation = broker.value()->explain(request.id, request.generation);
    if (!explanation.ok()) {
      continue;
    }
    std::printf("explanation: %s\n", describe_explanation(explanation.value()).c_str());
  }

  const auto live = broker.value()->grants(target, true);
  if (!live.ok()) {
    return 1;
  }
  std::printf("live grants: %zu\n", live.value().size());
  for (const Grant& grant : live.value()) {
    const auto released = broker.value()->release(grant.id, grant.generation);
    if (!released.ok()) {
      std::fprintf(stderr, "release failed: %s\n", released.error().message.c_str());
      return 1;
    }
  }
  const auto accounting = broker.value()->accounting(target);
  if (!accounting.ok()) {
    return 1;
  }
  std::printf("after release: authorized=%lld unallocated=%lld\n",
              static_cast<long long>(accounting.value().authorized_consumption.bits_per_second()),
              static_cast<long long>(accounting.value().unallocated.bits_per_second()));
  return 0;
}
