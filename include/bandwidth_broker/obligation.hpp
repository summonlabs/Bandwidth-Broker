// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Hard obligations.
//
// An obligation is a committed capacity reservation imported from the Bandwidth
// Reservation Fabric. Reservations are durable/future commitments and are owned
// by that runtime; Bandwidth Broker only represents the resulting capacity
// obligation so that arbitration does not over-allocate over it.
//
// Obligations are never grants: they contribute to accounting as reserved
// capacity and are consumed only by grants bound to the same reservation
// reference and generation.

#ifndef BANDWIDTH_BROKER_OBLIGATION_HPP
#define BANDWIDTH_BROKER_OBLIGATION_HPP

#include <optional>
#include <string>

#include "bandwidth_broker/capacity.hpp"
#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

struct BB_API Obligation final {
  ReservationReferenceId reservation{};
  ReservationGeneration generation{};
  CapacityTarget target{};
  Bandwidth amount{};

  // Policy view of whether this obligation may temporarily lend unused capacity
  // to borrowers, and what share of it may be lent.
  bool lendable{false};
  std::uint32_t max_lend_permille{0};

  Provenance provenance{};

  [[nodiscard]] Status validate() const;

  // Largest amount of this obligation that policy permits to be lent.
  [[nodiscard]] Result<Bandwidth> lendable_ceiling() const;

  [[nodiscard]] friend bool operator==(const Obligation& a, const Obligation& b) noexcept {
    return a.reservation == b.reservation && a.generation == b.generation && a.target == b.target &&
           a.amount == b.amount && a.lendable == b.lendable && a.max_lend_permille == b.max_lend_permille;
  }
};

[[nodiscard]] BB_API Status validate_obligation(const Obligation& obligation);

// Canonical ordering used for deterministic reporting and accounting.
[[nodiscard]] BB_API bool obligation_less(const Obligation& a, const Obligation& b) noexcept;

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_OBLIGATION_HPP
