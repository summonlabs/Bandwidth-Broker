// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Synthetic benchmark of the completed arbitration path.
//
// Everything measured here is synthetic: a synthetic request population against
// synthetic capacity. These numbers are NOT physical-network performance and
// must never be presented as such. The benchmark measures completed work (a
// full arbitration plus the resulting decision set), not submission latency.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bandwidth_broker/arbitrator.hpp"
#include "bandwidth_broker/broker.hpp"

namespace {

using namespace bandwidth_broker;

[[nodiscard]] Bandwidth bps(std::int64_t value) { return Bandwidth::from_bits_per_second(value).value(); }

struct Population final {
  Policy policy;
  CapacitySnapshot snapshot;
  CapacityTarget target;
  std::vector<Obligation> obligations;
  std::vector<ArbitrationCandidate> candidates;
};

[[nodiscard]] Population build(std::size_t request_count,
                               std::size_t group_count,
                               std::size_t obligation_count,
                               std::int64_t physical,
                               bool borrowing,
                               std::uint64_t seed) {
  const FabricEpoch epoch = FabricEpoch::from_value(1);
  const CoordinatorIncarnation incarnation = CoordinatorIncarnation::from_value(1);
  const BootId boot = boot_id_from_u64(1, 1);

  Population population;
  population.target.resource = BandwidthResourceId::from_value(1);
  population.target.resource_generation = BandwidthResourceGeneration::initial();

  Policy& policy = population.policy;
  policy.id = PolicyId::from_value(1);
  policy.generation = PolicyGeneration::initial();
  policy.fairness_config_generation = FairnessConfigGeneration::initial();
  policy.tenant_config_generation = TenantConfigGeneration::initial();
  policy.minimum_allocation_quantum = bps(1'000);
  policy.starvation.enabled = true;
  policy.starvation.aging_threshold_rounds = 4;
  if (borrowing) {
    policy.borrow.enabled = true;
    policy.borrow.lend_obligations = true;
    policy.borrow.max_lend_permille = 1000;
    policy.borrow.max_borrow_permille = 1000;
  }
  for (std::uint32_t rank = 1; rank <= 4; ++rank) {
    PriorityClass priority_class;
    priority_class.id = PriorityClassId::from_value(rank);
    priority_class.name = "class" + std::to_string(rank);
    priority_class.rank = rank * 10;
    priority_class.weight = rank;
    priority_class.borrowing_eligible = borrowing;
    policy.priority_classes.push_back(priority_class);
  }
  for (std::uint64_t index = 1; index <= group_count; ++index) {
    FairnessGroupConfig group;
    group.id = FairnessGroupId::from_value(index);
    group.tenant = TenantId::from_value(index);
    group.name = "group" + std::to_string(index);
    group.weight = 1 + (index % 5);
    group.borrowing_eligible = borrowing;
    policy.fairness_groups.push_back(group);
  }

  CapacitySnapshot& snapshot = population.snapshot;
  snapshot.target = population.target;
  snapshot.snapshot = CapacitySnapshotId::from_value(1);
  snapshot.generation = CapacitySnapshotGeneration::initial();
  snapshot.evidence = CapacityEvidenceState::Known;
  snapshot.physical_configured = bps(physical);
  snapshot.provenance.source_kind = NodeKind::CapacityPublisher;
  snapshot.provenance.source_boot = boot;
  snapshot.provenance.epoch = epoch;
  snapshot.provenance.coordinator = incarnation;
  snapshot.authority.fabric_epoch = epoch;
  snapshot.authority.coordinator = incarnation;
  snapshot.authority.resource = population.target.resource;
  snapshot.authority.resource_generation = population.target.resource_generation;
  snapshot.authority.capacity_generation = snapshot.generation;
  snapshot.authority.publisher = PublisherId::from_value(2);
  snapshot.authority.publisher_boot = boot;

  for (std::uint64_t index = 1; index <= obligation_count; ++index) {
    Obligation obligation;
    obligation.reservation = ReservationReferenceId::from_value(index);
    obligation.generation = ReservationGeneration::initial();
    obligation.target = population.target;
    obligation.amount = bps(physical / static_cast<std::int64_t>(obligation_count + 1));
    obligation.lendable = borrowing;
    obligation.max_lend_permille = borrowing ? 1000 : 0;
    obligation.provenance.source_kind = NodeKind::Operator;
    obligation.provenance.epoch = epoch;
    population.obligations.push_back(obligation);
  }

  // A deterministic request population: no floating point, no hidden state.
  std::uint64_t state = seed;
  for (std::size_t index = 0; index < request_count; ++index) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t noise = state >> 33;

    BandwidthRequest request;
    request.id = BandwidthRequestId::from_value(index + 1);
    request.generation = BandwidthRequestGeneration::initial();
    request.target = population.target;
    request.minimum = bps(static_cast<std::int64_t>(noise % 500'000));
    request.desired = request.minimum.checked_add(bps(static_cast<std::int64_t>(noise % 2'000'000))).value();
    request.maximum = request.desired;
    request.priority = PriorityClassId::from_value(1 + (noise % 4));
    const std::uint64_t group = 1 + (noise % group_count);
    request.tenant = TenantId::from_value(group);
    request.fairness_group = FairnessGroupId::from_value(group);
    request.borrowing_eligible = borrowing;
    request.preemptible = true;
    request.authority.fabric_epoch = epoch;
    request.authority.resource = population.target.resource;
    request.authority.resource_generation = population.target.resource_generation;
    request.authority.policy = policy.id;
    request.authority.policy_generation = policy.generation;
    request.authority.request = request.id;
    request.authority.request_generation = request.generation;
    request.authority.fairness_group = request.fairness_group;
    request.authority.fairness_config_generation = policy.fairness_config_generation;
    request.authority.tenant_config_generation = policy.tenant_config_generation;
    request.authority.publisher = PublisherId::from_value(1);
    request.authority.publisher_boot = boot;
    request.authority.publisher_sequence = index + 1;

    ArbitrationCandidate candidate;
    candidate.request = request;
    candidate.wait_rounds = noise % 8;
    population.candidates.push_back(candidate);
  }
  return population;
}

[[nodiscard]] double run_rounds(const Population& population, std::size_t rounds) {
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t granted = 0;
  for (std::size_t round = 0; round < rounds; ++round) {
    ArbitrationInput input;
    input.target = population.target;
    input.snapshot = population.snapshot;
    input.policy = population.policy;
    input.obligations = population.obligations;
    input.candidates = population.candidates;
    input.epoch = FabricEpoch::from_value(1);
    input.coordinator = CoordinatorIncarnation::from_value(1);
    input.tick = round + 1;
    input.decision = DecisionId::from_value(round + 1);
    const auto outcome = arbitrate(input);
    if (!outcome.ok()) {
      std::fprintf(stderr, "benchmark round failed: %s\n", outcome.error().message.c_str());
      std::exit(1);
    }
    for (const ArbitrationDecision& decision : outcome.value().decisions) {
      if (decision.grant.has_value()) {
        ++granted;
      }
    }
  }
  const auto end = std::chrono::steady_clock::now();
  (void)granted;
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void report(const char* dimension,
            const char* value,
            std::size_t requests,
            std::size_t groups,
            std::size_t obligations,
            std::size_t rounds,
            double milliseconds) {
  std::printf("dimension=%s value=%s requests=%zu groups=%zu obligations=%zu rounds=%zu total_ms=%.3f per_round_ms=%.6f\n",
              dimension, value, requests, groups, obligations, rounds, milliseconds,
              milliseconds / static_cast<double>(rounds));
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      quick = true;
    }
  }

  const std::size_t rounds = quick ? 3 : 20;
  std::printf("# synthetic population benchmark; not physical-network performance\n");

  for (const std::size_t requests : (quick ? std::vector<std::size_t>{64, 256} : std::vector<std::size_t>{64, 256, 1024, 4096})) {
    const Population population = build(requests, 8, 4, 100'000'000, false, 12345);
    report("request_count", std::to_string(requests).c_str(), requests, 8, 4, rounds,
           run_rounds(population, rounds));
  }
  for (const std::size_t groups : (quick ? std::vector<std::size_t>{4, 16} : std::vector<std::size_t>{4, 16, 64, 256})) {
    const Population population = build(512, groups, 4, 100'000'000, false, 999);
    report("group_count", std::to_string(groups).c_str(), 512, groups, 4, rounds, run_rounds(population, rounds));
  }
  for (const std::size_t obligations : (quick ? std::vector<std::size_t>{1, 8} : std::vector<std::size_t>{1, 8, 64, 256})) {
    const Population population = build(512, 8, obligations, 100'000'000, true, 4242);
    report("obligation_density", std::to_string(obligations).c_str(), 512, 8, obligations, rounds,
           run_rounds(population, rounds));
  }
  {
    const Population plain = build(1024, 16, 32, 100'000'000, false, 7);
    report("borrow_recall_pressure", "off", 1024, 16, 32, rounds, run_rounds(plain, rounds));
    const Population borrowing = build(1024, 16, 32, 100'000'000, true, 7);
    report("borrow_recall_pressure", "on", 1024, 16, 32, rounds, run_rounds(borrowing, rounds));
  }

  // Full commit path through the coordinator, including durable records.
  {
    const std::size_t requests = quick ? 64 : 512;
    const Population population = build(requests, 8, 4, 100'000'000, false, 31337);
    BrokerConfig config;
    config.epoch = FabricEpoch::from_value(1);
    config.incarnation = CoordinatorIncarnation::from_value(1);
    auto broker = Broker::open(config);
    if (!broker.ok()) {
      std::fprintf(stderr, "cannot open the coordinator\n");
      return 1;
    }
    if (!broker.value()->set_policy(population.policy).ok()) {
      return 1;
    }
    for (const Obligation& obligation : population.obligations) {
      (void)broker.value()->upsert_obligation(obligation);
    }
    if (!broker.value()->publish_capacity(population.snapshot).ok()) {
      return 1;
    }
    for (const ArbitrationCandidate& candidate : population.candidates) {
      (void)broker.value()->submit(candidate.request);
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
      const auto summary = broker.value()->arbitrate(population.target);
      if (!summary.ok()) {
        std::fprintf(stderr, "commit path failed: %s\n", summary.error().message.c_str());
        return 1;
      }
    }
    const auto end = std::chrono::steady_clock::now();
    const double milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    report("commit_path_requests", std::to_string(requests).c_str(), requests, 8, 4, rounds, milliseconds);
  }
  return 0;
}
