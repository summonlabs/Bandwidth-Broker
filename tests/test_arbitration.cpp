// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof obligations for arbitration: guarantees, caps, fairness, borrowing,
// recall, staleness rejection, authority and accounting closure.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "support/round_harness.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;
using bb_fixture::basic_policy;
using bb_fixture::Fixture;
using bb_fixture::RequestSpec;
using bb_round::RoundOptions;
using bb_round::run_round;

namespace {

RequestSpec spec(std::uint64_t id, std::int64_t minimum, std::int64_t desired, std::int64_t maximum,
                 std::uint64_t priority = 1, std::uint64_t group = 1, std::uint64_t tenant = 1) {
  RequestSpec request;
  request.id = id;
  request.minimum = minimum;
  request.desired = desired;
  request.maximum = maximum;
  request.priority = priority;
  request.group = group;
  request.tenant = tenant;
  return request;
}

RoundOptions requests(const Fixture& fixture, std::vector<RequestSpec> specs) {
  RoundOptions options;
  for (const RequestSpec& entry : specs) {
    options.candidates.push_back(bb_round::candidate(fixture.request(entry)));
  }
  return options;
}

}  // namespace

BB_TEST(Arbitration, StrictPriorityServesStrongerClassFirst) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 0, 1000, 1000, /*priority=*/1), spec(2, 0, 1000, 1000, 3)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 1000);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 0);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 1), OutcomeReason::ZeroCapacityAvailable);
}

BB_TEST(Arbitration, LowerClassReceivesWhatStrongerClassDoesNotWant) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 0, 400, 400, 1), spec(2, 0, 300, 300, 3)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 300);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 400);
  BB_CHECK_EQ(outcome.accounting.unallocated.bits_per_second(), 300);
}

BB_TEST(Arbitration, GuaranteedMinimaAreReservedBeforeDiscretionary) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  // The weakest class declares a 600 bps guarantee; the strongest class asks
  // for everything. The guarantee must be reserved first.
  RoundOptions options = requests(fixture, {spec(1, 0, 1000, 1000, 3), spec(2, 600, 600, 600, 1)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::guaranteed(outcome, 2), 600);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 400);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 1), OutcomeReason::PartiallySatisfied);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 2), OutcomeReason::SatisfiedFully);
}

BB_TEST(Arbitration, GuaranteedMinimumExceedingCapacityIsReportedAsWaiting) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 800, 800, 800, 3), spec(2, 800, 800, 800, 2)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  // The stronger guarantee is met first; the weaker one is explicitly waiting.
  BB_CHECK_EQ(bb_round::guaranteed(outcome, 1), 800);
  BB_CHECK_EQ(bb_round::guaranteed(outcome, 2), 200);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 2), OutcomeReason::BelowMinimumWaiting);
  BB_CHECK(!bb_round::find_decision(outcome, 2)->satisfied);
}

BB_TEST(Arbitration, NeverAllocatesAboveMaximumNorBelowMinimumWhileSatisfied) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 100, 900, 1000, 2), spec(2, 100, 900, 1000, 2)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  for (const ArbitrationDecision& decision : outcome.decisions) {
    BB_REQUIRE(decision.grant.has_value());
    const auto total = decision.grant->allocation.total();
    BB_REQUIRE(total.ok());
    BB_CHECK(total.value() <= decision.requested_maximum);
    if (decision.satisfied) {
      BB_CHECK(total.value() >= decision.requested_minimum);
    }
  }
}

BB_TEST(Arbitration, WeightedFairShareHonoursGroupWeights) {
  Fixture fixture;
  Policy policy = basic_policy(fixture, 2, SchedulingMode::WeightedFairShare);
  policy.fairness_groups[0].weight = 1;
  policy.fairness_groups[1].weight = 3;
  RoundOptions options = requests(fixture, {spec(1, 0, 10'000, 10'000, 2, 1, 1), spec(2, 0, 10'000, 10'000, 2, 2, 2)});
  options.physical = 800;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 200);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 600);
}

BB_TEST(Arbitration, StrictPrioritySharesInsideAClassByGroupWeight) {
  Fixture fixture;
  Policy policy = basic_policy(fixture, 2);
  policy.fairness_groups[0].weight = 1;
  policy.fairness_groups[1].weight = 1;
  RoundOptions options = requests(fixture, {spec(1, 0, 10'000, 10'000, 2, 1, 1), spec(2, 0, 10'000, 10'000, 2, 2, 2)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 500);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 500);
}

