// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/accounting.hpp"

#include "bandwidth_broker/grant.hpp"
#include "bandwidth_broker/numeric.hpp"

namespace bandwidth_broker {
namespace {

void check_equality(std::vector<AccountingInvariantViolation>& out,
                    const char* name,
                    Bandwidth left,
                    Bandwidth right) {
  if (left == right) {
    return;
  }
  AccountingInvariantViolation violation;
  violation.invariant = name;
  violation.detail = left.to_string() + " != " + right.to_string();
  out.push_back(std::move(violation));
}

void check_not_greater(std::vector<AccountingInvariantViolation>& out,
                       const char* name,
                       Bandwidth left,
                       Bandwidth right) {
  if (left <= right) {
    return;
  }
  AccountingInvariantViolation violation;
  violation.invariant = name;
  violation.detail = left.to_string() + " > " + right.to_string();
  out.push_back(std::move(violation));
}

// Checked sum of the accounting terms; returns false on overflow, which is
// itself an invariant violation.
[[nodiscard]] Bandwidth sat_sub(Bandwidth a, Bandwidth b) noexcept { return Bandwidth::saturating_sub(a, b); }

[[nodiscard]] bool sum_checked(const std::vector<Bandwidth>& terms, Bandwidth& out) {
  const auto total = Bandwidth::sum(terms);
  if (!total.ok()) {
    return false;
  }
  out = total.value();
  return true;
}

}  // namespace

std::vector<AccountingInvariantViolation> ResourceAccounting::validate() const {
  std::vector<AccountingInvariantViolation> violations;
  Bandwidth total = Bandwidth::zero();

  if (evidence == CapacityEvidenceState::Known) {
    Bandwidth physical_side = Bandwidth::zero();
    Bandwidth allocation_side = Bandwidth::zero();
    if (!sum_checked({effective_physical, capacity_deficit}, physical_side)) {
      violations.push_back({"no_overflow", "effective physical + deficit overflowed"});
      return violations;
    }
    if (!sum_checked({obligations_reserved, allocatable}, allocation_side)) {
      violations.push_back({"no_overflow", "reserved + allocatable overflowed"});
      return violations;
    }
    check_equality(violations, "physical_closes", physical_side, allocation_side);

    if (!sum_checked({emergency_reserve, headroom, arbitrable}, total)) {
      violations.push_back({"no_overflow", "reserve + headroom + arbitrable overflowed"});
      return violations;
    }
    check_equality(violations, "allocatable_closes", allocatable, total);

    if (!sum_checked({emergency_reserve_unused, headroom, guaranteed_granted, discretionary_granted, unallocated},
                     total)) {
      violations.push_back({"no_overflow", "grant closure overflowed"});
      return violations;
    }
    check_equality(violations, "grant_closure", allocatable, total);
    check_not_greater(violations, "emergency_reserve_unused_within_reserve", emergency_reserve_unused,
                      emergency_reserve);
  } else {
    check_equality(violations, "unknown_has_no_emergency_reserve", emergency_reserve, Bandwidth::zero());
    check_equality(violations, "unknown_has_no_effective_capacity", effective_physical, Bandwidth::zero());
    check_equality(violations, "unknown_has_no_allocatable_capacity", allocatable, Bandwidth::zero());
    check_equality(violations, "unknown_has_no_arbitrable_capacity", arbitrable, Bandwidth::zero());
    check_equality(violations, "unknown_authorizes_nothing", authorized_consumption, Bandwidth::zero());
  }

  if (!sum_checked({obligations_consumed, obligations_lent, obligations_idle}, total)) {
    violations.push_back({"no_overflow", "obligation split overflowed"});
    return violations;
  }
  check_equality(violations, "obligations_split_closes", obligations_reserved, total);

  if (!sum_checked({contingent_granted, contingent_unallocated}, total)) {
    violations.push_back({"no_overflow", "contingent split overflowed"});
    return violations;
  }
  check_equality(violations, "contingent_split_closes", contingent_pool, total);

  check_equality(violations, "borrowed_equals_lent", borrowed_granted, obligations_lent);

  if (!sum_checked({obligations_consumed, guaranteed_granted, discretionary_granted, borrowed_granted, contingent_granted},
                   total)) {
    violations.push_back({"no_overflow", "authorized consumption overflowed"});
    return violations;
  }
  check_equality(violations, "authorized_consumption_closes", authorized_consumption, total);

  if (!sum_checked({effective_physical, contingent_pool}, total)) {
    violations.push_back({"no_overflow", "capacity ceiling overflowed"});
    return violations;
  }
  check_not_greater(violations, "no_silent_oversubscription", authorized_consumption, total);

  return violations;
}

bool ResourceAccounting::consistent() const { return validate().empty(); }

Status apply_grant_retirement(ResourceAccounting& accounting, const Grant& grant) {
  const Bandwidth granted_guaranteed = grant.allocation.guaranteed;
  const Bandwidth reserved_guaranteed = Bandwidth::min(grant.obligation_backed, granted_guaranteed);
  const Bandwidth remaining_guaranteed = sat_sub(granted_guaranteed, reserved_guaranteed);
  const Bandwidth reserve_in_guaranteed = Bandwidth::min(grant.reserve_backed, remaining_guaranteed);
  const Bandwidth arbitrable_guaranteed = sat_sub(remaining_guaranteed, reserve_in_guaranteed);
  const Bandwidth reserve_in_discretionary = sat_sub(grant.reserve_backed, reserve_in_guaranteed);
  const Bandwidth arbitrable_discretionary = sat_sub(grant.allocation.discretionary, reserve_in_discretionary);

  accounting.obligations_consumed = sat_sub(accounting.obligations_consumed, reserved_guaranteed);
  accounting.obligations_idle = accounting.obligations_idle.checked_add(reserved_guaranteed).value();
  accounting.obligations_lent = sat_sub(accounting.obligations_lent, grant.allocation.borrowed);
  accounting.obligations_idle = accounting.obligations_idle.checked_add(grant.allocation.borrowed).value();
  accounting.guaranteed_granted = sat_sub(accounting.guaranteed_granted, arbitrable_guaranteed);
  accounting.discretionary_granted = sat_sub(accounting.discretionary_granted, arbitrable_discretionary);
  accounting.unallocated = accounting.unallocated.checked_add(arbitrable_guaranteed).value();
  accounting.unallocated = accounting.unallocated.checked_add(arbitrable_discretionary).value();
  accounting.emergency_reserve_unused =
      accounting.emergency_reserve_unused.checked_add(grant.reserve_backed).value();
  accounting.borrowed_granted = sat_sub(accounting.borrowed_granted, grant.allocation.borrowed);
  accounting.contingent_granted = sat_sub(accounting.contingent_granted, grant.allocation.contingent);
  accounting.contingent_unallocated =
      accounting.contingent_unallocated.checked_add(grant.allocation.contingent).value();

  Bandwidth authorized = accounting.obligations_consumed;
  for (const Bandwidth term : {accounting.guaranteed_granted, accounting.discretionary_granted,
                               accounting.borrowed_granted, accounting.contingent_granted}) {
    const auto next = authorized.checked_add(term);
    if (!next.ok()) {
      return next.error();
    }
    authorized = next.value();
  }
  accounting.authorized_consumption = authorized;

  const auto violations = accounting.validate();
  if (!violations.empty()) {
    return make_error_status(ErrorCode::AccountingInvariantViolation,
                             "releasing a grant produced inconsistent accounting: " + describe_violations(violations));
  }
  return Status::success();
}

std::string describe_accounting(const ResourceAccounting& accounting) {
  std::string out;
  out.reserve(512);
  out += "target=";
  out += accounting.target.describe();
  out += ",evidence=";
  out += to_string(accounting.evidence);
  out += ",effective=";
  out += accounting.effective_physical.to_string();
  out += ",reserved=";
  out += accounting.obligations_reserved.to_string();
  out += ",allocatable=";
  out += accounting.allocatable.to_string();
  out += ",emergency=";
  out += accounting.emergency_reserve.to_string();
  out += ",headroom=";
  out += accounting.headroom.to_string();
  out += ",arbitrable=";
  out += accounting.arbitrable.to_string();
  out += ",guaranteed=";
  out += accounting.guaranteed_granted.to_string();
  out += ",discretionary=";
  out += accounting.discretionary_granted.to_string();
  out += ",borrowed=";
  out += accounting.borrowed_granted.to_string();
  out += ",contingent=";
  out += accounting.contingent_granted.to_string();
  out += ",unallocated=";
  out += accounting.unallocated.to_string();
  out += ",authorized=";
  out += accounting.authorized_consumption.to_string();
  return out;
}

std::string describe_violations(const std::vector<AccountingInvariantViolation>& violations) {
  std::string out;
  for (std::size_t i = 0; i < violations.size(); ++i) {
    if (i != 0) {
      out += "; ";
    }
    out += violations[i].invariant;
    out += ": ";
    out += violations[i].detail;
  }
  if (out.empty()) {
    out = "none";
  }
  return out;
}

}  // namespace bandwidth_broker
