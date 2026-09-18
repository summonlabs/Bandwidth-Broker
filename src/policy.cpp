// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/policy.hpp"

#include <set>

#include "bandwidth_broker/checksum.hpp"
#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {
namespace {

[[nodiscard]] Status validate_weight(std::uint64_t weight, const char* field) {
  if (weight == 0) {
    return make_error_status(ErrorCode::PolicyInvalid, std::string(field) + " must be at least 1");
  }
  if (weight > limits::kMaxWeight) {
    return make_error_status(ErrorCode::PolicyInvalid, std::string(field) + " exceeds the supported maximum weight");
  }
  return Status::success();
}

}  // namespace

const char* to_string(SchedulingMode mode) noexcept {
  switch (mode) {
    case SchedulingMode::StrictPriority: return "strict_priority";
    case SchedulingMode::WeightedFairShare: return "weighted_fair_share";
  }
  return "strict_priority";
}

Status Policy::validate() const {
  if (!id.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "policy id is not set");
  }
  if (!generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "policy generation is not set");
  }
  if (!fairness_config_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "policy fairness config generation is not set");
  }
  if (!tenant_config_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "policy tenant config generation is not set");
  }
  if (priority_classes.empty()) {
    return make_error_status(ErrorCode::PolicyInvalid, "policy must define at least one priority class");
  }
  if (priority_classes.size() > limits::kMaxPriorityClasses) {
    return make_error_status(ErrorCode::PolicyInvalid, "policy defines too many priority classes");
  }
  if (fairness_groups.size() > limits::kMaxFairnessGroups) {
    return make_error_status(ErrorCode::PolicyInvalid, "policy defines too many fairness groups");
  }

  std::set<std::uint64_t> class_ids;
  std::set<std::uint32_t> ranks;
  for (const PriorityClass& priority_class : priority_classes) {
    if (!priority_class.id.valid()) {
      return make_error_status(ErrorCode::InvalidIdentity, "priority class id is not set");
    }
    if (!class_ids.insert(priority_class.id.value()).second) {
      return make_error_status(ErrorCode::PolicyInvalid, "duplicate priority class id in policy");
    }
    if (!ranks.insert(priority_class.rank).second) {
      return make_error_status(ErrorCode::PolicyInvalid, "duplicate priority class rank in policy");
    }
    if (priority_class.rank > 10000) {
      return make_error_status(ErrorCode::PolicyInvalid, "priority class rank is out of range");
    }
    BB_RETURN_IF_ERROR(validate_weight(priority_class.weight, "priority class weight"));
    BB_RETURN_IF_ERROR(validate_text(priority_class.name, limits::kMaxNameBytes, "priority class name"));
    if (priority_class.name.empty()) {
      return make_error_status(ErrorCode::PolicyInvalid, "priority class name must not be empty");
    }
  }

  std::set<std::uint64_t> group_ids;
  for (const FairnessGroupConfig& group : fairness_groups) {
    if (!group.id.valid()) {
      return make_error_status(ErrorCode::InvalidIdentity, "fairness group id is not set");
    }
    if (!group_ids.insert(group.id.value()).second) {
      return make_error_status(ErrorCode::PolicyInvalid, "duplicate fairness group id in policy");
    }
    if (!group.tenant.valid()) {
      return make_error_status(ErrorCode::InvalidIdentity, "fairness group tenant id is not set");
    }
    BB_RETURN_IF_ERROR(validate_weight(group.weight, "fairness group weight"));
    if (group.maximum_cap_permille > limits::kMaxPermille) {
      return make_error_status(ErrorCode::PolicyInvalid, "fairness group cap permille exceeds 1000");
    }
    BB_RETURN_IF_ERROR(validate_text(group.name, limits::kMaxNameBytes, "fairness group name"));
    if (group.name.empty()) {
      return make_error_status(ErrorCode::PolicyInvalid, "fairness group name must not be empty");
    }
  }

  if (headroom_permille > limits::kMaxPermille) {
    return make_error_status(ErrorCode::PolicyInvalid, "headroom permille exceeds 1000");
  }
  if (emergency_reserve_permille > limits::kMaxPermille) {
    return make_error_status(ErrorCode::PolicyInvalid, "emergency reserve permille exceeds 1000");
  }
  if (static_cast<std::uint32_t>(headroom_permille) + emergency_reserve_permille > limits::kMaxPermille) {
    return make_error_status(ErrorCode::PolicyInvalid,
                             "headroom and emergency reserve together exceed all allocatable capacity");
  }
  if (borrow.max_lend_permille > limits::kMaxPermille) {
    return make_error_status(ErrorCode::PolicyInvalid, "borrow lend permille exceeds 1000");
  }
  if (borrow.max_borrow_permille > limits::kMaxPermille) {
    return make_error_status(ErrorCode::PolicyInvalid, "borrow ceiling permille exceeds 1000");
  }
  if (!borrow.enabled) {
    if (borrow.lend_obligations || borrow.max_lend_permille != 0 || borrow.max_borrow_permille != 0) {
      return make_error_status(ErrorCode::PolicyInvalid, "borrowing is disabled but borrow parameters are set");
    }
  } else {
    if (borrow.max_borrow_permille == 0) {
      return make_error_status(ErrorCode::PolicyInvalid, "borrowing is enabled but the borrow ceiling is zero");
    }
    if (borrow.lend_obligations && borrow.max_lend_permille == 0) {
      return make_error_status(ErrorCode::PolicyInvalid,
                               "lending obligations is enabled but the lend ceiling is zero");
    }
    if (!borrow.lend_obligations && borrow.max_lend_permille != 0) {
      return make_error_status(ErrorCode::PolicyInvalid,
                               "lending obligations is disabled but the lend ceiling is non-zero");
    }
    for (const FairnessGroupConfig& group : fairness_groups) {
      if (group.borrowing_eligible && !group.preemptible) {
        return make_error_status(ErrorCode::PolicyInvalid,
                                 "a borrowing-eligible fairness group must be preemptible");
      }
    }
    for (const PriorityClass& priority_class : priority_classes) {
      if (priority_class.borrowing_eligible && !priority_class.preemptible) {
        return make_error_status(ErrorCode::PolicyInvalid,
                                 "a borrowing-eligible priority class must be preemptible");
      }
    }
  }

  if (starvation.aging_threshold_rounds == 0) {
    return make_error_status(ErrorCode::PolicyInvalid, "starvation aging threshold must be at least one round");
  }
  if (starvation.aging_threshold_rounds > limits::kMaxAgingThresholdRounds) {
    return make_error_status(ErrorCode::PolicyInvalid, "starvation aging threshold is out of range");
  }
  if (starvation.aging_weight_multiplier == 0 || starvation.aging_weight_multiplier > limits::kMaxAgingMultiplier) {
    return make_error_status(ErrorCode::PolicyInvalid, "starvation aging weight multiplier is out of range");
  }
  if (starvation.maximum_promotions > limits::kMaxPromotions) {
    return make_error_status(ErrorCode::PolicyInvalid, "starvation maximum promotions is out of range");
  }

  if (preemption.recall_grace_rounds > limits::kMaxRecallGraceRounds) {
    return make_error_status(ErrorCode::PolicyInvalid, "preemption recall grace rounds is out of range");
  }
  if (!preemption.enabled && preemption.recall_discretionary) {
    return make_error_status(ErrorCode::PolicyInvalid,
                             "preemption is disabled but discretionary recall is enabled");
  }

  if (oversubscription.denominator == 0) {
    return make_error_status(ErrorCode::PolicyInvalid, "oversubscription denominator must not be zero");
  }
  if (oversubscription.numerator < oversubscription.denominator) {
    return make_error_status(ErrorCode::PolicyInvalid,
                             "oversubscription numerator must not be below its denominator");
  }
  if (oversubscription.numerator >
      static_cast<std::uint64_t>(oversubscription.denominator) * limits::kMaxOversubscriptionNumerator) {
    return make_error_status(ErrorCode::PolicyInvalid, "oversubscription ratio exceeds the supported maximum");
  }

  if (hysteresis_rounds > limits::kMaxHysteresisRounds) {
    return make_error_status(ErrorCode::PolicyInvalid, "policy hysteresis rounds is out of range");
  }

  if (minimum_allocation_quantum > Bandwidth::zero()) {
    const auto quantum_limit = Bandwidth::from_bits_per_second(Bandwidth::kMaxBitsPerSecond / 2);
    if (!quantum_limit.ok()) {
      return quantum_limit.error();
    }
    if (minimum_allocation_quantum > quantum_limit.value()) {
      return make_error_status(ErrorCode::PolicyInvalid, "minimum allocation quantum is out of range");
    }
  }

  BB_RETURN_IF_ERROR(validate_text(provenance.detail, limits::kMaxProvenanceDetailBytes, "provenance detail"));
  return Status::success();
}

