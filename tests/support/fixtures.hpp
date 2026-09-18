// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic fixtures shared by the test suites. Everything here is built
// from explicit integers so that a failure is reproducible from the source.

#ifndef BB_TESTS_SUPPORT_FIXTURES_HPP
#define BB_TESTS_SUPPORT_FIXTURES_HPP

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "bandwidth_broker/arbitrator.hpp"

namespace bb_fixture {

using namespace bandwidth_broker;

[[nodiscard]] inline Bandwidth bw(std::int64_t bits_per_second) {
  auto value = Bandwidth::from_bits_per_second(bits_per_second);
  if (!value.ok()) {
    std::abort();
  }
  return value.value();
}

struct RequestSpec final {
  std::uint64_t id{1};
  std::uint64_t generation{1};
  std::int64_t minimum{0};
  std::int64_t desired{0};
  std::int64_t maximum{0};
  std::uint64_t priority{1};
  std::uint64_t group{1};
  std::uint64_t tenant{1};
  bool borrowing_eligible{false};
  bool preemptible{true};
  RecallTolerance recall{RecallTolerance::Unspecified};
  std::uint64_t publisher{1};
  std::uint64_t publisher_sequence{1};
  bool reservation{false};
  std::uint64_t reservation_id{0};
  std::uint64_t reservation_generation{1};
  GuaranteeClass guarantee{GuaranteeClass::Unspecified};
};

struct Fixture final {
  FabricEpoch epoch{FabricEpoch::from_value(1)};
  CoordinatorIncarnation coordinator{CoordinatorIncarnation::from_value(1)};
  BandwidthResourceId resource{BandwidthResourceId::from_value(1)};
  BandwidthResourceGeneration resource_generation{BandwidthResourceGeneration::initial()};
  CapacitySnapshotId snapshot_id{CapacitySnapshotId::from_value(1)};
  CapacitySnapshotGeneration capacity_generation{CapacitySnapshotGeneration::initial()};
  PolicyId policy_id{PolicyId::from_value(1)};
  PolicyGeneration policy_generation{PolicyGeneration::initial()};
  FairnessConfigGeneration fairness_generation{FairnessConfigGeneration::initial()};
  TenantConfigGeneration tenant_generation{TenantConfigGeneration::initial()};
  std::uint64_t tick{1};
  std::uint64_t next_grant_id{1};
  std::uint64_t next_recall_id{1};
  std::uint64_t next_decision_id{1};

  [[nodiscard]] CapacityTarget target() const {
    CapacityTarget t;
    t.resource = resource;
    t.resource_generation = resource_generation;
    return t;
  }

  [[nodiscard]] Provenance provenance(NodeKind kind = NodeKind::CapacityPublisher) const {
    Provenance p;
    p.source_kind = kind;
    p.source_id = 1;
    p.source_boot = boot_id_from_u64(1, 2);
    p.epoch = epoch;
    p.coordinator = coordinator;
    p.source_sequence = 1;
    return p;
  }

  [[nodiscard]] CapacitySnapshot snapshot(std::int64_t physical,
                                          CapacityEvidenceState evidence = CapacityEvidenceState::Known,
                                          std::int64_t headroom_target = 0,
                                          std::int64_t admin_unavailable = 0,
                                          std::int64_t degraded = 0) const {
    CapacitySnapshot snapshot;
    snapshot.target = target();
    snapshot.snapshot = snapshot_id;
    snapshot.generation = capacity_generation;
    snapshot.evidence = evidence;
    if (evidence == CapacityEvidenceState::Unknown) {
      snapshot.physical_configured = std::nullopt;
      snapshot.administratively_unavailable = Bandwidth::zero();
      snapshot.degraded_loss = Bandwidth::zero();
      snapshot.headroom_target = Bandwidth::zero();
    } else {
      snapshot.physical_configured = bw(physical);
      snapshot.administratively_unavailable = bw(admin_unavailable);
      snapshot.degraded_loss = bw(degraded);
      snapshot.headroom_target = bw(headroom_target);
    }
    snapshot.provenance = provenance();
    snapshot.publisher_sequence = 1;
    snapshot.observed_at = Timestamp::from_unix_nanoseconds(1'700'000'000'000'000'000LL).value();
    snapshot.authority.fabric_epoch = epoch;
    snapshot.authority.coordinator = coordinator;
    snapshot.authority.resource = resource;
    snapshot.authority.resource_generation = resource_generation;
    snapshot.authority.capacity_generation = capacity_generation;
    snapshot.authority.publisher = PublisherId::from_value(7);
    snapshot.authority.publisher_boot = boot_id_from_u64(3, 4);
    snapshot.authority.publisher_sequence = 1;
    snapshot.authority.monotonic_tick = tick;
    return snapshot;
  }

