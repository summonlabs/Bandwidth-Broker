// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/fairness.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/numeric.hpp"

namespace bandwidth_broker {
namespace {

struct UnitDemand final {
  std::size_t id{0};
  std::uint64_t weight{1};
  std::uint64_t units{0};
  std::uint64_t allocated{0};
};

// Exact weight * units product comparison: -1, 0 or 1 for (a_units/a_weight)
// compared against (b_units/b_weight).
[[nodiscard]] int compare_ratio(std::uint64_t a_units, std::uint64_t a_weight, std::uint64_t b_units,
                                std::uint64_t b_weight) noexcept {
  return cmp_mul_u64(a_units, b_weight, b_units, a_weight);
}

[[nodiscard]] Result<std::uint64_t> scaled_units(std::uint64_t units, std::uint64_t numerator,
                                                 std::uint64_t denominator) {
  if (denominator == 0) {
    return make_error<std::uint64_t>(ErrorCode::InvalidArgument, "fair share denominator must not be zero");
  }
  bool overflow = false;
  const std::uint64_t result = mul_div_floor_u64(units, numerator, denominator, overflow);
  if (overflow) {
    return make_error<std::uint64_t>(ErrorCode::NumericOverflow, "fair share arithmetic overflowed");
  }
  return result;
}

// Core progressive filling over indivisible units.
[[nodiscard]] Status wmmf_units(std::uint64_t capacity_units, std::vector<UnitDemand>& demands) {
  std::vector<std::size_t> active;
  active.reserve(demands.size());
  for (std::size_t i = 0; i < demands.size(); ++i) {
    if (demands[i].units > 0 && demands[i].weight > 0) {
      active.push_back(i);
    }
  }

  std::uint64_t remaining = capacity_units;
  while (!active.empty() && remaining > 0) {
    // Deterministic bottleneck selection: minimum units/weight ratio, lowest
    // identifier on ties.
    std::size_t min_position = 0;
    for (std::size_t position = 1; position < active.size(); ++position) {
      const UnitDemand& candidate = demands[active[position]];
      const UnitDemand& incumbent = demands[active[min_position]];
      const int order = compare_ratio(candidate.units, candidate.weight, incumbent.units, incumbent.weight);
      if (order < 0) {
        min_position = position;
      }
    }
    const std::size_t bottleneck = active[min_position];
    const std::uint64_t level_units = demands[bottleneck].units;
    const std::uint64_t level_weight = demands[bottleneck].weight;

    // Capacity required to satisfy the bottleneck and lift every other active
    // demand to the same level.
    std::uint64_t needed = level_units;
    bool overflow = false;
    for (const std::size_t index : active) {
      if (index == bottleneck) {
        continue;
      }
      const auto share = scaled_units(level_units, demands[index].weight, level_weight);
      if (!share.ok()) {
        return share.error();
      }
      needed = add_u64(needed, share.value(), overflow);
      if (overflow) {
        return make_error_status(ErrorCode::NumericOverflow, "fair share requirement overflowed");
      }
    }

    if (remaining >= needed) {
      for (const std::size_t index : active) {
        if (index == bottleneck) {
          continue;
        }
        const auto share = scaled_units(level_units, demands[index].weight, level_weight);
        if (!share.ok()) {
          return share.error();
        }
        demands[index].allocated += share.value();
        demands[index].units -= share.value();
      }
      demands[bottleneck].allocated += level_units;
      demands[bottleneck].units = 0;
      remaining -= needed;
      active.erase(active.begin() + static_cast<std::ptrdiff_t>(min_position));
      continue;
    }

    // The offered capacity cannot reach the next water level: share it
    // proportionally and distribute the indivisible remainder deterministically.
    std::uint64_t total_weight = 0;
    for (const std::size_t index : active) {
      total_weight = add_u64(total_weight, demands[index].weight, overflow);
      if (overflow) {
        return make_error_status(ErrorCode::NumericOverflow, "fair share weight total overflowed");
      }
    }
    std::uint64_t distributed = 0;
    std::vector<std::pair<std::uint64_t, std::size_t>> remainders;
    remainders.reserve(active.size());
    for (const std::size_t index : active) {
      bool div_overflow = false;
      std::uint64_t remainder = 0;
      const std::uint64_t quotient =
          u128_div_u64(u128_mul(remaining, demands[index].weight), total_weight, remainder, div_overflow);
      if (div_overflow) {
        return make_error_status(ErrorCode::NumericOverflow, "fair share distribution overflowed");
      }
      demands[index].allocated += quotient;
      distributed += quotient;
      remainders.emplace_back(remainder, index);
    }
    std::uint64_t leftover = remaining - distributed;
    std::sort(remainders.begin(), remainders.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first) {
        return a.first > b.first;
      }
      return a.second < b.second;
    });
    for (std::size_t i = 0; i < remainders.size() && leftover > 0; ++i, --leftover) {
      demands[remainders[i].second].allocated += 1;
    }
    remaining = 0;
    break;
  }