const PriorityClass* Policy::find_class(PriorityClassId class_id) const noexcept {
  for (const PriorityClass& priority_class : priority_classes) {
    if (priority_class.id == class_id) {
      return &priority_class;
    }
  }
  return nullptr;
}

const FairnessGroupConfig* Policy::find_group(FairnessGroupId group_id) const noexcept {
  for (const FairnessGroupConfig& group : fairness_groups) {
    if (group.id == group_id) {
      return &group;
    }
  }
  return nullptr;
}

Result<Bandwidth> Policy::quantum() const {
  if (minimum_allocation_quantum.is_positive()) {
    return minimum_allocation_quantum;
  }
  return Bandwidth::from_bits_per_second(1);
}

std::uint32_t Policy::strongest_rank() const noexcept {
  std::uint32_t best = 0;
  for (const PriorityClass& priority_class : priority_classes) {
    if (priority_class.rank > best) {
      best = priority_class.rank;
    }
  }
  return best;
}

std::uint32_t Policy::weakest_rank() const noexcept {
  std::uint32_t worst = 0;
  bool first = true;
  for (const PriorityClass& priority_class : priority_classes) {
    if (first || priority_class.rank < worst) {
      worst = priority_class.rank;
      first = false;
    }
  }
  return worst;
}

Result<std::uint32_t> Policy::effective_rank(PriorityClassId class_id, std::uint64_t wait_rounds) const {
  const PriorityClass* priority_class = find_class(class_id);
  if (priority_class == nullptr) {
    return make_error<std::uint32_t>(ErrorCode::PolicyInvalid, "request priority class is not defined by the policy");
  }
  std::uint32_t rank = priority_class->rank;
  if (starvation.enabled && wait_rounds >= starvation.aging_threshold_rounds && starvation.maximum_promotions > 0) {
    rank += starvation.maximum_promotions;
    if (rank > strongest_rank()) {
      rank = strongest_rank();
    }
  }
  return rank;
}

