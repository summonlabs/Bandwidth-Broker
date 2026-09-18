// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Explanations.
//
// Every request outcome is explainable without re-running arbitration: the
// explanation carries the exact request, the exact resource and generations,
// the effective capacity and obligations that were applied, the fairness group
// and priority, the guaranteed/borrowed/denied breakdown, whether the request
// is waiting or refused, the binding reason, the recall status, provenance and
// the full authority vector.
//
// Explanations are bounded: every string is capped and the label list is
// capped, so an explanation can never be used to amplify memory or bandwidth.

#ifndef BANDWIDTH_BROKER_EXPLANATION_HPP
#define BANDWIDTH_BROKER_EXPLANATION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "bandwidth_broker/accounting.hpp"
#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/grant.hpp"
#include "bandwidth_broker/request.hpp"

namespace bandwidth_broker {

struct BB_API RequestExplanation final {
  // Identity of the request being explained.
  BandwidthRequestId request{};
  BandwidthRequestGeneration request_generation{};
  CapacityTarget target{};

  GrantState state{GrantState::Rejected};
  OutcomeReason reason{OutcomeReason::None};
  bool satisfied{false};
  bool waiting{false};
  bool refused{false};

  // Authority actually used, with the coordinator-held bindings filled in.
  AuthorityVector authority{};
  FabricEpoch epoch{};
  CoordinatorIncarnation coordinator{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  DecisionId decision{};
  std::uint64_t decision_tick{0};

  // Capacity view at the moment of the decision.
  CapacityEvidenceState evidence{CapacityEvidenceState::Unknown};
  CapacitySnapshotGeneration capacity_generation{};
  Bandwidth effective_physical{};
  Bandwidth obligations_applied{};
  Bandwidth allocatable{};
  Bandwidth headroom_preserved{};
  Bandwidth emergency_reserve_preserved{};
  Bandwidth arbitrable{};

  // Outcome breakdown.
  Bandwidth requested_minimum{};
  Bandwidth requested_desired{};
  Bandwidth requested_maximum{};
  Bandwidth guaranteed{};
  Bandwidth discretionary{};
  Bandwidth borrowed{};
  Bandwidth contingent{};
  Bandwidth total_granted{};
  Bandwidth denied{};

  // Admission context.
  PriorityClassId priority{};
  FairnessGroupId fairness_group{};
  TenantId tenant{};
  std::uint32_t effective_rank{0};
  std::uint64_t effective_weight{1};
  bool starvation_promoted{false};
  Bandwidth group_cap{};
  Bandwidth group_allocated{};
  std::uint64_t wait_rounds{0};

  // Recall status.
  bool recall_pending{false};
  Bandwidth recalled{};
  RecallId recall{};
  OutcomeReason recall_reason{OutcomeReason::None};
  std::uint64_t recall_deadline_tick{0};

  std::string binding_reason;
  std::string authority_mismatch;

  BandwidthResourceGeneration resource_generation{};
  BandwidthGrantId grant{};
  BandwidthGrantGeneration grant_generation{};

  Provenance provenance{};

  [[nodiscard]] Status validate() const;
};

[[nodiscard]] BB_API Status validate_explanation(const RequestExplanation& explanation);

// Bounded, human-readable rendering used by the inspection tool and by logs.
[[nodiscard]] BB_API std::string describe_explanation(const RequestExplanation& explanation);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_EXPLANATION_HPP
