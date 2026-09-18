// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Capacity model.
//
// Capacity is supplied by an external authority. Bandwidth Broker never infers,
// measures or discovers physical capacity: a resource whose evidence is absent,
// stale or withdrawn has UNKNOWN capacity, and UNKNOWN capacity is never treated
// as spare capacity.

#ifndef BANDWIDTH_BROKER_CAPACITY_HPP
#define BANDWIDTH_BROKER_CAPACITY_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

// Evidence quality of a capacity statement. Only Known evidence may be used to
// authorise a grant.
enum class CapacityEvidenceState : std::uint8_t {
  Unknown = 0,  // no usable evidence; never treated as spare capacity
  Known = 1,    // current, generation-bound evidence from an authorised publisher
  Stale = 2     // superseded evidence; retained for audit, never authoritative
};

[[nodiscard]] BB_API const char* to_string(CapacityEvidenceState state) noexcept;

// The exact capacity object a request, obligation or grant refers to: either a
// whole resource or one pool within a resource.
struct BB_API CapacityTarget final {
  BandwidthResourceId resource{};
  BandwidthResourceGeneration resource_generation{};
  BandwidthPoolId pool{};
  BandwidthPoolGeneration pool_generation{};

  [[nodiscard]] bool has_pool() const noexcept { return pool.valid(); }
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::string describe() const;

  [[nodiscard]] friend bool operator==(const CapacityTarget& a, const CapacityTarget& b) noexcept {
    return a.resource == b.resource && a.resource_generation == b.resource_generation && a.pool == b.pool &&
           a.pool_generation == b.pool_generation;
  }
  [[nodiscard]] friend bool operator!=(const CapacityTarget& a, const CapacityTarget& b) noexcept { return !(a == b); }
  [[nodiscard]] friend bool operator<(const CapacityTarget& a, const CapacityTarget& b) noexcept {
    if (a.resource != b.resource) return a.resource < b.resource;
    if (a.pool != b.pool) return a.pool < b.pool;
    if (a.resource_generation != b.resource_generation) return a.resource_generation < b.resource_generation;
    return a.pool_generation < b.pool_generation;
  }
};

// One generation-bound capacity statement produced by an authorised publisher.
//
//   effective_physical = physical_configured
//                      - administratively_unavailable
//                      - degraded_loss
//
// All three terms are explicit. A publisher that knows only part of the picture
// must declare evidence Unknown rather than supply a partial number.
struct BB_API CapacitySnapshot final {
  CapacityTarget target{};
  CapacitySnapshotId snapshot{};
  CapacitySnapshotGeneration generation{};
  CapacityEvidenceState evidence{CapacityEvidenceState::Unknown};
  std::optional<Bandwidth> physical_configured{};
  Bandwidth administratively_unavailable{};
  Bandwidth degraded_loss{};
  Bandwidth headroom_target{};       // operator-declared headroom floor for this target
  Bandwidth reserved_committed{};    // declared committed total; must equal the sum of obligations
  Provenance provenance{};
  AuthorityVector authority{};
  Timestamp observed_at{};
  std::uint64_t publisher_sequence{0};

  [[nodiscard]] bool is_usable() const noexcept { return evidence == CapacityEvidenceState::Known && physical_configured.has_value(); }
  [[nodiscard]] Status validate() const;

  // Precondition: is_usable(); otherwise fails with ErrorCode::CapacityUnknown.
  [[nodiscard]] Result<Bandwidth> effective_physical() const;
};

[[nodiscard]] BB_API Status validate_capacity_snapshot(const CapacitySnapshot& snapshot);

// Maximum of two headroom statements; used to combine the operator target with
// the policy permille so that neither silently overrides the other.
[[nodiscard]] BB_API Result<Bandwidth> combine_headroom(Bandwidth target, Bandwidth permille_share) noexcept;

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_CAPACITY_HPP
