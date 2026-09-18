// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Bandwidth request model.
//
// A request is a *claim*, never an authorisation. Submitting a request makes no
// capacity available: only an arbitration decision that binds the exact
// generations in the request's authority vector produces a grant.

#ifndef BANDWIDTH_BROKER_REQUEST_HPP
#define BANDWIDTH_BROKER_REQUEST_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bandwidth_broker/capacity.hpp"
#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

enum class GuaranteeClass : std::uint8_t {
  Unspecified = 0,  // derived deterministically from the minimum
  Guaranteed = 1,   // the minimum is a guarantee that must be met or explicitly refused
  BestEffort = 2    // no guarantee; the minimum must be zero
};

enum class RecallTolerance : std::uint8_t {
  Unspecified = 0,  // derived deterministically from preemptibility
  Immediate = 1,    // the holder accepts immediate recall
  GracePeriod = 2,  // the holder accepts recall after the policy grace period
  NoRecall = 3      // the holder never accepts recall; requires preemptible == false
};

[[nodiscard]] BB_API const char* to_string(GuaranteeClass value) noexcept;
[[nodiscard]] BB_API const char* to_string(RecallTolerance value) noexcept;

// A policy label. Labels are metadata for policy selection and audit; they never
// carry authority. The vector must be sorted by key with unique keys.
struct BB_API PolicyLabel final {
  std::string key;
  std::string value;

  [[nodiscard]] friend bool operator==(const PolicyLabel& a, const PolicyLabel& b) noexcept {
    return a.key == b.key && a.value == b.value;
  }
  [[nodiscard]] friend bool operator<(const PolicyLabel& a, const PolicyLabel& b) noexcept { return a.key < b.key; }
};

// Binding to a committed reservation owned by the Bandwidth Reservation Fabric.
struct BB_API ReservationBinding final {
  ReservationReferenceId reservation{};
  ReservationGeneration generation{};

  [[nodiscard]] bool valid() const noexcept { return reservation.valid() && generation.valid(); }
  [[nodiscard]] friend bool operator==(const ReservationBinding& a, const ReservationBinding& b) noexcept {
    return a.reservation == b.reservation && a.generation == b.generation;
  }
};

// Effective window expressed in coordinator ticks. Wall-clock time never grants
// or expires authority.
struct BB_API RequestWindow final {
  std::uint64_t start_tick{0};
  std::optional<std::uint64_t> end_tick{};

  [[nodiscard]] bool contains(std::uint64_t tick) const noexcept {
    return tick >= start_tick && (!end_tick.has_value() || tick < *end_tick);
  }
  [[nodiscard]] friend bool operator==(const RequestWindow& a, const RequestWindow& b) noexcept {
    return a.start_tick == b.start_tick && a.end_tick == b.end_tick;
  }
};

struct BB_API BandwidthRequest final {
  BandwidthRequestId id{};
  BandwidthRequestGeneration generation{};

  // Requester-claimed authority. The coordinator-filled members of this vector
  // (coordinator, capacity_generation, monotonic_tick) must be unset here.
  AuthorityVector authority{};

  CapacityTarget target{};

  Bandwidth minimum{};
  Bandwidth desired{};
  Bandwidth maximum{};

  GuaranteeClass guarantee_class{GuaranteeClass::Unspecified};
  PriorityClassId priority{};
  TenantId tenant{};
  FairnessGroupId fairness_group{};

  bool borrowing_eligible{false};
  bool preemptible{true};
  RecallTolerance recall_tolerance{RecallTolerance::Unspecified};

  std::optional<ReservationBinding> reservation{};
  std::optional<RequestWindow> window{};
  SloClassId latency_slo{};

  std::vector<PolicyLabel> labels{};
  Provenance provenance{};

  [[nodiscard]] GuaranteeClass effective_guarantee_class() const noexcept;
  [[nodiscard]] RecallTolerance effective_recall_tolerance() const noexcept;

  // A request is revocable when it is preemptible and tolerates recall. Only
  // revocable requests may borrow: borrowed bandwidth is always revocable.
  [[nodiscard]] bool is_revocable() const noexcept { return effective_recall_tolerance() != RecallTolerance::NoRecall; }

  // The largest amount this request may hold.
  [[nodiscard]] Bandwidth ceiling() const noexcept { return Bandwidth::min(desired, maximum); }

  [[nodiscard]] Status validate() const;

  // Canonical content hash of the requester-supplied semantics. Two submissions
  // with the same (id, generation) and the same content hash are duplicates;
  // the same identity with a different content hash is a conflict.
  [[nodiscard]] std::uint64_t content_hash() const;

  [[nodiscard]] bool operator==(const BandwidthRequest& other) const noexcept;
};

[[nodiscard]] BB_API Status validate_request(const BandwidthRequest& request);

// Validates the requester-claimed subset of an authority vector and rejects a
// submission that pre-fills coordinator-owned bindings.
[[nodiscard]] BB_API Status validate_requester_authority(const AuthorityVector& authority) noexcept;

[[nodiscard]] BB_API bool requester_authority_well_formed(const AuthorityVector& authority) noexcept;

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_REQUEST_HPP
