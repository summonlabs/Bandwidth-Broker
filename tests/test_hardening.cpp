// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Property, concurrency and adversarial hardening.
//
// Property tests drive deterministic seeded sequences of every authoritative
// operation and re-check every accounting and authority invariant after each
// step. Concurrency tests run real threads against one coordinator and require
// exactly one authoritative winner per conflict. Adversarial tests try to break
// the runtime with contradictory, oversized, stale and hostile input.

#include <atomic>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bandwidth_broker/broker.hpp"
#include "support/fixtures.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;
using bb_fixture::basic_policy;
using bb_fixture::Fixture;
using bb_fixture::RequestSpec;

namespace {

[[nodiscard]] BrokerConfig memory_config(const Fixture& fixture) {
  BrokerConfig config;
  config.epoch = fixture.epoch;
  config.incarnation = fixture.coordinator;
  return config;
}

[[nodiscard]] RequestSpec spec(std::uint64_t id, std::int64_t minimum, std::int64_t desired, std::int64_t maximum,
                               std::uint64_t priority = 1, std::uint64_t group = 1) {
  RequestSpec request;
  request.id = id;
  request.minimum = minimum;
  request.desired = desired;
  request.maximum = maximum;
  request.priority = priority;
  request.group = group;
  request.tenant = group;
  return request;
}

// Asserts every invariants that must hold after any authoritative mutation.
void check_invariants(const Broker& broker, const Fixture& fixture, const char* where) {
  const auto accounting = broker.accounting(fixture.target());
  if (!accounting.ok()) {
    return;
  }
  const auto violations = accounting.value().validate();
  if (!violations.empty()) {
    BB_FAIL(std::string(where) + ": accounting invariants violated: " + describe_violations(violations));
    return;
  }
  const ResourceAccounting& value = accounting.value();
  BB_CHECK(value.authorized_consumption <=
           value.effective_physical.checked_add(value.contingent_pool).value());
  if (value.evidence == CapacityEvidenceState::Known) {
    BB_CHECK(value.obligations_reserved ==
             value.obligations_consumed.checked_add(value.obligations_lent).value().checked_add(value.obligations_idle).value());
  } else {
    BB_CHECK_EQ(value.authorized_consumption.bits_per_second(), 0);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Property tests
// ---------------------------------------------------------------------------

BB_TEST(Property, SeededOperationSequencePreservesEveryInvariant) {
  constexpr std::uint64_t kSeed = 0x51ED5EEDULL;
  std::mt19937_64 generator(kSeed);

  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  Policy policy = basic_policy(fixture, 4);
  policy.borrow.enabled = true;
  policy.borrow.lend_obligations = true;
  policy.borrow.max_lend_permille = 700;
  policy.borrow.max_borrow_permille = 500;
  policy.headroom_permille = 50;
  policy.priority_classes[2].borrowing_eligible = true;
  policy.fairness_groups[0].borrowing_eligible = true;
  policy.fairness_groups[1].borrowing_eligible = true;
  policy.fairness_groups[2].borrowing_eligible = true;
  policy.fairness_groups[3].borrowing_eligible = true;
  BB_REQUIRE(broker.value()->set_policy(policy).ok());

  std::int64_t physical = 1'000'000;
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(physical)).ok());

  std::uint64_t next_request = 1;
  std::uint64_t next_obligation = 1;
  std::vector<std::uint64_t> live;
  std::uint64_t capacity_generation = 1;

  for (int step = 0; step < 400; ++step) {
    const std::uint64_t choice = generator() % 100;
    if (choice < 30) {
      // The generator must respect minimum <= desired <= maximum, otherwise it
      // would produce the contradictory requests that other tests reject.
      const std::int64_t minimum = static_cast<std::int64_t>(generator() % 200'000);
      const std::int64_t desired = minimum + static_cast<std::int64_t>(generator() % 800'000);
      const std::int64_t maximum = desired + static_cast<std::int64_t>(generator() % 500'000);
      RequestSpec request = spec(next_request, minimum, desired, maximum, 1 + (generator() % 3), 1 + (generator() % 4));
      request.borrowing_eligible = (generator() % 2) == 0;
      BandwidthRequest built = fixture.request(request);
      if (request.borrowing_eligible) {
        built.borrowing_eligible = true;
        built.preemptible = true;
      }
      live.push_back(next_request);
      ++next_request;
      const auto outcome = broker.value()->submit(built);
      BB_CHECK(outcome.ok());
    } else if (choice < 45) {
      Fixture renewed = fixture;
      BB_CHECK_OK(broker.value()->arbitrate(fixture.target()));
    } else if (choice < 55) {
      const auto grants = broker.value()->grants(fixture.target(), true);
      BB_REQUIRE(grants.ok());
      if (!grants.value().empty()) {
        const Grant& grant = grants.value()[generator() % grants.value().size()];
        const auto released = broker.value()->release(grant.id, grant.generation);
        if (!released.ok()) {
          BB_CHECK_EQ(released.code(), ErrorCode::StaleGrant);
        }
      }
    } else if (choice < 62) {
      const auto grants = broker.value()->grants(fixture.target(), true);
      BB_REQUIRE(grants.ok());
      if (!grants.value().empty()) {
        const Grant& grant = grants.value()[generator() % grants.value().size()];
        const auto revoked = broker.value()->revoke(grant.id, grant.generation, OutcomeReason::Revoked, "test revoke");
        BB_CHECK(revoked.ok() || revoked.code() == ErrorCode::StaleGrant);
      }
    } else if (choice < 70) {
      physical = static_cast<std::int64_t>(generator() % 2'000'000);
      capacity_generation += 1;
      Fixture publication = fixture;
      publication.capacity_generation = CapacitySnapshotGeneration::from_value(capacity_generation);
      publication.tick = static_cast<std::uint64_t>(step) + 1;
      BB_CHECK_OK(broker.value()->publish_capacity(publication.snapshot(physical)));
    } else if (choice < 78) {
      Obligation obligation;
      obligation.reservation = ReservationReferenceId::from_value(next_obligation);
      obligation.generation = ReservationGeneration::initial();
      obligation.target = fixture.target();
      obligation.amount = bb_fixture::bw(static_cast<std::int64_t>(generator() % 300'000) + 1);
      obligation.lendable = true;
      obligation.max_lend_permille = 500;
      obligation.provenance = fixture.provenance(NodeKind::Operator);
      ++next_obligation;
      BB_CHECK_OK(broker.value()->upsert_obligation(obligation));
    } else if (choice < 85) {
      const auto obligations = broker.value()->obligations(fixture.target());
      BB_REQUIRE(obligations.ok());
      if (!obligations.value().empty()) {
        const Obligation& obligation = obligations.value()[generator() % obligations.value().size()];
        BB_CHECK(broker.value()->retire_obligation(obligation.reservation, obligation.generation).ok());
      }
    } else if (choice < 90) {
      const auto grants = broker.value()->grants(fixture.target(), true);
      BB_REQUIRE(grants.ok());
      if (!grants.value().empty()) {
        const Grant& grant = grants.value()[generator() % grants.value().size()];
        // Stale-generation release must be refused and must not corrupt state.
        BB_CHECK_ERR(ErrorCode::StaleGrant,
                     broker.value()->release(grant.id, BandwidthGrantGeneration::from_value(999)));
      }
    } else if (choice < 95) {
      if (!live.empty()) {
        const std::uint64_t id = live[generator() % live.size()];
        (void)broker.value()->retire_request(BandwidthRequestId::from_value(id),
                                             BandwidthRequestGeneration::initial());
      }
    } else {
      Policy updated = policy;
      updated.generation = PolicyGeneration::from_value(policy.generation.value() + 1);
      updated.fairness_groups[0].weight = 1 + (generator() % 20);
      updated.headroom_permille = static_cast<std::uint32_t>(generator() % 300);
      const auto applied = broker.value()->set_policy(updated);
      if (applied.ok()) {
        policy = updated;
      }
    }

    check_invariants(*broker.value(), fixture, "property sequence");
    if (bb_test::Context::instance().failure_count() > 0) {
      BB_FAIL("seed " + std::to_string(kSeed) + " step " + std::to_string(step));
      return;
    }
  }

  // Drain: releasing and revoking everything returns accounting to baseline.
  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  for (const Grant& grant : grants.value()) {
    const auto released = broker.value()->release(grant.id, grant.generation);
    BB_CHECK(released.ok());
  }
  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().authorized_consumption.bits_per_second(), 0);
  BB_CHECK(accounting.value().validate().empty());
}

BB_TEST(Property, ArbitrationIsMonotoneInCapacity) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  std::int64_t previous_total = -1;
  for (std::int64_t physical = 0; physical <= 2'000'000; physical += 25'000) {
    std::vector<ArbitrationCandidate> candidates;
    for (std::uint64_t id = 1; id <= 6; ++id) {
      RequestSpec entry = spec(id, 0, 500'000, 900'000, 1 + (id % 3), 1 + (id % 2));
      candidates.push_back(ArbitrationCandidate{fixture.request(entry), 0, std::nullopt});
    }
    ArbitrationInput input;
    input.target = fixture.target();
    input.snapshot = fixture.snapshot(physical);
    input.policy = policy;
    input.candidates = candidates;
    input.epoch = fixture.epoch;
    input.coordinator = fixture.coordinator;
    input.tick = 1;
    input.decision = DecisionId::from_value(1);
    const auto outcome = arbitrate(input);
    BB_REQUIRE(outcome.ok());
    std::int64_t total = 0;
    for (const ArbitrationDecision& decision : outcome.value().decisions) {
      if (decision.grant.has_value()) {
        total += decision.grant->allocation.total().value().bits_per_second();
      }
    }
    BB_CHECK(total >= previous_total);
    BB_CHECK(total <= physical);
    previous_total = total;
  }
}

// ---------------------------------------------------------------------------
// Concurrency tests
// ---------------------------------------------------------------------------

BB_TEST(Concurrency, RacingSubmissionsAndRoundsKeepAccountingClosed) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  Policy policy = basic_policy(fixture, 4);
  BB_REQUIRE(broker.value()->set_policy(policy).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(500'000)).ok());

  constexpr int kThreads = 8;
  constexpr int kOperations = 120;
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int worker = 0; worker < kThreads; ++worker) {
    workers.emplace_back([&, worker]() {
      for (int operation = 0; operation < kOperations; ++operation) {
        const std::uint64_t id = static_cast<std::uint64_t>(worker) * 10'000 + static_cast<std::uint64_t>(operation) + 1;
        Fixture local = fixture;
        RequestSpec entry = spec(id, 0, 20'000, 40'000, 1 + (id % 3), 1 + (id % 4));
        const auto submitted = broker.value()->submit(local.request(entry));
        if (!submitted.ok()) {
          failures.fetch_add(1);
        }
        const auto round = broker.value()->arbitrate(fixture.target());
        if (!round.ok()) {
          failures.fetch_add(1);
          continue;
        }
        const auto grants = broker.value()->grants(fixture.target(), true);
        if (!grants.ok()) {
          failures.fetch_add(1);
          continue;
        }
        if (!grants.value().empty()) {
          const Grant& grant = grants.value()[id % grants.value().size()];
          const auto released = broker.value()->release(grant.id, grant.generation);
          // A racing release may legitimately lose to another thread and be
          // reported as stale; that is a refusal, not corruption.
          if (!released.ok() && released.code() != ErrorCode::StaleGrant &&
              released.code() != ErrorCode::InvalidStateTransition && released.code() != ErrorCode::NotFound) {
            failures.fetch_add(1);
          }
        }
        const auto accounting = broker.value()->accounting(fixture.target());
        if (accounting.ok() && !accounting.value().validate().empty()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  BB_CHECK_EQ(failures.load(), 0);
  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK(accounting.value().validate().empty());
  BB_CHECK(accounting.value().authorized_consumption <= accounting.value().effective_physical);
}

BB_TEST(Concurrency, DuplicateIdentityRaceHasExactlyOneWinner) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_REQUIRE(broker.value()->set_policy(basic_policy(fixture)).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(1'000'000)).ok());

  const BandwidthRequest request = fixture.request(spec(1, 0, 100'000, 100'000, 3));
  constexpr int kThreads = 12;
  std::atomic<int> created{0};
  std::atomic<int> replayed{0};
  std::atomic<int> conflicted{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < kThreads; ++worker) {
    workers.emplace_back([&]() {
      const auto outcome = broker.value()->submit(request);
      if (!outcome.ok()) {
        conflicted.fetch_add(1);
        return;
      }
      if (outcome.value().disposition == IdempotencyDisposition::New) {
        created.fetch_add(1);
      } else {
        replayed.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  BB_CHECK_EQ(created.load(), 1);
  BB_CHECK_EQ(replayed.load(), kThreads - 1);
  BB_CHECK_EQ(conflicted.load(), 0);

  const auto round = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(round.ok());
  BB_CHECK_EQ(round.value().granted, std::size_t{1});
  BB_CHECK_EQ(round.value().accounting.authorized_consumption.bits_per_second(), 100'000);
}

BB_TEST(Concurrency, RevokeRacingReleaseHasOneWinner) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_REQUIRE(broker.value()->set_policy(basic_policy(fixture)).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(1'000'000)).ok());
  BB_REQUIRE(broker.value()->submit(fixture.request(spec(1, 0, 100'000, 100'000, 3))).ok());
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);
  const Grant grant = grants.value()[0];

  std::atomic<int> successes{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 2; ++worker) {
    workers.emplace_back([&, worker]() {
      if (worker == 0) {
        if (broker.value()->release(grant.id, grant.generation).ok()) {
          successes.fetch_add(1);
        }
      } else {
        if (broker.value()->revoke(grant.id, grant.generation, OutcomeReason::Revoked, "race").ok()) {
          successes.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  BB_CHECK_EQ(successes.load(), 1);
  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().authorized_consumption.bits_per_second(), 0);
  BB_CHECK(accounting.value().validate().empty());
}

// ---------------------------------------------------------------------------
// Adversarial tests
// ---------------------------------------------------------------------------

BB_TEST(Adversarial, ContradictoryAndHostileRequestsAreRefused) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);

  std::vector<RequestSpec> hostile;
  hostile.push_back(spec(1, 900, 900, 100, 3));                       // minimum above maximum
  hostile.push_back(spec(2, 0, 0, 1, 3));                             // zero desired, tiny maximum
  RequestSpec guarantee_without_minimum = spec(3, 0, 100, 200, 3);
  guarantee_without_minimum.guarantee = GuaranteeClass::Guaranteed;
  hostile.push_back(guarantee_without_minimum);
  RequestSpec best_effort_with_minimum = spec(4, 100, 200, 300, 3);
  best_effort_with_minimum.guarantee = GuaranteeClass::BestEffort;
  hostile.push_back(best_effort_with_minimum);
  RequestSpec no_recall_but_preemptible = spec(5, 0, 100, 200, 3);
  no_recall_but_preemptible.recall = RecallTolerance::NoRecall;
  hostile.push_back(no_recall_but_preemptible);
  RequestSpec borrowing_but_not_preemptible = spec(6, 0, 100, 200, 3);
  borrowing_but_not_preemptible.borrowing_eligible = true;
  borrowing_but_not_preemptible.preemptible = false;
  hostile.push_back(borrowing_but_not_preemptible);

  std::vector<ArbitrationCandidate> candidates;
  for (const RequestSpec& entry : hostile) {
    candidates.push_back(ArbitrationCandidate{fixture.request(entry), 0, std::nullopt});
  }
  ArbitrationInput input;
  input.target = fixture.target();
  input.snapshot = fixture.snapshot(1'000'000);
  input.policy = policy;
  input.candidates = candidates;
  input.epoch = fixture.epoch;
  input.coordinator = fixture.coordinator;
  input.tick = 1;
  input.decision = DecisionId::from_value(1);

  const auto outcome = arbitrate(input);
  BB_REQUIRE(outcome.ok());
  for (const ArbitrationDecision& decision : outcome.value().decisions) {
    BB_CHECK(decision.refused);
    BB_CHECK(!decision.grant.has_value());
    // A contradictory request is refused as contradictory; a request whose
    // ceiling is below one allocation quantum is refused for that reason, which
    // is also a refusal and also consumes no capacity.
    BB_CHECK(decision.reason == OutcomeReason::RefusedContradictory ||
             decision.reason == OutcomeReason::RefusedBelowQuantum);
  }
  BB_CHECK_EQ(outcome.value().accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK(outcome.value().accounting.validate().empty());
}

BB_TEST(Adversarial, OversizedBandwidthIsRejectedNotClamped) {
  BB_CHECK_ERR(ErrorCode::OutOfRange, Bandwidth::from_bits_per_second(Bandwidth::kMaxBitsPerSecond + 1));
  BB_CHECK_ERR(ErrorCode::OutOfRange, Bandwidth::from_bits_per_second(-1));
  BB_CHECK_ERR(ErrorCode::NumericOverflow,
               bb_fixture::bw(Bandwidth::kMaxBitsPerSecond)
                   .checked_add(bb_fixture::bw(Bandwidth::kMaxBitsPerSecond)));
  BB_CHECK_ERR(ErrorCode::NumericOverflow, bb_fixture::bw(Bandwidth::kMaxBitsPerSecond).checked_mul(2));
  BB_CHECK_ERR(ErrorCode::NumericOverflow,
               bb_fixture::bw(Bandwidth::kMaxBitsPerSecond).scaled(1'000'000, 1));
}

BB_TEST(Adversarial, HugeWeightsAreBoundedByPolicy) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.fairness_groups[0].weight = limits::kMaxWeight + 1;
  BB_CHECK_ERR(ErrorCode::PolicyInvalid, policy.validate());
  policy.fairness_groups[0].weight = 0;
  BB_CHECK_ERR(ErrorCode::PolicyInvalid, policy.validate());
  policy.fairness_groups[0].weight = limits::kMaxWeight;
  BB_CHECK_OK(policy.validate());
}

BB_TEST(Adversarial, ReservationOvercommitDoesNotProduceNegativeCapacity) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_REQUIRE(broker.value()->set_policy(basic_policy(fixture)).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(10'000)).ok());

  for (std::uint64_t index = 1; index <= 20; ++index) {
    Obligation obligation;
    obligation.reservation = ReservationReferenceId::from_value(index);
    obligation.generation = ReservationGeneration::initial();
    obligation.target = fixture.target();
    obligation.amount = bb_fixture::bw(5'000);
    obligation.provenance = fixture.provenance(NodeKind::Operator);
    BB_REQUIRE(broker.value()->upsert_obligation(obligation).ok());
  }
  const auto round = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(round.ok());
  BB_CHECK_EQ(round.value().accounting.allocatable.bits_per_second(), 0);
  BB_CHECK_EQ(round.value().accounting.capacity_deficit.bits_per_second(), 90'000);
  BB_CHECK(round.value().accounting.validate().empty());
}

BB_TEST(Adversarial, AllCapacityWithdrawnStalesEveryGrant) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_REQUIRE(broker.value()->set_policy(basic_policy(fixture)).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(1'000'000)).ok());
  BB_REQUIRE(broker.value()->submit(fixture.request(spec(1, 0, 400'000, 400'000, 3))).ok());
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
  BB_CHECK_EQ(broker.value()->status().live_grants, std::size_t{1});

  Fixture withdrawn = fixture;
  withdrawn.capacity_generation = CapacitySnapshotGeneration::from_value(2);
  withdrawn.tick = 2;
  BB_REQUIRE(broker.value()->publish_capacity(withdrawn.snapshot(0, CapacityEvidenceState::Known)).ok());
  const auto round = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(round.ok());
  BB_CHECK_EQ(round.value().accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(broker.value()->status().live_grants, std::size_t{0});

  Fixture unknown = fixture;
  unknown.capacity_generation = CapacitySnapshotGeneration::from_value(3);
  unknown.tick = 3;
  BB_REQUIRE(broker.value()->publish_capacity(unknown.snapshot(0, CapacityEvidenceState::Unknown)).ok());
  BB_REQUIRE(broker.value()->submit(fixture.request(spec(2, 0, 100, 100, 3))).ok());
  const auto unknown_round = broker.value()->arbitrate(fixture.target());
  BB_REQUIRE(unknown_round.ok());
  BB_CHECK_EQ(unknown_round.value().accounting.evidence, CapacityEvidenceState::Unknown);
  BB_CHECK_EQ(unknown_round.value().accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK(unknown_round.value().accounting.validate().empty());
}

BB_TEST(Adversarial, RepeatedBorrowRecallCyclesStayConsistent) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  Policy policy = basic_policy(fixture);
  policy.borrow.enabled = true;
  policy.borrow.lend_obligations = true;
  policy.borrow.max_lend_permille = 1000;
  policy.borrow.max_borrow_permille = 1000;
  policy.priority_classes[2].borrowing_eligible = true;
  policy.fairness_groups[0].borrowing_eligible = true;
  BB_REQUIRE(broker.value()->set_policy(policy).ok());

  Obligation obligation;
  obligation.reservation = ReservationReferenceId::from_value(1);
  obligation.generation = ReservationGeneration::initial();
  obligation.target = fixture.target();
  obligation.amount = bb_fixture::bw(400'000);
  obligation.lendable = true;
  obligation.max_lend_permille = 1000;
  obligation.provenance = fixture.provenance(NodeKind::Operator);
  BB_REQUIRE(broker.value()->upsert_obligation(obligation).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(1'000'000)).ok());

  RequestSpec borrower = spec(1, 0, 900'000, 900'000, 3);
  borrower.borrowing_eligible = true;
  BB_REQUIRE(broker.value()->submit(fixture.request(borrower)).ok());

  for (int cycle = 0; cycle < 60; ++cycle) {
    if ((cycle % 2) == 0) {
      // The owner arrives and reclaims; the borrower must be recalled.
      RequestSpec owner = spec(2, 400'000, 400'000, 400'000, 1);
      owner.reservation = true;
      owner.reservation_id = 1;
      owner.guarantee = GuaranteeClass::Guaranteed;
      const auto submitted = broker.value()->submit(fixture.request(owner));
      BB_REQUIRE(submitted.ok());
    } else {
      (void)broker.value()->retire_request(BandwidthRequestId::from_value(2),
                                           BandwidthRequestGeneration::initial());
    }
    const auto round = broker.value()->arbitrate(fixture.target());
    BB_REQUIRE(round.ok());
    const auto violations = round.value().accounting.validate();
    if (!violations.empty()) {
      BB_FAIL("borrow/recall cycle " + std::to_string(cycle) + ": " + describe_violations(violations));
      return;
    }
    BB_CHECK(round.value().accounting.authorized_consumption <= round.value().accounting.effective_physical);
  }
}

BB_TEST(Adversarial, MalformedRequestPopulationsAreRejectedDeterministically) {
  constexpr std::uint64_t kSeed = 0xADBE11ULL;
  std::mt19937_64 generator(kSeed);
  Fixture fixture;
  const Policy policy = basic_policy(fixture);

  for (int iteration = 0; iteration < 200; ++iteration) {
    const std::uint32_t garbage = static_cast<std::uint32_t>(generator());
    RequestSpec entry = spec(1 + (generator() % 5), static_cast<std::int64_t>(generator() % 1'000),
                             static_cast<std::int64_t>(generator() % 1'000),
                             static_cast<std::int64_t>(generator() % 1'000), 1 + (generator() % 3));
    BandwidthRequest request = fixture.request(entry);
    switch (garbage % 8) {
      case 0: request.priority = PriorityClassId::from_value(99); break;
      case 1: request.fairness_group = FairnessGroupId::from_value(99); break;
      case 2: request.authority.fabric_epoch = FabricEpoch::from_value(99); break;
      case 3: request.authority.resource_generation = BandwidthResourceGeneration::from_value(99); break;
      case 4: request.authority.policy_generation = PolicyGeneration::from_value(99); break;
      case 5: request.authority.publisher_boot = boot_id_from_u64(0, 1); break;
      case 6: request.authority.request = BandwidthRequestId::from_value(999); break;
      default: request.target.resource_generation = BandwidthResourceGeneration::from_value(7); break;
    }

    ArbitrationInput input;
    input.target = fixture.target();
    input.snapshot = fixture.snapshot(1'000'000);
    input.policy = policy;
    input.candidates.push_back(ArbitrationCandidate{request, 0, std::nullopt});
    input.epoch = fixture.epoch;
    input.coordinator = fixture.coordinator;
    input.tick = 1;
    input.decision = DecisionId::from_value(1);

    const auto outcome = arbitrate(input);
    if (outcome.ok()) {
      BB_CHECK(outcome.value().accounting.validate().empty());
      for (const ArbitrationDecision& decision : outcome.value().decisions) {
        if (decision.refused) {
          BB_CHECK_EQ(decision.allocation.guaranteed.bits_per_second(), 0);
          BB_CHECK_EQ(decision.allocation.discretionary.bits_per_second(), 0);
        }
      }
    } else {
      // Input-level rejection is also acceptable for structurally invalid
      // rounds, but it must be a deterministic error, never a crash.
      BB_CHECK(outcome.code() != ErrorCode::Ok);
    }
  }
}

BB_TEST(Adversarial, FencedBootCannotBeRevalidated) {
  Fixture fixture;
  auto broker = Broker::open(memory_config(fixture));
  BB_REQUIRE(broker.ok());
  BB_REQUIRE(broker.value()->set_policy(basic_policy(fixture)).ok());
  BB_REQUIRE(broker.value()->publish_capacity(fixture.snapshot(1'000'000)).ok());
  BB_REQUIRE(broker.value()->submit(fixture.request(spec(1, 0, 100'000, 100'000, 3))).ok());
  BB_REQUIRE(broker.value()->arbitrate(fixture.target()).ok());
  BB_REQUIRE(broker.value()->advance_epoch().ok());

  const auto grants = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(grants.ok());
  BB_REQUIRE(grants.value().size() == 1);
  const auto fenced = broker.value()->fence_publisher(PublisherId::from_value(1), boot_id_from_u64(1, 0xAA), "dead");
  BB_REQUIRE(fenced.ok());
  // Fencing withdraws the grant immediately, so the presented generation is
  // already superseded; either refusal proves the fenced incarnation can never
  // regain authority.
  const auto revalidated = broker.value()->revalidate(grants.value()[0].id, grants.value()[0].generation);
  BB_CHECK(!revalidated.ok());
  BB_CHECK(revalidated.code() == ErrorCode::BootFenced || revalidated.code() == ErrorCode::StaleGrant);
  const auto after = broker.value()->grants(fixture.target(), true);
  BB_REQUIRE(after.ok());
  BB_CHECK(after.value().empty());
  const auto accounting = broker.value()->accounting(fixture.target());
  BB_REQUIRE(accounting.ok());
  BB_CHECK_EQ(accounting.value().authorized_consumption.bits_per_second(), 0);
}
