// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/arbitrator.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "bandwidth_broker/fairness.hpp"
#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {
namespace {

constexpr const char* kReasonReservationInsufficient = "reservation capacity is below the requested minimum";
constexpr const char* kReasonGuaranteeShortfall = "guaranteed capacity exhausted before this request";
constexpr const char* kReasonGroupCap = "fairness group maximum cap reached";
constexpr const char* kReasonZeroCapacity = "no arbitrable capacity is available";
constexpr const char* kReasonHeadroom = "only preserved headroom remains";
constexpr const char* kReasonEmergencyReserve = "only the emergency reserve remains";
constexpr const char* kReasonBorrowDisabled = "borrowing is disabled by policy";
constexpr const char* kReasonBorrowCeiling = "borrow ceiling reached";
constexpr const char* kReasonOversubscription = "controlled oversubscription is disabled";
constexpr const char* kReasonFairness = "weighted max-min fair share limited the allocation";
constexpr const char* kReasonStrongerClass = "a stronger priority class holds the remaining capacity";
constexpr const char* kReasonQuantum = "request maximum is below the minimum allocation quantum";

[[nodiscard]] Bandwidth sat_sub(Bandwidth a, Bandwidth b) noexcept { return Bandwidth::saturating_sub(a, b); }

struct WorkItem final {
  std::size_t candidate_index{0};
  const BandwidthRequest* request{nullptr};
  const PriorityClass* priority_class{nullptr};
  const FairnessGroupConfig* group{nullptr};
  std::uint32_t effective_rank{0};
  std::uint64_t effective_weight{1};
  bool starvation_promoted{false};
  bool admitted{false};
  bool refused{false};
  bool capacity_unavailable{false};
  OutcomeReason reason{OutcomeReason::None};
  GrantAllocation allocation{};
  Bandwidth obligation_guaranteed{};
  Bandwidth reserve_allocated{};
  Bandwidth residual{};
  Bandwidth unmet_minimum{};
  Bandwidth denied{};
  std::string binding_reason;
  std::string authority_mismatch;
};

struct RunState final {
  Bandwidth quantum{};
  Bandwidth unlimited{};
  std::vector<WorkItem> items;
  std::map<FairnessGroupId, Bandwidth> group_cap{};
  std::map<FairnessGroupId, Bandwidth> group_allocated{};
  std::map<FairnessGroupId, std::uint64_t> group_weight{};

  [[nodiscard]] Bandwidth remaining_budget(FairnessGroupId group) const {
    const auto cap = group_cap.find(group);
    if (cap == group_cap.end()) {
      return unlimited;
    }
    const auto used = group_allocated.find(group);
    const Bandwidth consumed = (used == group_allocated.end()) ? Bandwidth::zero() : used->second;
    return sat_sub(cap->second, consumed);
  }

  [[nodiscard]] bool group_cap_reached(FairnessGroupId group) const {
    return group_cap.find(group) != group_cap.end() && remaining_budget(group).is_zero();
  }

  void add_group_allocated(FairnessGroupId group, Bandwidth amount) {
    auto& slot = group_allocated[group];
    const auto next = slot.checked_add(amount);
    slot = next.ok() ? next.value() : unlimited;
  }
};

void add_allocation(GrantAllocation& allocation, AllocationKind kind, Bandwidth amount) {
  switch (kind) {
    case AllocationKind::Guaranteed: allocation.guaranteed = allocation.guaranteed.checked_add(amount).value(); break;
    case AllocationKind::Discretionary: allocation.discretionary = allocation.discretionary.checked_add(amount).value(); break;
    case AllocationKind::Borrowed: allocation.borrowed = allocation.borrowed.checked_add(amount).value(); break;
    case AllocationKind::Contingent: allocation.contingent = allocation.contingent.checked_add(amount).value(); break;
    case AllocationKind::None: break;
  }
}

// Distributes capacity among the selected items, sharing between fairness
// groups by group weight and inside a group by the item's effective class
// weight, with hierarchical weighted max-min fairness.
[[nodiscard]] Result<Bandwidth> distribute(RunState& run,
                                           const std::vector<std::size_t>& selection,
                                           Bandwidth capacity,
                                           AllocationKind kind,
                                           bool from_reserve = false) {
  if (capacity.is_zero() || selection.empty()) {
    return Bandwidth::zero();
  }
  std::map<FairnessGroupId, std::vector<FairnessLeaf>> buckets;
  for (const std::size_t index : selection) {
    WorkItem& item = run.items[index];
    const Bandwidth budget = run.remaining_budget(item.group->id);
    const Bandwidth demand = Bandwidth::min(item.residual, budget);
    if (demand.is_zero()) {
      continue;
    }
    FairnessLeaf leaf;
    leaf.id = index;
    leaf.weight = item.effective_weight;
    leaf.demand = demand;
    buckets[item.group->id].push_back(leaf);
  }
  if (buckets.empty()) {
    return Bandwidth::zero();
  }

  std::vector<FairnessBranch> branches;
  branches.reserve(buckets.size());
  for (auto& entry : buckets) {
    FairnessBranch branch;
    branch.weight = run.group_weight[entry.first];
    branch.leaves = std::move(entry.second);
    branches.push_back(std::move(branch));
  }

  const auto fair = weighted_max_min_fair_share(capacity, run.quantum, branches);
  if (!fair.ok()) {
    return fair.error();
  }
  for (const FairnessShare& share : fair.value().shares) {
    WorkItem& item = run.items[share.id];
    add_allocation(item.allocation, kind, share.allocated);
    item.residual = sat_sub(item.residual, share.allocated);
    run.add_group_allocated(item.group->id, share.allocated);
    if (from_reserve) {
      const auto next = item.reserve_allocated.checked_add(share.allocated);
      if (!next.ok()) {
        return next.error();
      }
      item.reserve_allocated = next.value();
    }
  }
  return fair.value().allocated;
}

[[nodiscard]] Bandwidth total_allocation(const GrantAllocation& allocation) {
  const auto total = allocation.total();
  return total.ok() ? total.value() : Bandwidth::zero();
}

[[nodiscard]] std::string bounded(std::string text) {
  if (text.size() > limits::kMaxExplanationTextBytes) {
    text.resize(limits::kMaxExplanationTextBytes);
  }
  return text;
}

}  // namespace

