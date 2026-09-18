// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Arbitration policy.
//
// Arbitration is policy, not an incidental code path: priority ordering,
// fairness weights, guarantees, caps, borrowing, recall precedence, starvation
// prevention, headroom preservation and emergency reservation are all explicit,
// validated, generation-bound policy fields.
//
// Precedence applied by the arbitrator, in order:
//   1. capacity evidence must be Known and generation-current, otherwise no
//      capacity is arbitrable at all;
//   2. hard obligations (reserved committed capacity) are withheld;
//   3. the emergency reserve is withheld for emergency priority classes;
//   4. headroom is withheld and is never lent;
//   5. guaranteed minima are satisfied in descending effective priority rank;
//   6. residual capacity is distributed by the scheduling mode;
//   7. unused, lendable obligation capacity may be borrowed by revocable
//      requests;
//   8. only explicitly enabled oversubscription may grant contingent capacity.

#ifndef BANDWIDTH_BROKER_POLICY_HPP
#define BANDWIDTH_BROKER_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

enum class SchedulingMode : std::uint8_t {
  // Priority classes are served in descending rank order. Within a class,
  // capacity is shared by weighted max-min fairness across fairness groups.
  StrictPriority = 0,
  // One weighted max-min fair share across all groups, where a group's weight
  // is its own weight multiplied by its priority class weight. Class rank is
  // used only for guaranteed minima ordering and deterministic tie-breaking.
  WeightedFairShare = 1
};

[[nodiscard]] BB_API const char* to_string(SchedulingMode mode) noexcept;

struct BB_API PriorityClass final {
  PriorityClassId id{};
  std::string name;
  std::uint32_t rank{0};               // strictly unique; larger rank is stronger
  std::uint64_t weight{1};             // fairness weight inside the class
  bool emergency{false};               // may draw on the emergency reserve
  bool preemptible{true};              // class default for requests that do not say
  bool borrowing_eligible{false};      // class default for requests that do not say
  bool allow_discretionary{true};      // may hold capacity above its minimum

  [[nodiscard]] friend bool operator==(const PriorityClass& a, const PriorityClass& b) noexcept {
    return a.id == b.id && a.rank == b.rank && a.weight == b.weight && a.emergency == b.emergency &&
           a.preemptible == b.preemptible && a.borrowing_eligible == b.borrowing_eligible &&
           a.allow_discretionary == b.allow_discretionary && a.name == b.name;
  }
};

struct BB_API FairnessGroupConfig final {
  FairnessGroupId id{};
  TenantId tenant{};
  std::string name;
  std::uint64_t weight{1};
  Bandwidth maximum_cap{};              // absolute cap; zero means uncapped
  std::uint32_t maximum_cap_permille{0};// share of effective physical capacity; zero means uncapped
  bool borrowing_eligible{false};
  bool preemptible{true};

  [[nodiscard]] friend bool operator==(const FairnessGroupConfig& a, const FairnessGroupConfig& b) noexcept {
    return a.id == b.id && a.tenant == b.tenant && a.weight == b.weight && a.maximum_cap == b.maximum_cap &&
           a.maximum_cap_permille == b.maximum_cap_permille && a.borrowing_eligible == b.borrowing_eligible &&
           a.preemptible == b.preemptible && a.name == b.name;
  }
};

struct BB_API BorrowPolicy final {
  bool enabled{false};
  bool lend_obligations{false};         // unused obligation capacity may be lent
  std::uint32_t max_lend_permille{0};   // ceiling on the lendable share of an obligation
  std::uint32_t max_borrow_permille{0}; // ceiling on borrowed capacity as a share of effective physical capacity
  bool borrow_from_headroom{false};     // headroom is never lent unless this is explicitly enabled

  [[nodiscard]] friend bool operator==(const BorrowPolicy& a, const BorrowPolicy& b) noexcept {
    return a.enabled == b.enabled && a.lend_obligations == b.lend_obligations &&
           a.max_lend_permille == b.max_lend_permille && a.max_borrow_permille == b.max_borrow_permille &&
           a.borrow_from_headroom == b.borrow_from_headroom;
  }
};