BB_TEST(Arbitration, GroupCapIsEnforced) {
  Fixture fixture;
  Policy policy = basic_policy(fixture, 2);
  policy.fairness_groups[0].maximum_cap = bb_fixture::bw(120);
  policy.fairness_groups[1].maximum_cap = bb_fixture::bw(120);
  RoundOptions options = requests(fixture, {spec(1, 0, 1000, 1000, 2, 1, 1), spec(2, 0, 1000, 1000, 2, 2, 2)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 120);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 120);
  BB_CHECK_EQ(outcome.accounting.unallocated.bits_per_second(), 760);
}

BB_TEST(Arbitration, HeadroomIsPreservedAndNeverGranted) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.headroom_permille = 200;
  RoundOptions options = requests(fixture, {spec(1, 0, 10'000, 10'000, 3)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(outcome.accounting.headroom.bits_per_second(), 200);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 800);
}

BB_TEST(Arbitration, EmergencyReserveIsHeldForEmergencyClasses) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.emergency_reserve_permille = 300;
  policy.priority_classes[2].emergency = true;  // class 3, rank 30
  RoundOptions options = requests(fixture, {spec(1, 0, 10'000, 10'000, 3), spec(2, 0, 10'000, 10'000, 2)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(outcome.accounting.emergency_reserve.bits_per_second(), 300);
  // Class 3 is emergency and strongest: it takes the reserve plus the pool.
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 1000);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 0);
  BB_CHECK_EQ(outcome.accounting.emergency_reserve_unused.bits_per_second(), 0);
}

BB_TEST(Arbitration, ObligationsAreReservedAndConsumedByTheirHolder) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  Obligation obligation;
  obligation.reservation = ReservationReferenceId::from_value(11);
  obligation.generation = ReservationGeneration::initial();
  obligation.target = fixture.target();
  obligation.amount = bb_fixture::bw(400);
  obligation.provenance = fixture.provenance(NodeKind::Operator);

  RequestSpec bound = spec(1, 400, 400, 400, 3);
  bound.reservation = true;
  bound.reservation_id = 11;
  bound.guarantee = GuaranteeClass::Guaranteed;

  RoundOptions options = requests(fixture, {bound, spec(2, 0, 10'000, 10'000, 2)});
  options.physical = 1000;
  options.obligations = {obligation};
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(outcome.accounting.obligations_reserved.bits_per_second(), 400);
  BB_CHECK_EQ(outcome.accounting.obligations_consumed.bits_per_second(), 400);
  BB_CHECK_EQ(bb_round::guaranteed(outcome, 1), 400);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 600);
}

BB_TEST(Arbitration, UnknownCapacityGrantsNothingAndStalesExistingGrants) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 100, 500, 500, 3)});
  options.physical = 1000;
  options.evidence = CapacityEvidenceState::Unknown;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 0);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 1), OutcomeReason::CapacityUnknown);
  BB_CHECK_EQ(outcome.accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(outcome.accounting.effective_physical.bits_per_second(), 0);
  BB_CHECK_EQ(outcome.accounting.evidence, CapacityEvidenceState::Unknown);
}

BB_TEST(Arbitration, StaleResourceGenerationIsRefused) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  Fixture newer = fixture;
  newer.resource_generation = BandwidthResourceGeneration::from_value(2);
  RoundOptions options = requests(fixture, {spec(1, 0, 500, 500, 3)});
  options.physical = 1000;
  // The request was built against generation 1 while the snapshot is generation 2.
  ArbitrationInput input = bb_round::make_input(newer, policy, options);
  const auto outcome = arbitrate(input);
  BB_REQUIRE(outcome.ok());
  BB_CHECK_EQ(bb_round::reason_of(outcome.value(), 1), OutcomeReason::RefusedStaleResourceGeneration);
  BB_CHECK_EQ(bb_round::granted(outcome.value(), 1), 0);
  BB_CHECK(outcome.value().accounting.validate().empty());
}

BB_TEST(Arbitration, StalePolicyGenerationIsRefused) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 0, 500, 500, 3)});
  options.physical = 1000;
  policy.generation = PolicyGeneration::from_value(9);
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 1), OutcomeReason::RefusedStalePolicyGeneration);
}

