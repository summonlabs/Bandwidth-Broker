// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Weighted max-min fairness.
//
// The engine implements exact weighted max-min fairness (WMMF) by progressive
// filling, in integer arithmetic, over indivisible quanta. It is hierarchical:
// capacity is shared between branches by branch weight, and inside a branch
// between its leaves by leaf weight, recursively.
//
// Guarantees proven by the test suite:
//   * total conservation  - the sum of allocations plus the unallocated
//                           remainder equals the offered capacity exactly;
//   * monotonicity        - increasing the offered capacity never decreases any
//                           allocation;
//   * demand safety       - no allocation exceeds its demand or its weight
//                           share of the offered capacity;
//   * quantum integrity   - every allocation is a whole multiple of the
//                           quantum, and leftover below one quantum is
//                           reported as unallocated rather than invented;
//   * determinism         - identical input produces identical output, and the
//                           distribution of indivisible quanta is fixed by
//                           (remainder descending, identifier ascending);
//   * isolation           - a branch's allocation depends only on its own
//                           weight and demand plus the aggregate of its peers,
//                           never on the internal composition of its peers.

#ifndef BANDWIDTH_BROKER_FAIRNESS_HPP
#define BANDWIDTH_BROKER_FAIRNESS_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/export.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

// A request-level participant in a fair share computation.
struct BB_API FairnessLeaf final {
  std::size_t id{0};          // caller-assigned identifier; also the tie-break key
  std::uint64_t weight{1};    // must be at least 1
  Bandwidth demand{};         // residual demand offered to this fair share round
};

// A weighted container of leaves and nested branches (for example a fairness
// group inside a priority class).
struct BB_API FairnessBranch final {
  std::uint64_t weight{1};
  std::vector<FairnessLeaf> leaves{};
  std::vector<FairnessBranch> children{};
};

struct BB_API FairnessShare final {
  std::size_t id{0};
  Bandwidth demand{};
  Bandwidth allocated{};
};

struct BB_API FairnessResult final {
  std::vector<FairnessShare> shares{};       // sorted ascending by id
  std::vector<std::size_t> bottlenecked{};   // ids granted less than their demand, ascending
  Bandwidth allocated{};
  Bandwidth unallocated{};
};

// Flat weighted max-min fair share.
[[nodiscard]] BB_API Result<FairnessResult> weighted_max_min_fair_share(Bandwidth capacity,
                                                                        Bandwidth quantum,
                                                                        const std::vector<FairnessLeaf>& leaves);

// Hierarchical weighted max-min fair share.
[[nodiscard]] BB_API Result<FairnessResult> weighted_max_min_fair_share(Bandwidth capacity,
                                                                        Bandwidth quantum,
                                                                        const std::vector<FairnessBranch>& branches);

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_FAIRNESS_HPP