  return Status::success();
}

[[nodiscard]] Result<std::uint64_t> to_units(Bandwidth value, std::uint64_t quantum_bits) {
  if (quantum_bits == 0) {
    return make_error<std::uint64_t>(ErrorCode::InvalidArgument, "allocation quantum must be positive");
  }
  return static_cast<std::uint64_t>(value.bits_per_second()) / quantum_bits;
}

[[nodiscard]] Result<Bandwidth> from_units(std::uint64_t units, std::uint64_t quantum_bits) {
  bool overflow = false;
  const std::uint64_t bits = mul_div_floor_u64(units, quantum_bits, 1, overflow);
  if (overflow) {
    return make_error<Bandwidth>(ErrorCode::NumericOverflow, "fair share allocation overflowed");
  }
  return Bandwidth::from_bits_per_second(static_cast<Bandwidth::rep>(bits));
}

[[nodiscard]] std::uint64_t branch_units(const FairnessBranch& branch, std::uint64_t quantum_bits) {
  std::uint64_t total = 0;
  for (const FairnessLeaf& leaf : branch.leaves) {
    total += static_cast<std::uint64_t>(leaf.demand.bits_per_second()) / quantum_bits;
  }
  for (const FairnessBranch& child : branch.children) {
    total += branch_units(child, quantum_bits);
  }
  return total;
}

void collect_leaves(const FairnessBranch& branch, std::vector<FairnessLeaf>& out) {
  for (const FairnessLeaf& leaf : branch.leaves) {
    out.push_back(leaf);
  }
  for (const FairnessBranch& child : branch.children) {
    collect_leaves(child, out);
  }
}

// Allocates capacity_units inside a branch. Returns the units actually used.
[[nodiscard]] Result<std::uint64_t> allocate_branch(std::uint64_t capacity_units,
                                                    const FairnessBranch& branch,
                                                    std::uint64_t quantum_bits,
                                                    std::vector<UnitDemand>& leaf_allocations) {
  std::vector<UnitDemand> items;
  items.reserve(branch.leaves.size() + branch.children.size());
  for (const FairnessLeaf& leaf : branch.leaves) {
    UnitDemand item;
    item.id = leaf.id;
    item.weight = leaf.weight;
    item.units = static_cast<std::uint64_t>(leaf.demand.bits_per_second()) / quantum_bits;
    items.push_back(item);
  }
  for (const FairnessBranch& child : branch.children) {
    UnitDemand item;
    // Branch items participate in the fair share computation with the branch's
    // weight and its aggregate demand. Their identifier is never surfaced: only
    // leaf items are appended to leaf_allocations, so a branch id can never
    // collide with a leaf id in the result.
    item.id = leaf_allocations.size() + items.size();
    item.weight = child.weight;
    item.units = branch_units(child, quantum_bits);
    items.push_back(item);
  }

  BB_RETURN_IF_ERROR(wmmf_units(capacity_units, items));

  std::uint64_t used = 0;
  for (std::size_t i = 0; i < branch.leaves.size(); ++i) {
    leaf_allocations.push_back(items[i]);
    used += items[i].allocated;
  }
  for (std::size_t i = 0; i < branch.children.size(); ++i) {
    const auto child_used = allocate_branch(items[branch.leaves.size() + i].allocated, branch.children[i], quantum_bits,
                                            leaf_allocations);
    if (!child_used.ok()) {
      return child_used.error();
    }
    used += child_used.value();
  }
  return used;
}

}  // namespace