struct BB_API StarvationPolicy final {
  bool enabled{true};
  std::uint32_t aging_threshold_rounds{8};
  std::uint32_t aging_weight_multiplier{2};
  std::uint32_t maximum_promotions{1};

  [[nodiscard]] friend bool operator==(const StarvationPolicy& a, const StarvationPolicy& b) noexcept {
    return a.enabled == b.enabled && a.aging_threshold_rounds == b.aging_threshold_rounds &&
           a.aging_weight_multiplier == b.aging_weight_multiplier && a.maximum_promotions == b.maximum_promotions;
  }
};

struct BB_API PreemptionPolicy final {
  bool enabled{true};
  bool recall_discretionary{true};      // capacity above a minimum is revocable
  std::uint32_t recall_grace_rounds{0};

  [[nodiscard]] friend bool operator==(const PreemptionPolicy& a, const PreemptionPolicy& b) noexcept {
    return a.enabled == b.enabled && a.recall_discretionary == b.recall_discretionary &&
           a.recall_grace_rounds == b.recall_grace_rounds;
  }
};

// Explicit, integer-ratio model of controlled oversubscription. The default
// 1/1 disables oversubscription entirely. When numerator > denominator the
// additional capacity is "contingent": it is granted only to revocable
// requests, it is never counted as guaranteed, and it remains distinct from
// guaranteed capacity in every accounting view.
struct BB_API OversubscriptionPolicy final {
  std::uint32_t numerator{1};
  std::uint32_t denominator{1};

  [[nodiscard]] bool enabled() const noexcept { return numerator > denominator; }
  [[nodiscard]] friend bool operator==(const OversubscriptionPolicy& a, const OversubscriptionPolicy& b) noexcept {
    return a.numerator == b.numerator && a.denominator == b.denominator;
  }
};

struct BB_API Policy final {
  PolicyId id{};
  PolicyGeneration generation{};
  SchedulingMode scheduling{SchedulingMode::StrictPriority};

  std::vector<PriorityClass> priority_classes{};
  std::vector<FairnessGroupConfig> fairness_groups{};

  Bandwidth headroom_target{};                  // absolute headroom floor
  std::uint32_t headroom_permille{0};           // additional headroom as a share of allocatable capacity
  std::uint32_t emergency_reserve_permille{0};  // share withheld for emergency classes

  BorrowPolicy borrow{};
  StarvationPolicy starvation{};
  PreemptionPolicy preemption{};
  OversubscriptionPolicy oversubscription{};

  Bandwidth minimum_allocation_quantum{};       // indivisible allocation quantum; zero means one bit per second
  std::uint32_t hysteresis_rounds{0};           // rounds a superseded grant stays recall-pending

  FairnessConfigGeneration fairness_config_generation{};
  TenantConfigGeneration tenant_config_generation{};

  Provenance provenance{};

  [[nodiscard]] Status validate() const;

  [[nodiscard]] const PriorityClass* find_class(PriorityClassId id) const noexcept;
  [[nodiscard]] const FairnessGroupConfig* find_group(FairnessGroupId id) const noexcept;
  [[nodiscard]] Result<Bandwidth> quantum() const;

  // Strongest and weakest configured ranks.
  [[nodiscard]] std::uint32_t strongest_rank() const noexcept;
  [[nodiscard]] std::uint32_t weakest_rank() const noexcept;

  // Effective rank of a class after applying starvation promotion for a request
  // that has been unsatisfied for p wait_rounds consecutive rounds.
  [[nodiscard]] Result<std::uint32_t> effective_rank(PriorityClassId id, std::uint64_t wait_rounds) const;

  // Effective fairness weight of a class after applying starvation promotion.
  [[nodiscard]] Result<std::uint64_t> effective_class_weight(PriorityClassId id, std::uint64_t wait_rounds) const;

  [[nodiscard]] std::uint64_t content_hash() const;

  [[nodiscard]] bool operator==(const Policy& other) const noexcept;
};

[[nodiscard]] BB_API Status validate_policy(const Policy& policy);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_POLICY_HPP