Status validate_arbitration_input(const ArbitrationInput& input) {
  BB_RETURN_IF_ERROR(input.target.validate());
  BB_RETURN_IF_ERROR(input.snapshot.validate());
  BB_RETURN_IF_ERROR(input.policy.validate());

  if (input.snapshot.target != input.target) {
    return make_error_status(ErrorCode::InvalidArgument, "capacity snapshot target does not match the arbitration target");
  }
  if (!input.epoch.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "arbitration fabric epoch is not set");
  }
  if (!input.coordinator.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "arbitration coordinator incarnation is not set");
  }
  if (input.tick == 0) {
    return make_error_status(ErrorCode::InvalidArgument, "arbitration tick must be positive");
  }
  if (!input.decision.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "arbitration decision id is not set");
  }
  if (input.snapshot.authority.fabric_epoch != input.epoch) {
    return make_error_status(ErrorCode::EpochMismatch, "capacity snapshot was published under a different fabric epoch");
  }
  if (input.snapshot.authority.coordinator != input.coordinator) {
    return make_error_status(ErrorCode::NotAuthoritative,
                             "capacity snapshot was published under a different coordinator incarnation");
  }
  if (input.candidates.size() > limits::kMaxRequestsPerRound) {
    return make_error_status(ErrorCode::BoundsExceeded, "too many candidates in one arbitration round");
  }
  if (input.obligations.size() > limits::kMaxObligationsPerResource) {
    return make_error_status(ErrorCode::BoundsExceeded, "too many obligations for one target");
  }
  if (input.fenced_boots.size() > limits::kMaxBootFenceRecords) {
    return make_error_status(ErrorCode::BoundsExceeded, "too many boot fence records");
  }

  std::set<std::uint64_t> obligations_seen;
  for (const Obligation& obligation : input.obligations) {
    BB_RETURN_IF_ERROR(obligation.validate());
    if (obligation.target != input.target) {
      return make_error_status(ErrorCode::InvalidArgument, "obligation target does not match the arbitration target");
    }
    if (!obligations_seen.insert(obligation.reservation.value()).second) {
      return make_error_status(ErrorCode::IdentityConflict,
                               "two obligations claim the same reservation reference in one round");
    }
  }
  return Status::success();
}

