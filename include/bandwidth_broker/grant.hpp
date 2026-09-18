// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Grant lifecycle and allocation breakdown.
//
// A grant is authoritative allocation state. It is not enforcement: adjacent
// runtimes (Rate Governor, Flow Scheduler, Traffic Engineering Fabric) apply it.
//
// Explicit lifecycle states are preserved, and every transition is checked
// against a transition table. Recall is a state transition, never deletion: a
// recalled grant keeps its identity, its generation and its history, and its
// old generation never authorises consumption again.

#ifndef BANDWIDTH_BROKER_GRANT_HPP
#define BANDWIDTH_BROKER_GRANT_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "bandwidth_broker/capacity.hpp"
#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/quantity.hpp"
#include "bandwidth_broker/request.hpp"

namespace bandwidth_broker {

enum class GrantState : std::uint8_t {
  Requested = 0,
  Validated = 1,
  Queued = 2,
  GrantedGuaranteed = 3,
  GrantedBorrowed = 4,
  RecallPending = 5,
  RevalidationRequired = 6,
  Released = 7,
  Expired = 8,
  Revoked = 9,
  Fenced = 10,
  Stale = 11,
  Rejected = 12
};

// How a granted amount is backed. The distinction is authoritative: only
// Guaranteed capacity carries the full authority semantics of its owner.
enum class AllocationKind : std::uint8_t {
  None = 0,
  Guaranteed = 1,     // at or below the request minimum inside its guarantee class
  Discretionary = 2,  // above the minimum, from arbitrable residual capacity
  Borrowed = 3,       // lent by an obligation whose owner has not consumed it
  Contingent = 4      // created by explicitly enabled controlled oversubscription
};

[[nodiscard]] BB_API const char* to_string(GrantState state) noexcept;
[[nodiscard]] BB_API const char* to_string(AllocationKind kind) noexcept;
[[nodiscard]] BB_API bool is_terminal(GrantState state) noexcept;
[[nodiscard]] BB_API bool is_live(GrantState state) noexcept;
[[nodiscard]] BB_API bool authorises_consumption(GrantState state) noexcept;
[[nodiscard]] BB_API bool is_legal_grant_transition(GrantState from, GrantState to) noexcept;

// Deterministic explanation of an arbitration outcome.
enum class OutcomeReason : std::uint8_t {
  None = 0,
  SatisfiedFully,
  SatisfiedAtMaximum,
  PartiallySatisfied,
  MinimumGuaranteedOnly,
  BelowMinimumWaiting,
  ZeroCapacityAvailable,
  CapacityUnknown,
  CapacityWithdrawn,
  HeadroomPreserved,
  EmergencyReservePreserved,
  GroupCapReached,
  FairnessLimited,
  BorrowPoolExhausted,
  OversubscriptionDisabled,
  WaitingForStrongerClass,
  DeferredByHysteresis,
  RecalledByStrongerClass,
  RecalledByObligationReturn,
  RecalledByCapacityReduction,
  RecalledByPolicyChange,
  RefusedContradictory,
  RefusedDuplicateIdentity,
  RefusedIdentityConflict,
  RefusedUnknownPriorityClass,
  RefusedUnknownFairnessGroup,
  RefusedTenantMismatch,
  RefusedStaleEpoch,
  RefusedStaleResourceGeneration,
  RefusedStalePolicyGeneration,
  RefusedStaleRequester,
  RefusedStaleReservation,
  RefusedStaleTenantConfig,
  RefusedBootFenced,
  RefusedBelowQuantum,
  RefusedNoCapacityAuthority,
  Expired,
  Released,
  Revoked,
  Fenced,
  Stale,
  RevalidationRequired
};

[[nodiscard]] BB_API const char* to_string(OutcomeReason reason) noexcept;
[[nodiscard]] BB_API bool is_outcome_waiting(OutcomeReason reason) noexcept;
[[nodiscard]] BB_API bool is_outcome_refused(OutcomeReason reason) noexcept;
[[nodiscard]] BB_API bool is_outcome_granted(OutcomeReason reason) noexcept;

// Explicit breakdown of a granted amount. Every authorised bit belongs to
// exactly one kind, and total() is the amount the grant authorises.
struct BB_API GrantAllocation final {
  Bandwidth guaranteed{};
  Bandwidth discretionary{};
  Bandwidth borrowed{};
  Bandwidth contingent{};

  [[nodiscard]] Result<Bandwidth> total() const;
  [[nodiscard]] Result<Bandwidth> revocable() const;
  [[nodiscard]] bool has_revocable() const noexcept;
  [[nodiscard]] bool is_zero() const noexcept {
    return guaranteed.is_zero() && discretionary.is_zero() && borrowed.is_zero() && contingent.is_zero();
  }
  [[nodiscard]] friend bool operator==(const GrantAllocation& a, const GrantAllocation& b) noexcept {
    return a.guaranteed == b.guaranteed && a.discretionary == b.discretionary && a.borrowed == b.borrowed &&
           a.contingent == b.contingent;
  }
};

struct BB_API RecallRecord final {
  RecallId id{};
  BandwidthGrantId grant{};
  BandwidthGrantGeneration grant_generation{};
  BandwidthRequestId request{};
  CapacityTarget target{};
  Bandwidth recalled{};
  Bandwidth remaining{};
  OutcomeReason reason{OutcomeReason::None};
  std::uint64_t requested_tick{0};
  std::uint64_t deadline_tick{0};
  bool acknowledgement_required{false};
  AuthorityVector authority{};
};

struct BB_API Grant final {
  BandwidthGrantId id{};
  BandwidthGrantGeneration generation{};
  BandwidthRequestId request{};
  BandwidthRequestGeneration request_generation{};
  CapacityTarget target{};
  GrantState state{GrantState::Requested};
  GrantAllocation allocation{};

  Bandwidth requested_minimum{};
  Bandwidth requested_desired{};
  Bandwidth requested_maximum{};
  Bandwidth denied{};
  bool satisfied{false};
  OutcomeReason reason{OutcomeReason::None};

  DecisionId decision{};
  RecallId recall{};
  std::optional<BandwidthGrantId> superseded_by{};
  std::optional<BandwidthGrantId> supersedes{};

  std::uint64_t issued_tick{0};
  std::uint64_t last_revalidated_tick{0};
  std::optional<std::uint64_t> expires_at_tick{};

  // Scheduling state used by starvation prevention. It is durable state, not a
  // hint: identical state must produce identical grant outcomes.
  std::uint64_t wait_rounds{0};

  // Funding provenance of the granted amount. Recording it on the grant makes an
  // out-of-round release exactly reversible: capacity funded by an obligation
  // returns to that obligation, and capacity funded by the emergency reserve
  // returns to the reserve, instead of being invented as arbitrable capacity.
  Bandwidth obligation_backed{};
  Bandwidth reserve_backed{};

  AuthorityVector authority{};

  [[nodiscard]] Status transition_to(GrantState next) noexcept;
  [[nodiscard]] bool authorises_consumption() const noexcept { return bandwidth_broker::authorises_consumption(state); }
  [[nodiscard]] bool is_live() const noexcept { return bandwidth_broker::is_live(state); }
  [[nodiscard]] Result<Bandwidth> total() const { return allocation.total(); }

  // Bumps the grant generation. Used whenever the authority vector or the
  // granted amount changes, so that an old generation can never be reused.
  [[nodiscard]] Status advance_generation() noexcept;
};

[[nodiscard]] BB_API Status validate_grant(const Grant& grant);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_GRANT_HPP