BB_TEST(Arbitration, FencedBootIsRefused) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 0, 500, 500, 3)});
  options.physical = 1000;
  options.fenced_boots = {boot_id_from_u64(1, 0xAA)};
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 1), OutcomeReason::RefusedBootFenced);
}

BB_TEST(Arbitration, DuplicateIdentityIsRefusedDeterministically) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 0, 500, 500, 3), spec(1, 0, 500, 500, 3)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_REQUIRE(outcome.decisions.size() == 2);
  std::size_t refused = 0;
  std::size_t granted = 0;
  for (const ArbitrationDecision& decision : outcome.decisions) {
    if (decision.reason == OutcomeReason::RefusedDuplicateIdentity) {
      refused += 1;
    }
    if (decision.grant.has_value()) {
      granted += 1;
    }
  }
  BB_CHECK_EQ(refused, std::size_t{1});
  BB_CHECK_EQ(granted, std::size_t{1});
}

BB_TEST(Arbitration, ContradictoryRequestsAreRefusedWithReasons) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);

  RequestSpec min_above_max = spec(1, 900, 900, 500, 3);
  RequestSpec best_effort_with_minimum = spec(2, 100, 200, 300, 3);
  best_effort_with_minimum.guarantee = GuaranteeClass::BestEffort;
  RequestSpec borrowing_without_preemption = spec(3, 0, 100, 200, 3);
  borrowing_without_preemption.borrowing_eligible = true;
  borrowing_without_preemption.preemptible = false;

  RoundOptions options = requests(fixture, {min_above_max, best_effort_with_minimum, borrowing_without_preemption});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  for (std::uint64_t id = 1; id <= 3; ++id) {
    BB_CHECK_EQ(bb_round::reason_of(outcome, id), OutcomeReason::RefusedContradictory);
    BB_CHECK_EQ(bb_round::granted(outcome, id), 0);
  }
}

BB_TEST(Arbitration, BorrowingLendsUnusedLendableObligationCapacity) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.borrow.enabled = true;
  policy.borrow.lend_obligations = true;
  policy.borrow.max_lend_permille = 1000;
  policy.borrow.max_borrow_permille = 1000;
  policy.priority_classes[2].borrowing_eligible = true;

  Obligation obligation;
  obligation.reservation = ReservationReferenceId::from_value(21);
  obligation.generation = ReservationGeneration::initial();
  obligation.target = fixture.target();
  obligation.amount = bb_fixture::bw(500);
  obligation.lendable = true;
  obligation.max_lend_permille = 1000;
  obligation.provenance = fixture.provenance(NodeKind::Operator);

  RequestSpec borrower = spec(2, 0, 900, 900, 3);
  borrower.borrowing_eligible = true;

  RoundOptions options = requests(fixture, {borrower});
  options.physical = 1000;
  options.obligations = {obligation};
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);

  // 500 bps remain arbitrable after the obligation; the borrower takes all of it
  // as discretionary capacity and then borrows 400 bps of the unused, lendable
  // obligation. Borrowed capacity stays distinct from guaranteed capacity.
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 900);
  BB_CHECK_EQ(bb_round::borrowed(outcome, 2), 400);
  BB_CHECK_EQ(outcome.accounting.borrowed_granted.bits_per_second(), 400);
  BB_CHECK_EQ(outcome.accounting.obligations_lent.bits_per_second(), 400);
  BB_CHECK_EQ(outcome.accounting.obligations_idle.bits_per_second(), 100);
  BB_CHECK_EQ(outcome.accounting.authorized_consumption.bits_per_second(), 900);
}

BB_TEST(Arbitration, BorrowedCapacityIsDistinctFromGuaranteedCapacity) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.borrow.enabled = true;
  policy.borrow.lend_obligations = true;
  policy.borrow.max_lend_permille = 1000;
  policy.borrow.max_borrow_permille = 1000;
  policy.priority_classes[2].borrowing_eligible = true;

  Obligation obligation;
  obligation.reservation = ReservationReferenceId::from_value(31);
  obligation.generation = ReservationGeneration::initial();
  obligation.target = fixture.target();
  obligation.amount = bb_fixture::bw(600);
  obligation.lendable = true;
  obligation.max_lend_permille = 1000;
  obligation.provenance = fixture.provenance(NodeKind::Operator);

  // The owner never claims the obligation, so all of it is lendable. With only
  // 100 bps arbitrable, the borrower must reach into the lendable pool.
  RequestSpec borrower = spec(1, 0, 500, 500, 3);
  borrower.borrowing_eligible = true;

  RoundOptions options = requests(fixture, {borrower});
  options.physical = 700;
  options.obligations = {obligation};
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);

  BB_CHECK_EQ(bb_round::granted(outcome, 1), 500);
  BB_CHECK_EQ(outcome.accounting.borrowed_granted.bits_per_second(), 400);
  BB_CHECK_EQ(outcome.accounting.obligations_lent.bits_per_second(), 400);
  BB_CHECK_EQ(outcome.accounting.obligations_idle.bits_per_second(), 200);
  BB_CHECK_EQ(outcome.accounting.authorized_consumption.bits_per_second(), 500);
}

