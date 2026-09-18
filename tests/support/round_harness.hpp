// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Helpers for driving a complete arbitration round in tests and asserting the
// accounting invariants after every round.

#ifndef BB_TESTS_SUPPORT_ROUND_HARNESS_HPP
#define BB_TESTS_SUPPORT_ROUND_HARNESS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_harness.hpp"

namespace bb_round {

using namespace bandwidth_broker;
using bb_fixture::bw;
using bb_fixture::Fixture;

struct RoundOptions final {
  std::int64_t physical{0};
  CapacityEvidenceState evidence{CapacityEvidenceState::Known};
  std::int64_t headroom_target{0};
  std::int64_t admin_unavailable{0};
  std::int64_t degraded{0};
  std::vector<Obligation> obligations{};
  std::vector<BootId> fenced_boots{};
  std::vector<ArbitrationCandidate> candidates{};
  std::uint64_t wait_rounds{0};
};

[[nodiscard]] inline ArbitrationInput make_input(const Fixture& fixture, const Policy& policy,
                                                 const RoundOptions& options) {
  ArbitrationInput input;
  input.target = fixture.target();
  input.snapshot = fixture.snapshot(options.physical, options.evidence, options.headroom_target,
                                    options.admin_unavailable, options.degraded);
  input.policy = policy;
  input.obligations = options.obligations;
  input.fenced_boots = options.fenced_boots;
  input.candidates = options.candidates;
  input.epoch = fixture.epoch;
  input.coordinator = fixture.coordinator;
  input.tick = fixture.tick;
  input.decision = DecisionId::from_value(fixture.next_decision_id);
  input.next_grant_id = fixture.next_grant_id;
  input.next_recall_id = fixture.next_recall_id;
  return input;
}

[[nodiscard]] inline ArbitrationCandidate candidate(const BandwidthRequest& request,
                                                   std::uint64_t wait_rounds = 0) {
  ArbitrationCandidate entry;
  entry.request = request;
  entry.wait_rounds = wait_rounds;
  return entry;
}

// Runs a round and asserts that the resulting accounting closes.
[[nodiscard]] inline ArbitrationOutcome run_round(const Fixture& fixture, const Policy& policy,
                                                  const RoundOptions& options) {
  const auto outcome = arbitrate(make_input(fixture, policy, options));
  if (!outcome.ok()) {
    BB_FAIL(std::string("arbitration failed: ") + to_string(outcome.code()) + " " + outcome.error().message);
    return ArbitrationOutcome{};
  }
  const auto violations = outcome.value().accounting.validate();
  if (!violations.empty()) {
    BB_FAIL("accounting invariants violated: " + describe_violations(violations));
  }
  return outcome.value();
}

[[nodiscard]] inline const ArbitrationDecision* find_decision(const ArbitrationOutcome& outcome,
                                                              std::uint64_t request_id) {
  for (const ArbitrationDecision& decision : outcome.decisions) {
    if (decision.request == BandwidthRequestId::from_value(request_id)) {
      return &decision;
    }
  }
  return nullptr;
}

[[nodiscard]] inline std::int64_t granted(const ArbitrationOutcome& outcome, std::uint64_t request_id) {
  const ArbitrationDecision* decision = find_decision(outcome, request_id);
  if (decision == nullptr || !decision->grant.has_value()) {
    return 0;
  }
  const auto total = decision->grant->allocation.total();
  return total.ok() ? total.value().bits_per_second() : -1;
}

[[nodiscard]] inline std::int64_t guaranteed(const ArbitrationOutcome& outcome, std::uint64_t request_id) {
  const ArbitrationDecision* decision = find_decision(outcome, request_id);
  if (decision == nullptr || !decision->grant.has_value()) {
    return 0;
  }
  return decision->grant->allocation.guaranteed.bits_per_second();
}

[[nodiscard]] inline std::int64_t borrowed(const ArbitrationOutcome& outcome, std::uint64_t request_id) {
  const ArbitrationDecision* decision = find_decision(outcome, request_id);
  if (decision == nullptr || !decision->grant.has_value()) {
    return 0;
  }
  return decision->grant->allocation.borrowed.bits_per_second();
}

[[nodiscard]] inline std::int64_t contingent(const ArbitrationOutcome& outcome, std::uint64_t request_id) {
  const ArbitrationDecision* decision = find_decision(outcome, request_id);
  if (decision == nullptr || !decision->grant.has_value()) {
    return 0;
  }
  return decision->grant->allocation.contingent.bits_per_second();
}

[[nodiscard]] inline OutcomeReason reason_of(const ArbitrationOutcome& outcome, std::uint64_t request_id) {
  const ArbitrationDecision* decision = find_decision(outcome, request_id);
  return decision == nullptr ? OutcomeReason::None : decision->reason;
}

[[nodiscard]] inline std::int64_t total_granted(const ArbitrationOutcome& outcome) {
  std::int64_t total = 0;
  for (const ArbitrationDecision& decision : outcome.decisions) {
    if (decision.grant.has_value()) {
      const auto value = decision.grant->allocation.total();
      if (value.ok()) {
        total += value.value().bits_per_second();
      }
    }
  }
  return total;
}

}  // namespace bb_round

#endif  // BB_TESTS_SUPPORT_ROUND_HARNESS_HPP