  [[nodiscard]] AuthorityVector requester_authority(const RequestSpec& spec) const {
    AuthorityVector authority;
    authority.fabric_epoch = epoch;
    authority.resource = resource;
    authority.resource_generation = resource_generation;
    authority.policy = policy_id;
    authority.policy_generation = policy_generation;
    authority.request = BandwidthRequestId::from_value(spec.id);
    authority.request_generation = BandwidthRequestGeneration::from_value(spec.generation);
    authority.fairness_group = FairnessGroupId::from_value(spec.group);
    authority.fairness_config_generation = fairness_generation;
    authority.tenant_config_generation = tenant_generation;
    authority.publisher = PublisherId::from_value(spec.publisher);
    authority.publisher_boot = boot_id_from_u64(spec.publisher, 0xAA);
    authority.publisher_sequence = spec.publisher_sequence;
    if (spec.reservation) {
      authority.reservation = ReservationReferenceId::from_value(spec.reservation_id);
      authority.reservation_generation = ReservationGeneration::from_value(spec.reservation_generation);
    }
    return authority;
  }

  [[nodiscard]] BandwidthRequest request(const RequestSpec& spec) const {
    BandwidthRequest request;
    request.id = BandwidthRequestId::from_value(spec.id);
    request.generation = BandwidthRequestGeneration::from_value(spec.generation);
    request.authority = requester_authority(spec);
    request.target = target();
    request.minimum = bw(spec.minimum);
    request.desired = bw(spec.desired);
    request.maximum = bw(spec.maximum);
    request.guarantee_class = spec.guarantee;
    request.priority = PriorityClassId::from_value(spec.priority);
    request.tenant = TenantId::from_value(spec.tenant);
    request.fairness_group = FairnessGroupId::from_value(spec.group);
    request.borrowing_eligible = spec.borrowing_eligible;
    request.preemptible = spec.preemptible;
    request.recall_tolerance = spec.recall;
    if (spec.reservation) {
      ReservationBinding binding;
      binding.reservation = ReservationReferenceId::from_value(spec.reservation_id);
      binding.generation = ReservationGeneration::from_value(spec.reservation_generation);
      request.reservation = binding;
    }
    request.provenance = provenance(NodeKind::Requester);
    return request;
  }
};

[[nodiscard]] inline PriorityClass priority_class(std::uint64_t id, std::uint32_t rank, std::uint64_t weight = 1,
                                                  bool emergency = false, bool borrowing = false) {
  PriorityClass priority;
  priority.id = PriorityClassId::from_value(id);
  priority.name = "class" + std::to_string(id);
  priority.rank = rank;
  priority.weight = weight;
  priority.emergency = emergency;
  priority.borrowing_eligible = borrowing;
  return priority;
}

[[nodiscard]] inline FairnessGroupConfig fairness_group(std::uint64_t id, std::uint64_t tenant,
                                                        std::uint64_t weight = 1,
                                                        std::int64_t cap = 0,
                                                        std::uint32_t cap_permille = 0,
                                                        bool borrowing = false) {
  FairnessGroupConfig group;
  group.id = FairnessGroupId::from_value(id);
  group.tenant = TenantId::from_value(tenant);
  group.name = "group" + std::to_string(id);
  group.weight = weight;
  group.maximum_cap = bw(cap);
  group.maximum_cap_permille = cap_permille;
  group.borrowing_eligible = borrowing;
  return group;
}

// Three priority classes (ranks 10, 20, 30) and one fairness group per tenant,
// with weights of one. This is the canonical shape used by most tests.
[[nodiscard]] inline Policy basic_policy(const Fixture& fixture,
                                         std::size_t group_count = 1,
                                         SchedulingMode mode = SchedulingMode::StrictPriority) {
  Policy policy;
  policy.id = fixture.policy_id;
  policy.generation = fixture.policy_generation;
  policy.scheduling = mode;
  policy.fairness_config_generation = fixture.fairness_generation;
  policy.tenant_config_generation = fixture.tenant_generation;
  policy.priority_classes = {priority_class(1, 10), priority_class(2, 20), priority_class(3, 30)};
  for (std::size_t i = 0; i < group_count; ++i) {
    policy.fairness_groups.push_back(fairness_group(i + 1, i + 1));
  }
  return policy;
}

}  // namespace bb_fixture

#endif  // BB_TESTS_SUPPORT_FIXTURES_HPP