BB_TEST(Arbitration, ShrinkingAllocationEmitsRecallAndAdvancesGeneration) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);

  // First round: the weak request holds 1000 bps.
  RoundOptions first = requests(fixture, {spec(1, 0, 1000, 1000, 1)});
  first.physical = 1000;
  const ArbitrationOutcome round_one = run_round(fixture, policy, first);
  BB_REQUIRE(round_one.decisions.size() == 1);
  BB_REQUIRE(round_one.decisions[0].grant.has_value());
  const Grant before = round_one.decisions[0].grant.value();
  BB_CHECK_EQ(before.allocation.discretionary.bits_per_second(), 1000);

  // Second round: a stronger request arrives and takes capacity away.
  Fixture second_fixture = fixture;
  second_fixture.tick = 2;
  second_fixture.next_grant_id = round_one.next_grant_id;
  second_fixture.next_recall_id = round_one.next_recall_id;
  second_fixture.next_decision_id = 2;

  RoundOptions second = requests(second_fixture, {spec(1, 0, 1000, 1000, 1), spec(2, 0, 600, 600, 3)});
  second.physical = 1000;
  ArbitrationCandidate& weak = second.candidates[0];
  weak.existing_grant = before;

  const ArbitrationOutcome round_two = run_round(second_fixture, policy, second);
  BB_CHECK_EQ(bb_round::granted(round_two, 2), 600);
  BB_CHECK_EQ(bb_round::granted(round_two, 1), 400);

  const ArbitrationDecision* weak_decision = bb_round::find_decision(round_two, 1);
  BB_REQUIRE(weak_decision != nullptr);
  BB_REQUIRE(weak_decision->recall.has_value());
  BB_CHECK_EQ(weak_decision->recall->recalled.bits_per_second(), 600);
  BB_CHECK_EQ(weak_decision->recall->remaining.bits_per_second(), 400);
  BB_REQUIRE(weak_decision->grant.has_value());
  BB_CHECK(weak_decision->grant->generation.value() > before.generation.value());
  BB_CHECK(weak_decision->grant->generation != before.generation);
}

BB_TEST(Arbitration, UnchangedGrantIsRenewedWithoutGenerationChurn) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions first = requests(fixture, {spec(1, 0, 500, 500, 3)});
  first.physical = 1000;
  const ArbitrationOutcome round_one = run_round(fixture, policy, first);
  BB_REQUIRE(round_one.decisions.size() == 1);
  const Grant before = round_one.decisions[0].grant.value();

  Fixture second_fixture = fixture;
  second_fixture.tick = 2;
  second_fixture.next_grant_id = round_one.next_grant_id;
  second_fixture.next_decision_id = 2;
  RoundOptions second = requests(second_fixture, {spec(1, 0, 500, 500, 3)});
  second.physical = 1000;
  second.candidates[0].existing_grant = before;

  const ArbitrationOutcome round_two = run_round(second_fixture, policy, second);
  BB_REQUIRE(round_two.decisions[0].grant.has_value());
  BB_CHECK(round_two.decisions[0].grant->generation == before.generation);
  BB_CHECK(round_two.decisions[0].grant->id == before.id);
  BB_CHECK_EQ(round_two.decisions[0].grant->last_revalidated_tick, std::uint64_t{2});
  BB_CHECK(!round_two.decisions[0].previous_grant.has_value());
}

