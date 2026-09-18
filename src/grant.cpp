// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/grant.hpp"

namespace bandwidth_broker {

const char* to_string(GrantState state) noexcept {
  switch (state) {
    case GrantState::Requested: return "requested";
    case GrantState::Validated: return "validated";
    case GrantState::Queued: return "queued";
    case GrantState::GrantedGuaranteed: return "granted_guaranteed";
    case GrantState::GrantedBorrowed: return "granted_borrowed";
    case GrantState::RecallPending: return "recall_pending";
    case GrantState::RevalidationRequired: return "revalidation_required";
    case GrantState::Released: return "released";
    case GrantState::Expired: return "expired";
    case GrantState::Revoked: return "revoked";
    case GrantState::Fenced: return "fenced";
    case GrantState::Stale: return "stale";
    case GrantState::Rejected: return "rejected";
  }
  return "unknown";
}

const char* to_string(AllocationKind kind) noexcept {
  switch (kind) {
    case AllocationKind::None: return "none";
    case AllocationKind::Guaranteed: return "guaranteed";
    case AllocationKind::Discretionary: return "discretionary";
    case AllocationKind::Borrowed: return "borrowed";
    case AllocationKind::Contingent: return "contingent";
  }
  return "none";
}

bool is_terminal(GrantState state) noexcept {
  switch (state) {
    case GrantState::Released:
    case GrantState::Expired:
    case GrantState::Revoked:
    case GrantState::Fenced:
    case GrantState::Stale:
    case GrantState::Rejected:
      return true;
    default:
      return false;
  }
}

bool is_live(GrantState state) noexcept {
  return state == GrantState::GrantedGuaranteed || state == GrantState::GrantedBorrowed ||
         state == GrantState::RecallPending || state == GrantState::RevalidationRequired;
}

bool authorises_consumption(GrantState state) noexcept {
  // A recall-pending grant authorises only the amount that has not yet been
  // recalled; that residual is tracked by the recall record, so the grant state
  // itself authorises consumption only while it is fully granted.
  return state == GrantState::GrantedGuaranteed || state == GrantState::GrantedBorrowed;
}

bool is_legal_grant_transition(GrantState from, GrantState to) noexcept {
  if (from == to) {
    // Idempotent re-validation of an unchanged grant is legal.
    return true;
  }
  switch (from) {
    case GrantState::Requested:
      return to == GrantState::Validated || to == GrantState::Rejected || to == GrantState::Stale;
    case GrantState::Validated:
      return to == GrantState::Queued || to == GrantState::Rejected || to == GrantState::Stale;
    case GrantState::Queued:
      return to == GrantState::GrantedGuaranteed || to == GrantState::GrantedBorrowed ||
             to == GrantState::Rejected || to == GrantState::Stale || to == GrantState::RevalidationRequired;
    case GrantState::GrantedGuaranteed:
    case GrantState::GrantedBorrowed:
      return to == GrantState::RecallPending || to == GrantState::Released || to == GrantState::Expired ||
             to == GrantState::Revoked || to == GrantState::Fenced || to == GrantState::Stale ||
             to == GrantState::RevalidationRequired;
    case GrantState::RecallPending:
      return to == GrantState::Released || to == GrantState::Expired || to == GrantState::Revoked ||
             to == GrantState::Fenced || to == GrantState::Stale || to == GrantState::RevalidationRequired;
    case GrantState::RevalidationRequired:
      return to == GrantState::GrantedGuaranteed || to == GrantState::GrantedBorrowed ||
             to == GrantState::Released || to == GrantState::Expired || to == GrantState::Revoked ||
             to == GrantState::Fenced || to == GrantState::Stale;
    case GrantState::Released:
    case GrantState::Expired:
    case GrantState::Revoked:
    case GrantState::Fenced:
    case GrantState::Stale:
    case GrantState::Rejected:
      return false;
  }
  return false;
}

const char* to_string(OutcomeReason reason) noexcept {
  switch (reason) {
    case OutcomeReason::None: return "none";
    case OutcomeReason::SatisfiedFully: return "satisfied_fully";
    case OutcomeReason::SatisfiedAtMaximum: return "satisfied_at_maximum";
    case OutcomeReason::PartiallySatisfied: return "partially_satisfied";
    case OutcomeReason::MinimumGuaranteedOnly: return "minimum_guaranteed_only";
    case OutcomeReason::BelowMinimumWaiting: return "below_minimum_waiting";
    case OutcomeReason::ZeroCapacityAvailable: return "zero_capacity_available";
    case OutcomeReason::CapacityUnknown: return "capacity_unknown";
    case OutcomeReason::CapacityWithdrawn: return "capacity_withdrawn";
    case OutcomeReason::HeadroomPreserved: return "headroom_preserved";
    case OutcomeReason::EmergencyReservePreserved: return "emergency_reserve_preserved";
    case OutcomeReason::GroupCapReached: return "group_cap_reached";
    case OutcomeReason::FairnessLimited: return "fairness_limited";
    case OutcomeReason::BorrowPoolExhausted: return "borrow_pool_exhausted";
    case OutcomeReason::OversubscriptionDisabled: return "oversubscription_disabled";
    case OutcomeReason::WaitingForStrongerClass: return "waiting_for_stronger_class";
    case OutcomeReason::DeferredByHysteresis: return "deferred_by_hysteresis";
    case OutcomeReason::RecalledByStrongerClass: return "recalled_by_stronger_class";
    case OutcomeReason::RecalledByObligationReturn: return "recalled_by_obligation_return";
    case OutcomeReason::RecalledByCapacityReduction: return "recalled_by_capacity_reduction";
    case OutcomeReason::RecalledByPolicyChange: return "recalled_by_policy_change";
    case OutcomeReason::RefusedContradictory: return "refused_contradictory";
    case OutcomeReason::RefusedDuplicateIdentity: return "refused_duplicate_identity";
    case OutcomeReason::RefusedIdentityConflict: return "refused_identity_conflict";
    case OutcomeReason::RefusedUnknownPriorityClass: return "refused_unknown_priority_class";
    case OutcomeReason::RefusedUnknownFairnessGroup: return "refused_unknown_fairness_group";
    case OutcomeReason::RefusedTenantMismatch: return "refused_tenant_mismatch";
    case OutcomeReason::RefusedStaleEpoch: return "refused_stale_epoch";
    case OutcomeReason::RefusedStaleResourceGeneration: return "refused_stale_resource_generation";
    case OutcomeReason::RefusedStalePolicyGeneration: return "refused_stale_policy_generation";
    case OutcomeReason::RefusedStaleRequester: return "refused_stale_requester";
    case OutcomeReason::RefusedStaleReservation: return "refused_stale_reservation";
    case OutcomeReason::RefusedStaleTenantConfig: return "refused_stale_tenant_config";
    case OutcomeReason::RefusedBootFenced: return "refused_boot_fenced";
    case OutcomeReason::RefusedBelowQuantum: return "refused_below_quantum";
    case OutcomeReason::RefusedNoCapacityAuthority: return "refused_no_capacity_authority";
    case OutcomeReason::Expired: return "expired";
    case OutcomeReason::Released: return "released";
    case OutcomeReason::Revoked: return "revoked";
    case OutcomeReason::Fenced: return "fenced";
    case OutcomeReason::Stale: return "stale";
    case OutcomeReason::RevalidationRequired: return "revalidation_required";
  }
  return "none";
}

bool is_outcome_waiting(OutcomeReason reason) noexcept {
  switch (reason) {
    case OutcomeReason::BelowMinimumWaiting:
    case OutcomeReason::ZeroCapacityAvailable:
    case OutcomeReason::CapacityUnknown:
    case OutcomeReason::CapacityWithdrawn:
    case OutcomeReason::HeadroomPreserved:
    case OutcomeReason::EmergencyReservePreserved:
    case OutcomeReason::GroupCapReached:
    case OutcomeReason::FairnessLimited:
    case OutcomeReason::BorrowPoolExhausted:
    case OutcomeReason::OversubscriptionDisabled:
    case OutcomeReason::WaitingForStrongerClass:
    case OutcomeReason::PartiallySatisfied:
    case OutcomeReason::MinimumGuaranteedOnly:
    case OutcomeReason::DeferredByHysteresis:
      return true;
    default:
      return false;
  }
}

bool is_outcome_refused(OutcomeReason reason) noexcept {
  switch (reason) {
    case OutcomeReason::RefusedContradictory:
    case OutcomeReason::RefusedDuplicateIdentity:
    case OutcomeReason::RefusedIdentityConflict:
    case OutcomeReason::RefusedUnknownPriorityClass:
    case OutcomeReason::RefusedUnknownFairnessGroup:
    case OutcomeReason::RefusedTenantMismatch:
    case OutcomeReason::RefusedStaleEpoch:
    case OutcomeReason::RefusedStaleResourceGeneration:
    case OutcomeReason::RefusedStalePolicyGeneration:
    case OutcomeReason::RefusedStaleRequester:
    case OutcomeReason::RefusedStaleReservation:
    case OutcomeReason::RefusedStaleTenantConfig:
    case OutcomeReason::RefusedBootFenced:
    case OutcomeReason::RefusedBelowQuantum:
    case OutcomeReason::RefusedNoCapacityAuthority:
    case OutcomeReason::Expired:
    case OutcomeReason::Released:
    case OutcomeReason::Revoked:
    case OutcomeReason::Fenced:
    case OutcomeReason::Stale:
      return true;
    default:
      return false;
  }
}

bool is_outcome_granted(OutcomeReason reason) noexcept {
  switch (reason) {
    case OutcomeReason::SatisfiedFully:
    case OutcomeReason::SatisfiedAtMaximum:
      return true;
    default:
      return false;
  }
}

Result<Bandwidth> GrantAllocation::total() const {
  Bandwidth total = Bandwidth::zero();
  for (const Bandwidth part : {guaranteed, discretionary, borrowed, contingent}) {
    const auto next = total.checked_add(part);
    if (!next.ok()) {
      return next.error();
    }
    total = next.value();
  }
  return total;
}

Result<Bandwidth> GrantAllocation::revocable() const {
  Bandwidth total = Bandwidth::zero();
  for (const Bandwidth part : {discretionary, borrowed, contingent}) {
    const auto next = total.checked_add(part);
    if (!next.ok()) {
      return next.error();
    }
    total = next.value();
  }
  return total;
}

bool GrantAllocation::has_revocable() const noexcept {
  return discretionary.is_positive() || borrowed.is_positive() || contingent.is_positive();
}

Status Grant::transition_to(GrantState next) noexcept {
  if (!is_legal_grant_transition(state, next)) {
    return make_error_status(ErrorCode::InvalidStateTransition,
                             std::string("illegal grant transition ") + to_string(state) + " -> " + to_string(next));
  }
  state = next;
  return Status::success();
}

Status Grant::advance_generation() noexcept {
  const auto next = generation.next();
  if (!next.ok()) {
    return next.error();
  }
  generation = next.value();
  return Status::success();
}

Status validate_grant(const Grant& grant) {
  if (!grant.id.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "grant id is not set");
  }
  if (!grant.generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "grant generation is not set");
  }
  if (!grant.request.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "grant request id is not set");
  }
  if (!grant.request_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "grant request generation is not set");
  }
  BB_RETURN_IF_ERROR(grant.target.validate());

  const auto total = grant.allocation.total();
  if (!total.ok()) {
    return total.error();
  }
  if (total.value() > grant.requested_maximum) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "grant allocation exceeds the request maximum");
  }
  if (grant.satisfied && total.value() < grant.requested_minimum) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "a satisfied grant holds less than the request minimum");
  }
  if (grant.allocation.guaranteed > grant.requested_minimum) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "a grant holds more guaranteed capacity than the request minimum");
  }
  if (grant.state == GrantState::GrantedGuaranteed && grant.allocation.has_revocable()) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "a granted-guaranteed grant must not hold revocable capacity");
  }
  if (grant.obligation_backed > grant.allocation.guaranteed) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "a grant claims more obligation-backed capacity than it holds guaranteed");
  }
  {
    const auto funded = grant.allocation.guaranteed.checked_add(grant.allocation.discretionary);
    if (!funded.ok()) {
      return funded.error();
    }
    if (grant.reserve_backed > funded.value()) {
      return make_error_status(ErrorCode::AccountingInvariantViolation,
                               "a grant claims more reserve-backed capacity than it holds non-borrowed");
    }
  }
  if (grant.state == GrantState::GrantedBorrowed && !grant.allocation.has_revocable()) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "a granted-borrowed grant must hold revocable capacity");
  }
  if (is_terminal(grant.state) && grant.authorises_consumption()) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "a terminal grant must not authorise consumption");
  }
  // Terminal grants deliberately retain their final allocation as history: a
  // recall is a state transition, not a deletion. Capacity accounting is driven
  // by the grant state, never by the presence of a stored amount.
  return Status::success();
}

}  // namespace bandwidth_broker
