// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Capacity accounting.
//
// The identity that must hold for every resource after every authoritative
// mutation, when capacity evidence is Known:
//
//   (i)   effective_physical + capacity_deficit
//             == obligations_reserved + allocatable
//   (ii)  allocatable
//             == emergency_reserve + headroom + arbitrable
//   (iii) allocatable
//             == emergency_reserve_unused + headroom
//                + guaranteed_granted + discretionary_granted + unallocated
//   (iv)  obligations_reserved
//             == obligations_consumed + obligations_lent + obligations_idle
//   (v)   contingent_pool == contingent_granted + contingent_unallocated
//   (vi)  borrowed_granted == obligations_lent
//   (vii) authorized_consumption
//             == obligations_consumed + guaranteed_granted + discretionary_granted
//                + borrowed_granted + contingent_granted
//   (viii) authorized_consumption <= effective_physical + contingent_pool
//
// emergency_reserve is the reserve withheld before arbitration (E_init) and
// emergency_reserve_unused is what remained unclaimed (E_unused). Guaranteed
// capacity granted to emergency classes from the reserve is part of
// guaranteed_granted, which is why (iii) subtracts only the unused remainder.
//
// All quantities are non-negative by construction (checked integer arithmetic)
// and no term can overflow.

#ifndef BANDWIDTH_BROKER_ACCOUNTING_HPP
#define BANDWIDTH_BROKER_ACCOUNTING_HPP

#include <string>
#include <vector>

#include "bandwidth_broker/capacity.hpp"
#include "bandwidth_broker/grant.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

struct BB_API AccountingInvariantViolation final {
  std::string invariant;
  std::string detail;
};

struct BB_API ResourceAccounting final {
  CapacityTarget target{};
  CapacitySnapshotGeneration capacity_generation{};
  CapacityEvidenceState evidence{CapacityEvidenceState::Unknown};

  Bandwidth physical_configured{};
  Bandwidth administratively_unavailable{};
  Bandwidth degraded_loss{};
  Bandwidth effective_physical{};

  Bandwidth obligations_reserved{};
  Bandwidth obligations_consumed{};
  Bandwidth obligations_lent{};
  Bandwidth obligations_idle{};

  Bandwidth capacity_deficit{};
  Bandwidth allocatable{};
  Bandwidth emergency_reserve{};
  Bandwidth emergency_reserve_unused{};
  Bandwidth headroom{};
  Bandwidth arbitrable{};

  Bandwidth guaranteed_granted{};
  Bandwidth discretionary_granted{};
  Bandwidth borrowed_granted{};
  Bandwidth contingent_granted{};
  Bandwidth unallocated{};

  Bandwidth contingent_pool{};
  Bandwidth contingent_unallocated{};

  Bandwidth authorized_consumption{};

  [[nodiscard]] std::vector<AccountingInvariantViolation> validate() const;
  [[nodiscard]] bool consistent() const;
};

// Returns a terminated grant's capacity to the pools it was funded from.
//
// This is the exact inverse of the grant's contribution to the accounting
// identity, which is why the funding provenance is recorded on the grant. It is
// used by out-of-round release, revocation and fencing, so that accounting
// remains exact between arbitration rounds rather than drifting until the next
// round recomputes it.
[[nodiscard]] BB_API Status apply_grant_retirement(ResourceAccounting& accounting, const Grant& grant);

[[nodiscard]] BB_API std::string describe_accounting(const ResourceAccounting& accounting);
[[nodiscard]] BB_API std::string describe_violations(const std::vector<AccountingInvariantViolation>& violations);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_ACCOUNTING_HPP