BB_TEST(Arbitration, RevokedGrantReleasesItsCapacity) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions first = requests(fixture, {spec(1, 0, 700, 700, 3)});
  first.physical = 1000;
  const ArbitrationOutcome round_one = run_round(fixture, policy, first);
  const Grant before = round_one.decisions[0].grant.value();

  Fixture second_fixture = fixture;
  second_fixture.tick = 2;
  second_fixture.next_grant_id = round_one.next_grant_id;
  RoundOptions second;
  second.physical = 1000;
  ArbitrationCandidate entry = bb_round::candidate(second_fixture.request(spec(1, 0, 700, 700, 3)));
  entry.existing_grant = before;
  // The request is retracted by submitting it with a stale epoch instead.
  entry.request.authority.fabric_epoch = FabricEpoch::from_value(99);
  second.candidates.push_back(entry);

  const ArbitrationOutcome round_two = run_round(second_fixture, policy, second);
  BB_CHECK(is_outcome_refused(round_two.decisions[0].reason));
  BB_CHECK(round_two.decisions[0].grant.has_value());
  BB_CHECK_EQ(round_two.decisions[0].grant->state, GrantState::Stale);
  BB_CHECK_EQ(round_two.accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(round_two.accounting.unallocated.bits_per_second(), 1000);
}

BB_TEST(Arbitration, OversubscriptionIsExplicitAndContingent) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.oversubscription.numerator = 2;
  policy.oversubscription.denominator = 1;

  RequestSpec burst = spec(1, 0, 1600, 1600, 3);
  RoundOptions options = requests(fixture, {burst});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(outcome.accounting.contingent_pool.bits_per_second(), 1000);
  BB_CHECK_EQ(bb_round::contingent(outcome, 1), 600);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 1600);
  BB_CHECK_EQ(outcome.accounting.authorized_consumption.bits_per_second(), 1600);
  BB_CHECK_EQ(outcome.accounting.contingent_unallocated.bits_per_second(), 400);
}

BB_TEST(Arbitration, OversubscriptionDisabledByDefault) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 0, 5000, 5000, 3)});
  options.physical = 1000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(outcome.accounting.contingent_pool.bits_per_second(), 0);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 1000);
}

BB_TEST(Arbitration, RequestsBelowTheQuantumAreRefused) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.minimum_allocation_quantum = bb_fixture::bw(1000);
  RoundOptions options = requests(fixture, {spec(1, 0, 500, 500, 3)});
  options.physical = 10'000;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::reason_of(outcome, 1), OutcomeReason::RefusedBelowQuantum);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 0);
}

BB_TEST(Arbitration, AccountingClosesAcrossManyShapes) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  for (std::int64_t physical = 0; physical <= 4000; physical += 250) {
    for (std::int64_t headroom = 0; headroom <= 400; headroom += 200) {
      RoundOptions options = requests(fixture, {spec(1, 200, 900, 1500, 2), spec(2, 0, 700, 1200, 3),
                                                spec(3, 100, 100, 100, 1)});
      options.physical = physical;
      options.headroom_target = headroom;
      const ArbitrationOutcome outcome = run_round(fixture, policy, options);
      const ResourceAccounting& accounting = outcome.accounting;
      BB_CHECK(accounting.effective_physical <= bb_fixture::bw(physical));
      BB_CHECK(accounting.authorized_consumption <= accounting.effective_physical);
      BB_CHECK(accounting.unallocated <= accounting.arbitrable);
    }
  }
}

BB_TEST(Arbitration, IdenticalInputProducesIdenticalOutput) {
  Fixture fixture;
  Policy policy = basic_policy(fixture, 3);
  policy.borrow.enabled = true;
  policy.borrow.lend_obligations = true;
  policy.borrow.max_lend_permille = 500;
  policy.borrow.max_borrow_permille = 500;
  policy.headroom_permille = 100;
  policy.oversubscription.numerator = 3;
  policy.oversubscription.denominator = 2;
  policy.priority_classes[2].borrowing_eligible = true;

  RoundOptions options = requests(fixture, {spec(1, 100, 900, 1200, 2, 1, 1), spec(2, 0, 800, 800, 3, 2, 2),
                                            spec(3, 50, 400, 700, 1, 3, 3)});
  options.physical = 2000;

  const ArbitrationInput input = bb_round::make_input(fixture, policy, options);
  const auto first = arbitrate(input);
  const auto second = arbitrate(input);
  BB_REQUIRE(first.ok());
  BB_REQUIRE(second.ok());
  BB_REQUIRE(first.value().decisions.size() == second.value().decisions.size());
  for (std::size_t i = 0; i < first.value().decisions.size(); ++i) {
    BB_CHECK(first.value().decisions[i].request == second.value().decisions[i].request);
    BB_CHECK(first.value().decisions[i].reason == second.value().decisions[i].reason);
    BB_CHECK(first.value().decisions[i].allocation == second.value().decisions[i].allocation);
  }
  BB_CHECK_EQ(first.value().next_grant_id, second.value().next_grant_id);
}