Result<ArbitrationOutcome> arbitrate(const ArbitrationInput& input) {
  BB_RETURN_IF_ERROR(validate_arbitration_input(input));

  ArbitrationOutcome outcome;
  outcome.decision = input.decision;
  outcome.target = input.target;
  outcome.epoch = input.epoch;
  outcome.coordinator = input.coordinator;
  outcome.policy = input.policy.id;
  outcome.policy_generation = input.policy.generation;
  outcome.tick = input.tick;
  outcome.next_grant_id = input.next_grant_id;
  outcome.next_recall_id = input.next_recall_id;

  ResourceAccounting& accounting = outcome.accounting;
  accounting.target = input.target;
  accounting.capacity_generation = input.snapshot.generation;
  accounting.evidence = input.snapshot.evidence;

  const auto unlimited_result = Bandwidth::max();
  if (!unlimited_result.ok()) {
    return unlimited_result.error();
  }
  const Bandwidth unlimited = unlimited_result.value();

  // ---------------------------------------------------------------------
  // Phase A: authority validation.
  // ---------------------------------------------------------------------
  RunState run;
  run.unlimited = unlimited;
  {
    const auto quantum = input.policy.quantum();
    if (!quantum.ok()) {
      return quantum.error();
    }
    run.quantum = quantum.value();
  }

  std::set<std::pair<std::uint64_t, std::uint64_t>> identities_seen;
  std::unordered_map<std::uint64_t, const Obligation*> obligations_by_reservation;
  for (const Obligation& obligation : input.obligations) {
    obligations_by_reservation.emplace(obligation.reservation.value(), &obligation);
  }
  std::set<std::uint64_t> fenced;
  for (const BootId& boot : input.fenced_boots) {
    fenced.insert(boot.hash());
  }

  run.items.reserve(input.candidates.size());
  for (std::size_t i = 0; i < input.candidates.size(); ++i) {
    const ArbitrationCandidate& candidate = input.candidates[i];
    const BandwidthRequest& request = candidate.request;

    WorkItem item;
    item.candidate_index = i;
    item.request = &request;
    item.effective_rank = 0;
    item.effective_weight = 1;
    item.reason = OutcomeReason::None;

    const auto reject = [&item](OutcomeReason reason, const char* detail) {
      item.admitted = false;
      item.refused = true;
      item.reason = reason;
      item.binding_reason = bounded(detail);
    };

    const Status structural = request.validate();
    if (!structural.ok()) {
      item.authority_mismatch = bounded(structural.message());
      reject(OutcomeReason::RefusedContradictory, structural.message().c_str());
      run.items.push_back(std::move(item));
      continue;
    }
    if (!identities_seen.insert({request.id.value(), request.generation.value()}).second) {
      reject(OutcomeReason::RefusedDuplicateIdentity,
             "two candidates claim the same request identity and generation in one round");
      run.items.push_back(std::move(item));
      continue;
    }
    if (fenced.count(request.authority.publisher_boot.hash()) != 0) {
      reject(OutcomeReason::RefusedBootFenced, "the requester boot identity is permanently fenced");
      run.items.push_back(std::move(item));
      continue;
    }
    if (request.authority.fabric_epoch != input.epoch) {
      item.authority_mismatch = bounded(request.authority.describe_mismatch(input.snapshot.authority));
      reject(OutcomeReason::RefusedStaleEpoch, "request fabric epoch is not the current fabric epoch");
      run.items.push_back(std::move(item));
      continue;
    }
    if (request.target != input.target) {
      item.authority_mismatch = bounded(request.target.describe());
      reject(OutcomeReason::RefusedStaleResourceGeneration,
             "request target or resource generation is not the current target");
      run.items.push_back(std::move(item));
      continue;
    }
    if (request.authority.policy != input.policy.id ||
        request.authority.policy_generation != input.policy.generation) {
      reject(OutcomeReason::RefusedStalePolicyGeneration, "request policy generation is not the current policy");
      run.items.push_back(std::move(item));
      continue;
    }
    if (request.authority.fairness_config_generation != input.policy.fairness_config_generation ||
        request.authority.tenant_config_generation != input.policy.tenant_config_generation) {
      reject(OutcomeReason::RefusedStaleTenantConfig,
             "request fairness or tenant configuration generation is not current");
      run.items.push_back(std::move(item));
      continue;
    }
    const PriorityClass* priority_class = input.policy.find_class(request.priority);
    if (priority_class == nullptr) {
      reject(OutcomeReason::RefusedUnknownPriorityClass, "request priority class is not defined by the policy");
      run.items.push_back(std::move(item));
      continue;
    }
    const FairnessGroupConfig* group = input.policy.find_group(request.fairness_group);
    if (group == nullptr) {
      reject(OutcomeReason::RefusedUnknownFairnessGroup, "request fairness group is not defined by the policy");
      run.items.push_back(std::move(item));
      continue;
    }
    if (group->tenant != request.tenant) {
      reject(OutcomeReason::RefusedTenantMismatch, "request tenant does not own the request fairness group");
      run.items.push_back(std::move(item));
      continue;
    }
    if (request.reservation.has_value()) {
      const auto obligation = obligations_by_reservation.find(request.reservation->reservation.value());
      if (obligation == obligations_by_reservation.end() ||
          obligation->second->generation != request.reservation->generation) {
        reject(OutcomeReason::RefusedStaleReservation,
               "request reservation binding is not a current obligation generation");
        run.items.push_back(std::move(item));
        continue;
      }
    }

    const auto rank = input.policy.effective_rank(request.priority, candidate.wait_rounds);
    if (!rank.ok()) {
      reject(OutcomeReason::RefusedUnknownPriorityClass, "request priority class has no effective rank");
      run.items.push_back(std::move(item));
      continue;
    }
    const auto weight = input.policy.effective_class_weight(request.priority, candidate.wait_rounds);
    if (!weight.ok()) {
      reject(OutcomeReason::RefusedUnknownPriorityClass, "request priority class has no effective weight");
      run.items.push_back(std::move(item));
      continue;
    }
    item.priority_class = priority_class;
    item.group = group;
    item.effective_rank = rank.value();
    item.effective_weight = weight.value();
    item.starvation_promoted = input.policy.starvation.enabled &&
                               candidate.wait_rounds >= input.policy.starvation.aging_threshold_rounds &&
                               input.policy.starvation.maximum_promotions > 0 && rank.value() > priority_class->rank;
    item.admitted = true;
    if (request.ceiling().is_zero() || (request.ceiling().bits_per_second() < run.quantum.bits_per_second())) {
      item.reason = OutcomeReason::RefusedBelowQuantum;
      item.binding_reason = kReasonQuantum;
      item.admitted = false;
      item.refused = true;
    }
    run.items.push_back(std::move(item));
  }

  // ---------------------------------------------------------------------
  // Phase B: capacity evidence.
  // ---------------------------------------------------------------------
  const bool usable = input.snapshot.is_usable();
  Bandwidth effective_physical = Bandwidth::zero();
  if (usable) {
    const auto physical = input.snapshot.effective_physical();
    if (!physical.ok()) {
      return physical.error();
    }
    effective_physical = physical.value();
  } else {
    accounting.physical_configured = input.snapshot.physical_configured.value_or(Bandwidth::zero());
    accounting.administratively_unavailable = input.snapshot.administratively_unavailable;
    accounting.degraded_loss = input.snapshot.degraded_loss;
  }

  if (usable) {
    accounting.physical_configured = input.snapshot.physical_configured.value_or(Bandwidth::zero());
    accounting.administratively_unavailable = input.snapshot.administratively_unavailable;
    accounting.degraded_loss = input.snapshot.degraded_loss;
    accounting.effective_physical = effective_physical;
  }

  // ---------------------------------------------------------------------
  // Phase C: pools.
  // ---------------------------------------------------------------------
  Bandwidth obligations_reserved = Bandwidth::zero();
  for (const Obligation& obligation : input.obligations) {
    const auto next = obligations_reserved.checked_add(obligation.amount);
    if (!next.ok()) {
      return next.error();
    }
    obligations_reserved = next.value();
  }
  accounting.obligations_reserved = obligations_reserved;

  Bandwidth allocatable = Bandwidth::zero();
  Bandwidth emergency_reserve_initial = Bandwidth::zero();
  Bandwidth headroom = Bandwidth::zero();
  Bandwidth arbitrable = Bandwidth::zero();
  Bandwidth contingent_pool = Bandwidth::zero();
  Bandwidth borrow_ceiling = Bandwidth::zero();

  if (usable) {
    accounting.capacity_deficit = sat_sub(obligations_reserved, effective_physical);
    allocatable = sat_sub(effective_physical, obligations_reserved);
    accounting.allocatable = allocatable;

    const auto emergency = allocatable.scaled(input.policy.emergency_reserve_permille, limits::kMaxPermille);
    if (!emergency.ok()) {
      return emergency.error();
    }
    emergency_reserve_initial = emergency.value();
    accounting.emergency_reserve = emergency_reserve_initial;
    accounting.emergency_reserve_unused = emergency_reserve_initial;

    const auto policy_headroom = allocatable.scaled(input.policy.headroom_permille, limits::kMaxPermille);
    if (!policy_headroom.ok()) {
      return policy_headroom.error();
    }
    headroom = Bandwidth::max_of(input.snapshot.headroom_target, policy_headroom.value());
    headroom = Bandwidth::min(headroom, sat_sub(allocatable, emergency_reserve_initial));
    accounting.headroom = headroom;
    arbitrable = sat_sub(sat_sub(allocatable, emergency_reserve_initial), headroom);
    accounting.arbitrable = arbitrable;

    if (input.policy.oversubscription.enabled()) {
      const std::uint64_t extra_numerator =
          static_cast<std::uint64_t>(input.policy.oversubscription.numerator) - input.policy.oversubscription.denominator;
      const auto extra = effective_physical.scaled(extra_numerator, input.policy.oversubscription.denominator);
      if (!extra.ok()) {
        return extra.error();
      }
      contingent_pool = extra.value();
    }
    accounting.contingent_pool = contingent_pool;
    accounting.contingent_unallocated = contingent_pool;

    if (input.policy.borrow.enabled) {
      const auto ceiling = effective_physical.scaled(input.policy.borrow.max_borrow_permille, limits::kMaxPermille);
      if (!ceiling.ok()) {
        return ceiling.error();
      }
      borrow_ceiling = ceiling.value();
    }
  }

  std::map<ReservationReferenceId, Bandwidth> obligation_consumed;

  const auto finish_unsatisfied = [&](OutcomeReason reason, const char* detail) {
    for (WorkItem& item : run.items) {
      if (!item.admitted) {
        continue;
      }
      item.reason = reason;
      item.binding_reason = bounded(detail);
      item.capacity_unavailable = true;
    }
  };

  if (!usable) {
    finish_unsatisfied(input.snapshot.evidence == CapacityEvidenceState::Stale ? OutcomeReason::CapacityWithdrawn
                                                                              : OutcomeReason::CapacityUnknown,
                       "capacity evidence for this target is not current; unknown capacity is never spare capacity");
    for (WorkItem& item : run.items) {
      item.admitted = false;
    }
  }

  // ---------------------------------------------------------------------
  // Phase D: deterministic ordering and group budgets.
  // ---------------------------------------------------------------------
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < run.items.size(); ++i) {
    if (run.items[i].admitted) {
      order.push_back(i);
    }
  }
  std::sort(order.begin(), order.end(), [&run](std::size_t a, std::size_t b) {
    const WorkItem& left = run.items[a];
    const WorkItem& right = run.items[b];
    if (left.effective_rank != right.effective_rank) return left.effective_rank > right.effective_rank;
    if (left.group->weight != right.group->weight) return left.group->weight > right.group->weight;
    if (left.group->id != right.group->id) return left.group->id < right.group->id;
    if (left.request->tenant != right.request->tenant) return left.request->tenant < right.request->tenant;
    if (left.request->id != right.request->id) return left.request->id < right.request->id;
    return left.request->generation < right.request->generation;
  });

  for (const WorkItem& item : run.items) {
    // Refused candidates never reach the allocation phases and carry no policy
    // bindings; they must not be dereferenced here.
    if (!item.admitted) {
      continue;
    }
    run.group_weight[item.group->id] = item.group->weight;
    Bandwidth cap = unlimited;
    if (item.group->maximum_cap.is_positive()) {
      cap = Bandwidth::min(cap, item.group->maximum_cap);
    }
    if (item.group->maximum_cap_permille > 0) {
      const auto share = effective_physical.scaled(item.group->maximum_cap_permille, limits::kMaxPermille);
      if (!share.ok()) {
        return share.error();
      }
      cap = Bandwidth::min(cap, share.value());
    }
    auto existing = run.group_cap.find(item.group->id);
    if (existing == run.group_cap.end()) {
      run.group_cap.emplace(item.group->id, cap);
    } else {
      existing->second = Bandwidth::min(existing->second, cap);
    }
    run.group_allocated[item.group->id] = Bandwidth::zero();
  }

  for (WorkItem& item : run.items) {
    if (item.admitted) {
      item.residual = item.request->ceiling();
    }
  }

  Bandwidth remaining_arbitrable = arbitrable;
  Bandwidth emergency_available = emergency_reserve_initial;

  // ---------------------------------------------------------------------
  // Phase E: guaranteed minima, in descending effective priority rank.
  // ---------------------------------------------------------------------
  if (usable) {
    for (const std::size_t index : order) {
      WorkItem& item = run.items[index];
      const Bandwidth minimum = item.request->minimum;
      if (minimum.is_zero()) {
        continue;
      }
      if (item.request->reservation.has_value()) {
        const Obligation* obligation = obligations_by_reservation.at(item.request->reservation->reservation.value());
        Bandwidth& consumed = obligation_consumed[obligation->reservation];
        const Bandwidth available = sat_sub(obligation->amount, consumed);
        const Bandwidth granted = Bandwidth::min(minimum, available);
        const auto next = consumed.checked_add(granted);
        if (!next.ok()) {
          return next.error();
        }
        consumed = next.value();
        add_allocation(item.allocation, AllocationKind::Guaranteed, granted);
        item.obligation_guaranteed = granted;
        item.residual = sat_sub(item.residual, granted);
        run.add_group_allocated(item.group->id, granted);
        if (granted < minimum) {
          item.unmet_minimum = sat_sub(minimum, granted);
          item.reason = OutcomeReason::BelowMinimumWaiting;
          item.binding_reason = kReasonReservationInsufficient;
        }
        continue;
      }
      Bandwidth wanted = minimum;
      if (item.priority_class->emergency && emergency_available.is_positive()) {
        const Bandwidth from_reserve = Bandwidth::min(wanted, emergency_available);
        emergency_available = sat_sub(emergency_available, from_reserve);
        const auto reserve_total = item.reserve_allocated.checked_add(from_reserve);
        if (!reserve_total.ok()) {
          return reserve_total.error();
        }
        item.reserve_allocated = reserve_total.value();
        add_allocation(item.allocation, AllocationKind::Guaranteed, from_reserve);
        item.residual = sat_sub(item.residual, from_reserve);
        run.add_group_allocated(item.group->id, from_reserve);
        wanted = sat_sub(wanted, from_reserve);
      }
      if (wanted.is_positive() && remaining_arbitrable.is_positive()) {
        const Bandwidth from_pool = Bandwidth::min(wanted, remaining_arbitrable);
        remaining_arbitrable = sat_sub(remaining_arbitrable, from_pool);
        add_allocation(item.allocation, AllocationKind::Guaranteed, from_pool);
        item.residual = sat_sub(item.residual, from_pool);
        run.add_group_allocated(item.group->id, from_pool);
        wanted = sat_sub(wanted, from_pool);
      }
      if (wanted.is_positive()) {
        item.unmet_minimum = wanted;
        item.reason = OutcomeReason::BelowMinimumWaiting;
        item.binding_reason = kReasonGuaranteeShortfall;
      }
    }
  }

  // ---------------------------------------------------------------------
  // Phase F1: discretionary capacity toward each request's desired amount.
  // Phase F2: burst toward each request's maximum when capacity remains.
  // ---------------------------------------------------------------------
  if (usable) {
    std::vector<std::uint32_t> ranks;
    for (const std::size_t index : order) {
      if (ranks.empty() || ranks.back() != run.items[index].effective_rank) {
        ranks.push_back(run.items[index].effective_rank);
      }
    }
    std::sort(ranks.begin(), ranks.end(), std::greater<std::uint32_t>());

    const auto pass = [&](bool burst) -> Status {
      std::vector<std::size_t> selection;
      for (const std::size_t index : order) {
        WorkItem& item = run.items[index];
        const Bandwidth ceiling = burst ? item.request->maximum : item.request->desired;
        const Bandwidth held = total_allocation(item.allocation);
        if (held >= ceiling) {
          continue;
        }
        if (!burst && !item.priority_class->allow_discretionary) {
          continue;
        }
        selection.push_back(index);
      }
      if (selection.empty()) {
        return Status::success();
      }
      // Emergency classes are served before every other class and may draw on
      // the withheld emergency reserve. This is the only way reserve capacity
      // can leave the reserve, and it is recorded as an explicit policy rule.
      if (emergency_available.is_positive() && !burst) {
        std::vector<std::size_t> emergency_selection;
        for (const std::size_t index : selection) {
          if (run.items[index].priority_class->emergency) {
            emergency_selection.push_back(index);
          }
        }
        if (!emergency_selection.empty()) {
          // The reserve is distributed on its own so that every bit of it can
          // be attributed back to the reserve when a grant is retired.
          const auto granted =
              distribute(run, emergency_selection, emergency_available, AllocationKind::Discretionary, true);
          if (!granted.ok()) {
            return granted.error();
          }
          emergency_available = sat_sub(emergency_available, granted.value());
        }
      }
      if (input.policy.scheduling == SchedulingMode::StrictPriority) {
        for (const std::uint32_t rank : ranks) {
          if (remaining_arbitrable.is_zero()) {
            break;
          }
          std::vector<std::size_t> rank_selection;
          for (const std::size_t index : selection) {
            if (run.items[index].effective_rank == rank) {
              rank_selection.push_back(index);
            }
          }
          if (rank_selection.empty()) {
            continue;
          }
          const auto granted = distribute(run, rank_selection, remaining_arbitrable, AllocationKind::Discretionary);
          if (!granted.ok()) {
            return granted.error();
          }
          remaining_arbitrable = sat_sub(remaining_arbitrable, granted.value());
        }
        return Status::success();
      }
      const auto granted = distribute(run, selection, remaining_arbitrable, AllocationKind::Discretionary);
      if (!granted.ok()) {
        return granted.error();
      }
      remaining_arbitrable = sat_sub(remaining_arbitrable, granted.value());
      return Status::success();
    };

    BB_RETURN_IF_ERROR(pass(false));
    BB_RETURN_IF_ERROR(pass(true));
  }

  // ---------------------------------------------------------------------
  // Phase G: borrowing unused, lendable obligation capacity.
  // ---------------------------------------------------------------------
  Bandwidth borrowed_granted = Bandwidth::zero();
  if (usable && input.policy.borrow.enabled && input.policy.borrow.lend_obligations) {
    Bandwidth lendable = Bandwidth::zero();
    for (const Obligation& obligation : input.obligations) {
      if (!obligation.lendable) {
        continue;
      }
      const auto ceiling = obligation.amount.scaled(std::min(obligation.max_lend_permille,
                                                            input.policy.borrow.max_lend_permille),
                                                    limits::kMaxPermille);
      if (!ceiling.ok()) {
        return ceiling.error();
      }
      const Bandwidth consumed = obligation_consumed[obligation.reservation];
      const auto next = lendable.checked_add(sat_sub(ceiling.value(), consumed));
      if (!next.ok()) {
        return next.error();
      }
      lendable = next.value();
    }
    const Bandwidth pool = Bandwidth::min(lendable, borrow_ceiling);
    std::vector<std::size_t> selection;
    for (const std::size_t index : order) {
      WorkItem& item = run.items[index];
      const bool class_allows = item.priority_class->borrowing_eligible;
      const bool group_allows = item.group->borrowing_eligible;
      if (!item.request->borrowing_eligible || !item.request->is_revocable()) {
        continue;
      }
      if (!class_allows && !group_allows) {
        continue;
      }
      if (total_allocation(item.allocation) >= item.request->maximum) {
        continue;
      }
      selection.push_back(index);
    }
    if (!selection.empty() && pool.is_positive()) {
      std::vector<std::size_t> borrowed_selection;
      for (const std::size_t index : selection) {
        WorkItem& item = run.items[index];
        item.residual = sat_sub(item.request->maximum, total_allocation(item.allocation));
        if (item.residual.is_positive()) {
          borrowed_selection.push_back(index);
        }
      }
      const auto granted = distribute(run, borrowed_selection, pool, AllocationKind::Borrowed);
      if (!granted.ok()) {
        return granted.error();
      }
      borrowed_granted = granted.value();
    }
  }

  // ---------------------------------------------------------------------
  // Phase H: contingent capacity under explicit oversubscription.
  // ---------------------------------------------------------------------
  Bandwidth contingent_granted = Bandwidth::zero();
  if (usable && contingent_pool.is_positive()) {
    std::vector<std::size_t> selection;
    for (const std::size_t index : order) {
      WorkItem& item = run.items[index];
      if (!item.request->is_revocable()) {
        continue;
      }
      if (total_allocation(item.allocation) >= item.request->maximum) {
        continue;
      }
      item.residual = sat_sub(item.request->maximum, total_allocation(item.allocation));
      if (item.residual.is_positive()) {
        selection.push_back(index);
      }
    }
    if (!selection.empty()) {
      const auto granted = distribute(run, selection, contingent_pool, AllocationKind::Contingent);
      if (!granted.ok()) {
        return granted.error();
      }
      contingent_granted = granted.value();
    }
  }

  // ---------------------------------------------------------------------
  // Phase I: outcome classification and accounting assembly.
  // ---------------------------------------------------------------------
  Bandwidth guaranteed_granted = Bandwidth::zero();
  Bandwidth discretionary_granted = Bandwidth::zero();
  Bandwidth obligations_consumed = Bandwidth::zero();
  for (const WorkItem& item : run.items) {
    // Capacity granted to a reservation holder is funded by that obligation and
    // is accounted as obligation consumption, never as arbitrable grant.
    const Bandwidth from_arbitrable =
        sat_sub(item.allocation.guaranteed, item.obligation_guaranteed);
    const auto add_guaranteed = guaranteed_granted.checked_add(from_arbitrable);
    if (!add_guaranteed.ok()) {
      return add_guaranteed.error();
    }
    guaranteed_granted = add_guaranteed.value();
    const auto add_discretionary = discretionary_granted.checked_add(item.allocation.discretionary);
    if (!add_discretionary.ok()) {
      return add_discretionary.error();
    }
    discretionary_granted = add_discretionary.value();
    const auto add_obligation = obligations_consumed.checked_add(item.obligation_guaranteed);
    if (!add_obligation.ok()) {
      return add_obligation.error();
    }
    obligations_consumed = add_obligation.value();
  }

  // The reserve is reported after every distribution phase, because emergency
  // classes may draw on it during the discretionary phase as well.
  accounting.emergency_reserve_unused = emergency_available;
  accounting.guaranteed_granted = guaranteed_granted;
  accounting.discretionary_granted = discretionary_granted;
  accounting.borrowed_granted = borrowed_granted;
  accounting.contingent_granted = contingent_granted;
  accounting.obligations_consumed = obligations_consumed;
  accounting.obligations_lent = borrowed_granted;
  accounting.obligations_idle = sat_sub(sat_sub(obligations_reserved, obligations_consumed), borrowed_granted);
  accounting.unallocated = remaining_arbitrable;
  accounting.contingent_unallocated = sat_sub(contingent_pool, contingent_granted);
  {
    Bandwidth authorized = obligations_consumed;
    for (const Bandwidth term : {guaranteed_granted, discretionary_granted, borrowed_granted, contingent_granted}) {
      const auto next = authorized.checked_add(term);
      if (!next.ok()) {
        return next.error();
      }
      authorized = next.value();
    }
    accounting.authorized_consumption = authorized;
  }

  // ---------------------------------------------------------------------
  // Phase J: per-request decisions, reasons and grant materialisation.
  // ---------------------------------------------------------------------
  outcome.decisions.reserve(run.items.size());
  for (WorkItem& item : run.items) {
    const BandwidthRequest& request = *item.request;
    ArbitrationDecision decision;
    decision.request = request.id;
    decision.request_generation = request.generation;
    decision.requested_minimum = request.minimum;
    decision.requested_desired = request.desired;
    decision.requested_maximum = request.maximum;
    decision.allocation = item.allocation;
    decision.effective_rank = item.effective_rank;
    decision.effective_weight = item.effective_weight;
    decision.starvation_promoted = item.starvation_promoted;

    const Bandwidth held = total_allocation(item.allocation);
    const std::uint64_t previous_wait = input.candidates[item.candidate_index].wait_rounds;
    decision.next_wait_rounds = (held >= request.desired) ? 0 : previous_wait + 1;
    decision.denied = sat_sub(request.desired, held);
    decision.unmet_minimum = sat_sub(request.minimum, held);
    if (item.unmet_minimum > decision.unmet_minimum) {
      decision.unmet_minimum = item.unmet_minimum;
    }

    if (item.refused) {
      decision.state = GrantState::Rejected;
      decision.reason = item.reason;
      decision.refused = true;
      decision.binding_reason = item.binding_reason.empty()
                                    ? std::string(to_string(item.reason))
                                    : item.binding_reason;
    } else if (item.capacity_unavailable) {
      decision.state = GrantState::Queued;
      decision.reason = item.reason;
      decision.waiting = true;
      decision.binding_reason = item.binding_reason.empty() ? std::string(to_string(item.reason))
                                                            : item.binding_reason;
    } else {
      const bool satisfied = held >= request.desired;
      const bool met_minimum = held >= request.minimum;
      decision.satisfied = satisfied;
      decision.waiting = !satisfied;
      decision.state = satisfied ? (item.allocation.has_revocable() ? GrantState::GrantedBorrowed
                                                                   : GrantState::GrantedGuaranteed)
                                 : GrantState::Queued;
      if (satisfied) {
        decision.reason = (request.maximum > request.desired && held >= request.maximum)
                              ? OutcomeReason::SatisfiedAtMaximum
                              : OutcomeReason::SatisfiedFully;
        decision.binding_reason = "granted";
      } else if (!met_minimum) {
        decision.reason = OutcomeReason::BelowMinimumWaiting;
        decision.binding_reason = item.binding_reason.empty() ? kReasonGuaranteeShortfall : item.binding_reason;
      } else if (held.is_zero() && request.minimum.is_zero()) {
        decision.reason = OutcomeReason::ZeroCapacityAvailable;
        decision.binding_reason = kReasonZeroCapacity;
      } else if (held == request.minimum && request.desired > request.minimum) {
        decision.reason = OutcomeReason::MinimumGuaranteedOnly;
        decision.binding_reason = kReasonZeroCapacity;
      } else {
        decision.reason = OutcomeReason::PartiallySatisfied;
        decision.binding_reason = kReasonFairness;
      }

      if (!satisfied) {
        if (run.group_cap_reached(item.group->id)) {
          decision.binding_reason = kReasonGroupCap;
          decision.reason = met_minimum ? OutcomeReason::PartiallySatisfied : decision.reason;
          if (met_minimum && decision.reason == OutcomeReason::MinimumGuaranteedOnly) {
            decision.reason = OutcomeReason::MinimumGuaranteedOnly;
          }
        } else if (remaining_arbitrable.is_zero() && held.is_zero() && !arbitrable.is_zero()) {
          decision.binding_reason = kReasonStrongerClass;
        } else if (remaining_arbitrable.is_zero() && arbitrable.is_zero()) {
          decision.binding_reason = headroom.is_positive() ? kReasonHeadroom : kReasonZeroCapacity;
          decision.reason = decision.reason == OutcomeReason::BelowMinimumWaiting
                                ? OutcomeReason::BelowMinimumWaiting
                                : OutcomeReason::ZeroCapacityAvailable;
        } else if (request.borrowing_eligible && borrowed_granted.is_zero() &&
                   (total_allocation(item.allocation) < request.maximum) && input.policy.borrow.enabled) {
          decision.binding_reason = kReasonBorrowCeiling;
        }
        if (item.starvation_promoted && decision.binding_reason == kReasonFairness) {
          decision.binding_reason = "weighted max-min fair share limited the allocation after starvation promotion";
        }
      }
      if (item.starvation_promoted) {
        decision.binding_reason = bounded(decision.binding_reason + " (starvation promotion applied)");
      }
    }

    // Grant materialisation.
    const std::optional<Grant>& previous = input.candidates[item.candidate_index].existing_grant;
    AuthorityVector authority = request.authority;
    authority.coordinator = input.coordinator;
    authority.capacity_generation = input.snapshot.generation;
    authority.monotonic_tick = input.tick;

    if (previous.has_value() && previous->is_live()) {
      const Grant historical = *previous;

      if (item.refused || held.is_zero()) {
        Grant terminal = historical;
        const auto bumped = terminal.advance_generation();
        if (!bumped.ok()) {
          return bumped.error();
        }
        GrantState target_state = GrantState::Revoked;
        if (item.reason == OutcomeReason::RefusedBootFenced) {
          target_state = GrantState::Fenced;
        } else if (item.reason == OutcomeReason::RefusedStaleEpoch ||
                   item.reason == OutcomeReason::RefusedStaleResourceGeneration ||
                   item.reason == OutcomeReason::RefusedStalePolicyGeneration ||
                   item.reason == OutcomeReason::RefusedStaleTenantConfig ||
                   item.reason == OutcomeReason::RefusedStaleReservation ||
                   item.reason == OutcomeReason::CapacityWithdrawn) {
          target_state = GrantState::Stale;
        }
        terminal.reason = item.refused ? item.reason : OutcomeReason::Revoked;
        BB_RETURN_IF_ERROR(terminal.transition_to(target_state));
        decision.previous_grant = historical;
        const Bandwidth was_held = total_allocation(historical.allocation);
        if (was_held.is_positive()) {
          RecallRecord recall;
          const auto recall_id = RecallId::make(outcome.next_recall_id);
          if (!recall_id.ok()) {
            return recall_id.error();
          }
          outcome.next_recall_id += 1;
          recall.id = recall_id.value();
          recall.grant = terminal.id;
          recall.grant_generation = terminal.generation;
          recall.request = request.id;
          recall.target = input.target;
          recall.recalled = was_held;
          recall.remaining = Bandwidth::zero();
          recall.reason = target_state == GrantState::Stale ? OutcomeReason::Stale : OutcomeReason::Revoked;
          recall.requested_tick = input.tick;
          recall.deadline_tick = input.tick;
          recall.authority = terminal.authority;
          decision.recall = recall;
        }
        // Terminal grants keep their final allocation as history; accounting is
        // driven by the state, so recalled capacity is already released.
        decision.grant = terminal;
        decision.state = target_state;
      } else {
        const bool binding_same = previous->authority.same_binding(authority);
        const Bandwidth previous_total = total_allocation(previous->allocation);
        const bool amount_same = previous_total == held;
        const bool class_same = (previous->state == GrantState::GrantedGuaranteed) == !item.allocation.has_revocable();
        if (binding_same && amount_same && class_same && previous->state != GrantState::RecallPending) {
          Grant renewed = *previous;
          renewed.decision = input.decision;
          renewed.last_revalidated_tick = input.tick;
          renewed.reason = decision.reason;
          renewed.wait_rounds = decision.satisfied ? 0 : input.candidates[item.candidate_index].wait_rounds + 1;
          renewed.requested_minimum = request.minimum;
          renewed.requested_desired = request.desired;
          renewed.requested_maximum = request.maximum;
          renewed.denied = decision.denied;
          renewed.satisfied = decision.satisfied;
          decision.grant = renewed;
        } else {
          decision.previous_grant = historical;
          Grant updated = *previous;
          const auto bumped = updated.advance_generation();
          if (!bumped.ok()) {
            return bumped.error();
          }
          const bool shrinking = held < previous_total;
          updated.allocation = item.allocation;
          updated.obligation_backed = item.obligation_guaranteed;
          updated.reserve_backed = item.reserve_allocated;
          updated.authority = authority;
          updated.decision = input.decision;
          updated.last_revalidated_tick = input.tick;
          updated.reason = decision.reason;
          updated.wait_rounds = decision.satisfied ? 0 : input.candidates[item.candidate_index].wait_rounds + 1;
          updated.requested_minimum = request.minimum;
          updated.requested_desired = request.desired;
          updated.requested_maximum = request.maximum;
          updated.denied = decision.denied;
          updated.satisfied = decision.satisfied;
          updated.state = item.allocation.has_revocable() ? GrantState::GrantedBorrowed : GrantState::GrantedGuaranteed;
          if (shrinking) {
            RecallRecord recall;
            const auto recall_id = RecallId::make(outcome.next_recall_id);
            if (!recall_id.ok()) {
              return recall_id.error();
            }
            outcome.next_recall_id += 1;
            recall.id = recall_id.value();
            recall.grant = updated.id;
            recall.grant_generation = updated.generation;
            recall.request = request.id;
            recall.target = input.target;
            recall.recalled = sat_sub(previous_total, held);
            recall.remaining = held;
            recall.reason = OutcomeReason::RecalledByStrongerClass;
            recall.requested_tick = input.tick;
            recall.deadline_tick = input.tick + input.policy.preemption.recall_grace_rounds;
            recall.acknowledgement_required = input.policy.preemption.recall_grace_rounds > 0;
            recall.authority = authority;
            decision.recall = recall;
            if (recall.acknowledgement_required) {
              updated.state = GrantState::RecallPending;
            }
          }
          decision.grant = updated;
        }
      }
    } else if (!item.refused && held.is_positive()) {
      Grant grant;
      const auto grant_id = BandwidthGrantId::make(outcome.next_grant_id);
      if (!grant_id.ok()) {
        return grant_id.error();
      }
      if (outcome.next_grant_id == BandwidthGrantId::rep(-1)) {
        return make_error<ArbitrationOutcome>(ErrorCode::OutOfRange, "grant identity space exhausted");
      }
      outcome.next_grant_id += 1;
      grant.id = grant_id.value();
      grant.generation = BandwidthGrantGeneration::initial();
      grant.request = request.id;
      grant.request_generation = request.generation;
      grant.target = input.target;
      grant.authority = authority;
      grant.allocation = item.allocation;
      grant.obligation_backed = item.obligation_guaranteed;
      grant.reserve_backed = item.reserve_allocated;
      grant.requested_minimum = request.minimum;
      grant.requested_desired = request.desired;
      grant.requested_maximum = request.maximum;
      grant.denied = decision.denied;
      grant.satisfied = decision.satisfied;
      grant.reason = decision.reason;
      grant.decision = input.decision;
      grant.issued_tick = input.tick;
      grant.last_revalidated_tick = input.tick;
      grant.wait_rounds = decision.satisfied ? 0 : input.candidates[item.candidate_index].wait_rounds + 1;
      grant.state = item.allocation.has_revocable() ? GrantState::GrantedBorrowed : GrantState::GrantedGuaranteed;
      decision.grant = grant;
      decision.state = grant.state;
    }

    if (decision.grant.has_value()) {
      decision.state = decision.grant->state;
      const auto validated = validate_grant(decision.grant.value());
      if (!validated.ok()) {
        return validated.error();
      }
    }
    outcome.decisions.push_back(std::move(decision));
  }

  std::sort(outcome.decisions.begin(), outcome.decisions.end(),
            [](const ArbitrationDecision& a, const ArbitrationDecision& b) {
              if (a.request != b.request) return a.request < b.request;
              return a.request_generation < b.request_generation;
            });

  const auto violations = accounting.validate();
  if (!violations.empty()) {
    return make_error<ArbitrationOutcome>(ErrorCode::AccountingInvariantViolation,
                                          "arbitration produced inconsistent accounting: " +
                                              describe_violations(violations));
  }
  return outcome;
}

}  // namespace bandwidth_broker
