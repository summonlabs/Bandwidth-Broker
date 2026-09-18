// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The arbitration engine.
//
// arbitrate() is a pure function of its input. It performs no I/O, allocates no
// identity from any global source, invokes no callback and takes no lock. Given
// identical input it returns byte-identical output, which is what makes the
// whole runtime deterministic and property-testable.
//
// The engine never mutates authoritative state by itself: it computes the
// complete next state (grants, recalls, accounting and explanations) and the
// caller commits it or discards it.

#ifndef BANDWIDTH_BROKER_ARBITRATOR_HPP
#define BANDWIDTH_BROKER_ARBITRATOR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bandwidth_broker/accounting.hpp"
#include "bandwidth_broker/capacity.hpp"
#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/grant.hpp"
#include "bandwidth_broker/obligation.hpp"
#include "bandwidth_broker/policy.hpp"
#include "bandwidth_broker/request.hpp"

namespace bandwidth_broker {

// One competing request plus the durable scheduling state the coordinator holds
// for it. wait_rounds is authoritative state, not a hint: identical state must
// produce identical outcomes.
struct BB_API ArbitrationCandidate final {
  BandwidthRequest request{};
  std::uint64_t wait_rounds{0};

  // The grant currently held for this request, if any. The coordinator must
  // supply the complete record so that a decision can renew, reduce or revoke
  // it with full fidelity.
  std::optional<Grant> existing_grant{};
};

struct BB_API ArbitrationInput final {
  CapacityTarget target{};
  CapacitySnapshot snapshot{};
  Policy policy{};
  std::vector<Obligation> obligations{};
  std::vector<ArbitrationCandidate> candidates{};

  // Boot identities permanently fenced by this coordinator. A request whose
  // authority names a fenced boot is refused; stale frames from a dead process
  // can never authorise capacity again.
  std::vector<BootId> fenced_boots{};

  FabricEpoch epoch{};
  CoordinatorIncarnation coordinator{};
  std::uint64_t tick{0};
  DecisionId decision{};

  // Identity allocation for grants created by this round. The engine consumes
  // identifiers in a deterministic order and reports the next free value.
  std::uint64_t next_grant_id{1};
  std::uint64_t next_recall_id{1};
};

struct BB_API ArbitrationDecision final {
  BandwidthRequestId request{};
  BandwidthRequestGeneration request_generation{};

  GrantState state{GrantState::Rejected};
  OutcomeReason reason{OutcomeReason::None};
  GrantAllocation allocation{};

  Bandwidth requested_minimum{};
  Bandwidth requested_desired{};
  Bandwidth requested_maximum{};
  Bandwidth denied{};
  Bandwidth unmet_minimum{};

  bool satisfied{false};
  bool waiting{false};
  bool refused{false};
  bool starvation_promoted{false};

  // Scheduling state the coordinator must carry forward for this request.
  // Zero once the request has been satisfied in a round.
  std::uint64_t next_wait_rounds{0};

  std::uint32_t effective_rank{0};
  std::uint64_t effective_weight{1};

  std::string binding_reason;
  std::string authority_mismatch;

  // The authoritative grant after this decision, when one is held.
  std::optional<Grant> grant{};
  // The prior grant in its superseded state, when the request already held one.
  std::optional<Grant> previous_grant{};
  // The recall that justified reducing or withdrawing the previous grant.
  std::optional<RecallRecord> recall{};
};

struct BB_API ArbitrationOutcome final {
  DecisionId decision{};
  CapacityTarget target{};
  FabricEpoch epoch{};
  CoordinatorIncarnation coordinator{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  std::uint64_t tick{0};

  ResourceAccounting accounting{};
  std::vector<ArbitrationDecision> decisions{};

  std::uint64_t next_grant_id{1};
  std::uint64_t next_recall_id{1};
};

[[nodiscard]] BB_API Result<ArbitrationOutcome> arbitrate(const ArbitrationInput& input);

// Validation of an arbitration input, exposed so that callers can reject a
// malformed round before any state is touched.
[[nodiscard]] BB_API Status validate_arbitration_input(const ArbitrationInput& input);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_ARBITRATOR_HPP