BB_TEST(Arbitration, RequestOrderDoesNotChangeOutcomes) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture, 2);
  RoundOptions forward = requests(fixture, {spec(1, 0, 700, 700, 2, 1, 1), spec(2, 0, 700, 700, 2, 2, 2),
                                             spec(3, 0, 700, 700, 3, 1, 1)});
  forward.physical = 1000;
  RoundOptions reversed = forward;
  std::reverse(reversed.candidates.begin(), reversed.candidates.end());

  const ArbitrationOutcome first = run_round(fixture, policy, forward);
  const ArbitrationOutcome second = run_round(fixture, policy, reversed);
  for (std::uint64_t id = 1; id <= 3; ++id) {
    BB_CHECK_EQ(bb_round::granted(first, id), bb_round::granted(second, id));
    BB_CHECK_EQ(bb_round::reason_of(first, id), bb_round::reason_of(second, id));
  }
}

BB_TEST(Arbitration, ZeroCapacityGrantsNothing) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options = requests(fixture, {spec(1, 100, 500, 500, 3)});
  options.physical = 0;
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 0);
  BB_CHECK_EQ(outcome.accounting.authorized_consumption.bits_per_second(), 0);
  BB_CHECK_EQ(outcome.accounting.effective_physical.bits_per_second(), 0);
}

BB_TEST(Arbitration, ObligationsExceedingCapacityDoNotProduceNegativeAccounting) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  Obligation obligation;
  obligation.reservation = ReservationReferenceId::from_value(41);
  obligation.generation = ReservationGeneration::initial();
  obligation.target = fixture.target();
  obligation.amount = bb_fixture::bw(5000);
  obligation.provenance = fixture.provenance(NodeKind::Operator);

  RoundOptions options = requests(fixture, {spec(1, 0, 500, 500, 3)});
  options.physical = 1000;
  options.obligations = {obligation};
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(outcome.accounting.allocatable.bits_per_second(), 0);
  BB_CHECK_EQ(outcome.accounting.capacity_deficit.bits_per_second(), 4000);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 0);
}

BB_TEST(Arbitration, StarvationPromotionIsAppliedDeterministically) {
  Fixture fixture;
  Policy policy = basic_policy(fixture);
  policy.starvation.enabled = true;
  policy.starvation.aging_threshold_rounds = 2;
  policy.starvation.maximum_promotions = 2;
  policy.starvation.aging_weight_multiplier = 2;

  // The weak request has waited long enough to be promoted to the top rank.
  RoundOptions options;
  options.physical = 1000;
  ArbitrationCandidate weak = bb_round::candidate(fixture.request(spec(1, 0, 400, 400, 1)), 5);
  ArbitrationCandidate strong = bb_round::candidate(fixture.request(spec(2, 0, 400, 400, 3)), 0);
  options.candidates = {weak, strong};

  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 400);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 400);
  BB_CHECK(bb_round::find_decision(outcome, 1)->starvation_promoted);
  BB_CHECK(!bb_round::find_decision(outcome, 2)->starvation_promoted);
}

BB_TEST(Arbitration, WaitRoundsAccumulateWhileUnsatisfied) {
  Fixture fixture;
  const Policy policy = basic_policy(fixture);
  RoundOptions options;
  options.physical = 100;
  options.candidates = {bb_round::candidate(fixture.request(spec(1, 0, 1000, 1000, 2)), 7),
                        bb_round::candidate(fixture.request(spec(2, 0, 100, 100, 3)), 3)};
  const ArbitrationOutcome outcome = run_round(fixture, policy, options);
  // The unsatisfied request accumulates a wait round; the satisfied one resets.
  BB_CHECK_EQ(bb_round::find_decision(outcome, 1)->next_wait_rounds, std::uint64_t{8});
  BB_CHECK_EQ(bb_round::find_decision(outcome, 2)->next_wait_rounds, std::uint64_t{0});
  BB_CHECK_EQ(bb_round::granted(outcome, 1), 0);
  BB_CHECK_EQ(bb_round::granted(outcome, 2), 100);
}