Result<std::uint64_t> Policy::effective_class_weight(PriorityClassId class_id, std::uint64_t wait_rounds) const {
  const PriorityClass* priority_class = find_class(class_id);
  if (priority_class == nullptr) {
    return make_error<std::uint64_t>(ErrorCode::PolicyInvalid, "request priority class is not defined by the policy");
  }
  std::uint64_t weight = priority_class->weight;
  if (starvation.enabled && wait_rounds >= starvation.aging_threshold_rounds && starvation.maximum_promotions > 0) {
    if (weight > limits::kMaxWeight / starvation.aging_weight_multiplier) {
      weight = limits::kMaxWeight;
    } else {
      weight = weight * starvation.aging_weight_multiplier;
    }
  }
  return weight;
}

std::uint64_t Policy::content_hash() const {
  ContentHasher hasher;
  hasher.add_u64(id.value());
  hasher.add_u64(generation.value());
  hasher.add_u8(static_cast<std::uint8_t>(scheduling));
  hasher.add_u64(priority_classes.size());
  for (const PriorityClass& priority_class : priority_classes) {
    hasher.add_u64(priority_class.id.value());
    hasher.add_u64(priority_class.rank);
    hasher.add_u64(priority_class.weight);
    hasher.add_bool(priority_class.emergency);
    hasher.add_bool(priority_class.preemptible);
    hasher.add_bool(priority_class.borrowing_eligible);
    hasher.add_bool(priority_class.allow_discretionary);
    hasher.add_text(priority_class.name);
  }
  hasher.add_u64(fairness_groups.size());
  for (const FairnessGroupConfig& group : fairness_groups) {
    hasher.add_u64(group.id.value());
    hasher.add_u64(group.tenant.value());
    hasher.add_u64(group.weight);
    hasher.add_u64(static_cast<std::uint64_t>(group.maximum_cap.bits_per_second()));
    hasher.add_u32(group.maximum_cap_permille);
    hasher.add_bool(group.borrowing_eligible);
    hasher.add_bool(group.preemptible);
    hasher.add_text(group.name);
  }
  hasher.add_u64(static_cast<std::uint64_t>(headroom_target.bits_per_second()));
  hasher.add_u32(headroom_permille);
  hasher.add_u32(emergency_reserve_permille);
  hasher.add_bool(borrow.enabled);
  hasher.add_bool(borrow.lend_obligations);
  hasher.add_u32(borrow.max_lend_permille);
  hasher.add_u32(borrow.max_borrow_permille);
  hasher.add_bool(borrow.borrow_from_headroom);
  hasher.add_bool(starvation.enabled);
  hasher.add_u32(starvation.aging_threshold_rounds);
  hasher.add_u32(starvation.aging_weight_multiplier);
  hasher.add_u32(starvation.maximum_promotions);
  hasher.add_bool(preemption.enabled);
  hasher.add_bool(preemption.recall_discretionary);
  hasher.add_u32(preemption.recall_grace_rounds);
  hasher.add_u32(oversubscription.numerator);
  hasher.add_u32(oversubscription.denominator);
  hasher.add_u64(static_cast<std::uint64_t>(minimum_allocation_quantum.bits_per_second()));
  hasher.add_u32(hysteresis_rounds);
  hasher.add_u64(fairness_config_generation.value());
  hasher.add_u64(tenant_config_generation.value());
  return hasher.value();
}

bool Policy::operator==(const Policy& other) const noexcept {
  return id == other.id && generation == other.generation && scheduling == other.scheduling &&
         priority_classes == other.priority_classes && fairness_groups == other.fairness_groups &&
         headroom_target == other.headroom_target && headroom_permille == other.headroom_permille &&
         emergency_reserve_permille == other.emergency_reserve_permille && borrow == other.borrow &&
         starvation == other.starvation && preemption == other.preemption &&
         oversubscription == other.oversubscription &&
         minimum_allocation_quantum == other.minimum_allocation_quantum && hysteresis_rounds == other.hysteresis_rounds &&
         fairness_config_generation == other.fairness_config_generation &&
         tenant_config_generation == other.tenant_config_generation;
}

Status validate_policy(const Policy& policy) { return policy.validate(); }

}  // namespace bandwidth_broker