Result<FairnessResult> weighted_max_min_fair_share(Bandwidth capacity,
                                                   Bandwidth quantum,
                                                   const std::vector<FairnessLeaf>& leaves) {
  FairnessBranch root;
  root.weight = 1;
  root.leaves = leaves;
  const std::vector<FairnessBranch> branches{root};
  auto result = weighted_max_min_fair_share(capacity, quantum, branches);
  return result;
}

Result<FairnessResult> weighted_max_min_fair_share(Bandwidth capacity,
                                                   Bandwidth quantum,
                                                   const std::vector<FairnessBranch>& branches) {
  if (!quantum.is_positive()) {
    return make_error<FairnessResult>(ErrorCode::InvalidArgument, "allocation quantum must be positive");
  }
  if (capacity.bits_per_second() < 0) {
    return make_error<FairnessResult>(ErrorCode::InvalidArgument, "fair share capacity must not be negative");
  }
  const std::uint64_t quantum_bits = static_cast<std::uint64_t>(quantum.bits_per_second());

  std::vector<FairnessLeaf> all_leaves;
  for (const FairnessBranch& branch : branches) {
    collect_leaves(branch, all_leaves);
  }
  if (all_leaves.size() > limits::kMaxRequestsPerRound) {
    return make_error<FairnessResult>(ErrorCode::BoundsExceeded, "too many participants in a fair share round");
  }
  std::unordered_map<std::size_t, Bandwidth> demand_by_id;
  demand_by_id.reserve(all_leaves.size());
  for (const FairnessLeaf& leaf : all_leaves) {
    if (leaf.weight == 0) {
      return make_error<FairnessResult>(ErrorCode::InvalidArgument, "fair share weight must be at least 1");
    }
    if (leaf.weight > limits::kMaxWeight) {
      return make_error<FairnessResult>(ErrorCode::BoundsExceeded, "fair share weight exceeds the supported maximum");
    }
    if (!demand_by_id.emplace(leaf.id, leaf.demand).second) {
      return make_error<FairnessResult>(ErrorCode::InvalidArgument,
                                        "fair share participant identifiers must be unique");
    }
  }

  const auto capacity_units = to_units(capacity, quantum_bits);
  if (!capacity_units.ok()) {
    return capacity_units.error();
  }

  std::vector<UnitDemand> leaf_allocations;
  leaf_allocations.reserve(all_leaves.size());

  FairnessBranch root;
  root.weight = 1;
  root.children = branches;
  const auto used = allocate_branch(capacity_units.value(), root, quantum_bits, leaf_allocations);
  if (!used.ok()) {
    return used.error();
  }

  FairnessResult result;
  result.shares.reserve(leaf_allocations.size());
  for (const UnitDemand& item : leaf_allocations) {
    FairnessShare share;
    share.id = item.id;
    share.demand = demand_by_id.at(item.id);
    const auto allocated = from_units(item.allocated, quantum_bits);
    if (!allocated.ok()) {
      return allocated.error();
    }
    share.allocated = allocated.value();
    result.shares.push_back(share);
  }
  std::sort(result.shares.begin(), result.shares.end(),
            [](const FairnessShare& a, const FairnessShare& b) { return a.id < b.id; });

  Bandwidth allocated_total = Bandwidth::zero();
  for (const FairnessShare& share : result.shares) {
    const auto next = allocated_total.checked_add(share.allocated);
    if (!next.ok()) {
      return next.error();
    }
    allocated_total = next.value();
    if (share.allocated < share.demand) {
      result.bottlenecked.push_back(share.id);
    }
  }
  if (allocated_total > capacity) {
    return make_error<FairnessResult>(ErrorCode::AccountingInvariantViolation,
                                      "fair share exceeded the offered capacity");
  }
  result.allocated = allocated_total;
  result.unallocated = Bandwidth::saturating_sub(capacity, allocated_total);
  std::sort(result.bottlenecked.begin(), result.bottlenecked.end());
  return result;
}

}  // namespace bandwidth_broker
