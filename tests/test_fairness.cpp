// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof obligations for the weighted max-min fairness engine.

#include <cstddef>
#include <utility>
#include <vector>

#include "bandwidth_broker/fairness.hpp"
#include "bandwidth_broker/limits.hpp"
#include "support/fixtures.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;
using bb_fixture::bw;

namespace {

Bandwidth sum_shares(const FairnessResult& result) {
  Bandwidth total = Bandwidth::zero();
  for (const FairnessShare& share : result.shares) {
    total = total.checked_add(share.allocated).value();
  }
  return total;
}

std::vector<FairnessLeaf> equal_demands(std::size_t count, std::int64_t demand) {
  std::vector<FairnessLeaf> leaves;
  for (std::size_t i = 0; i < count; ++i) {
    leaves.push_back(FairnessLeaf{i, 1, bw(demand)});
  }
  return leaves;
}

}  // namespace

BB_TEST(Fairness, ConservesCapacityExactly) {
  for (std::int64_t capacity = 0; capacity <= 1000; capacity += 37) {
    const auto result = weighted_max_min_fair_share(bw(capacity), bw(1), equal_demands(7, 400));
    BB_REQUIRE(result.ok());
    BB_CHECK_EQ(result.value().allocated.bits_per_second() + result.value().unallocated.bits_per_second(), capacity);
    BB_CHECK_EQ(sum_shares(result.value()).bits_per_second(), result.value().allocated.bits_per_second());
  }
}

BB_TEST(Fairness, NeverExceedsDemand) {
  const auto leaves = equal_demands(4, 100);
  const auto result = weighted_max_min_fair_share(bw(100000), bw(1), leaves);
  BB_REQUIRE(result.ok());
  for (const FairnessShare& share : result.value().shares) {
    BB_CHECK(share.allocated <= share.demand);
    BB_CHECK_EQ(share.allocated.bits_per_second(), 100);
  }
  BB_CHECK_EQ(result.value().unallocated.bits_per_second(), 100000 - 400);
}

BB_TEST(Fairness, EqualDemandsShareEqually) {
  const auto result = weighted_max_min_fair_share(bw(100), bw(1), equal_demands(4, 1000));
  BB_REQUIRE(result.ok());
  for (const FairnessShare& share : result.value().shares) {
    BB_CHECK_EQ(share.allocated.bits_per_second(), 25);
  }
}

BB_TEST(Fairness, IndivisibleRemainderIsDeterministic) {
  // 10 units across 3 equal demands: 4, 3, 3 in ascending identifier order.
  const auto result = weighted_max_min_fair_share(bw(10), bw(1), equal_demands(3, 100));
  BB_REQUIRE(result.ok());
  BB_CHECK_EQ(result.value().shares[0].allocated.bits_per_second(), 4);
  BB_CHECK_EQ(result.value().shares[1].allocated.bits_per_second(), 3);
  BB_CHECK_EQ(result.value().shares[2].allocated.bits_per_second(), 3);

  // Repeating with a permuted identifier assignment must produce the same
  // mapping from identifier to allocation.
  std::vector<FairnessLeaf> permuted = equal_demands(3, 100);
  std::swap(permuted[0].id, permuted[2].id);
  const auto again = weighted_max_min_fair_share(bw(10), bw(1), permuted);
  BB_REQUIRE(again.ok());
  BB_CHECK_EQ(again.value().shares[0].allocated.bits_per_second(), 3);
  BB_CHECK_EQ(again.value().shares[1].allocated.bits_per_second(), 3);
  BB_CHECK_EQ(again.value().shares[2].allocated.bits_per_second(), 4);
}

BB_TEST(Fairness, WeightedSharesAreProportional) {
  std::vector<FairnessLeaf> leaves;
  leaves.push_back(FairnessLeaf{0, 3, bw(1'000'000)});
  leaves.push_back(FairnessLeaf{1, 1, bw(1'000'000)});
  const auto result = weighted_max_min_fair_share(bw(400), bw(1), leaves);
  BB_REQUIRE(result.ok());
  BB_CHECK_EQ(result.value().shares[0].allocated.bits_per_second(), 300);
  BB_CHECK_EQ(result.value().shares[1].allocated.bits_per_second(), 100);
}

BB_TEST(Fairness, MonotoneInCapacity) {
  const std::vector<FairnessLeaf> leaves = {
      FairnessLeaf{0, 1, bw(500)}, FairnessLeaf{1, 2, bw(900)}, FairnessLeaf{2, 5, bw(2000)}};
  Bandwidth previous = Bandwidth::zero();
  for (std::int64_t capacity = 0; capacity <= 3000; capacity += 13) {
    const auto result = weighted_max_min_fair_share(bw(capacity), bw(1), leaves);
    BB_REQUIRE(result.ok());
    const Bandwidth current = result.value().shares[0].allocated;
    BB_CHECK(current >= previous);
    previous = current;
  }
}

BB_TEST(Fairness, QuantizesAllocations) {
  // 950 bps with a 100 bps quantum is 9 whole quanta plus 50 bps that cannot be
  // allocated. The remainder is reported as unallocated, never invented.
  const auto result = weighted_max_min_fair_share(bw(950), bw(100), equal_demands(3, 1000));
  BB_REQUIRE(result.ok());
  for (const FairnessShare& share : result.value().shares) {
    BB_CHECK_EQ(share.allocated.bits_per_second() % 100, 0);
  }
  BB_CHECK_EQ(result.value().allocated.bits_per_second(), 900);
  BB_CHECK_EQ(result.value().unallocated.bits_per_second(), 50);

  const auto exact = weighted_max_min_fair_share(bw(1000), bw(100), equal_demands(3, 1000));
  BB_REQUIRE(exact.ok());
  BB_CHECK_EQ(exact.value().allocated.bits_per_second(), 1000);
  BB_CHECK_EQ(exact.value().unallocated.bits_per_second(), 0);
}

BB_TEST(Fairness, DemandBelowQuantumReceivesNothing) {
  std::vector<FairnessLeaf> leaves;
  leaves.push_back(FairnessLeaf{0, 1, bw(50)});
  leaves.push_back(FairnessLeaf{1, 1, bw(10'000)});
  const auto result = weighted_max_min_fair_share(bw(10'000), bw(100), leaves);
  BB_REQUIRE(result.ok());
  BB_CHECK_EQ(result.value().shares[0].allocated.bits_per_second(), 0);
  BB_CHECK_EQ(result.value().shares[1].allocated.bits_per_second(), 10'000);
}

BB_TEST(Fairness, HierarchicalWeightsIsolateBranches) {
  // Two branches, weights 1 and 3, each with two equal leaves. The branch
  // weight, not the leaf count, decides the split.
  FairnessBranch light;
  light.weight = 1;
  light.leaves = {FairnessLeaf{0, 1, bw(10'000)}, FairnessLeaf{1, 1, bw(10'000)}};
  FairnessBranch heavy;
  heavy.weight = 3;
  heavy.leaves = {FairnessLeaf{2, 1, bw(10'000)}, FairnessLeaf{3, 1, bw(10'000)}};
  const std::vector<FairnessBranch> branches{light, heavy};
  const auto result = weighted_max_min_fair_share(bw(800), bw(1), branches);
  BB_REQUIRE(result.ok());
  BB_CHECK_EQ(result.value().shares[0].allocated.bits_per_second(), 100);
  BB_CHECK_EQ(result.value().shares[1].allocated.bits_per_second(), 100);
  BB_CHECK_EQ(result.value().shares[2].allocated.bits_per_second(), 300);
  BB_CHECK_EQ(result.value().shares[3].allocated.bits_per_second(), 300);
  BB_CHECK_EQ(result.value().allocated.bits_per_second(), 800);
}

BB_TEST(Fairness, BottleneckedListIsExact) {
  std::vector<FairnessLeaf> leaves;
  leaves.push_back(FairnessLeaf{0, 1, bw(10)});
  leaves.push_back(FairnessLeaf{1, 1, bw(10'000)});
  const auto result = weighted_max_min_fair_share(bw(100), bw(1), leaves);
  BB_REQUIRE(result.ok());
  BB_CHECK_EQ(result.value().shares[0].allocated.bits_per_second(), 10);
  BB_CHECK_EQ(result.value().shares[1].allocated.bits_per_second(), 90);
  BB_REQUIRE(result.value().bottlenecked.size() == 1);
  BB_CHECK_EQ(result.value().bottlenecked[0], std::size_t{1});
}

BB_TEST(Fairness, RejectsMalformedInput) {
  std::vector<FairnessLeaf> leaves = {FairnessLeaf{0, 0, bw(10)}};
  BB_CHECK_ERR(ErrorCode::InvalidArgument, weighted_max_min_fair_share(bw(100), bw(1), leaves));

  std::vector<FairnessLeaf> duplicated = {FairnessLeaf{0, 1, bw(10)}, FairnessLeaf{0, 1, bw(10)}};
  BB_CHECK_ERR(ErrorCode::InvalidArgument, weighted_max_min_fair_share(bw(100), bw(1), duplicated));

  std::vector<FairnessLeaf> heavy = {FairnessLeaf{0, limits::kMaxWeight + 1, bw(10)}};
  BB_CHECK_ERR(ErrorCode::BoundsExceeded, weighted_max_min_fair_share(bw(100), bw(1), heavy));

  BB_CHECK_ERR(ErrorCode::InvalidArgument, weighted_max_min_fair_share(bw(100), Bandwidth::zero(), equal_demands(1, 10)));
}

BB_TEST(Fairness, HugeWeightsDoNotOverflow) {
  std::vector<FairnessLeaf> leaves;
  leaves.push_back(FairnessLeaf{0, limits::kMaxWeight, bw(Bandwidth::kMaxBitsPerSecond)});
  leaves.push_back(FairnessLeaf{1, limits::kMaxWeight, bw(Bandwidth::kMaxBitsPerSecond)});
  const auto result = weighted_max_min_fair_share(bw(Bandwidth::kMaxBitsPerSecond), bw(1), leaves);
  BB_REQUIRE(result.ok());
  BB_CHECK_EQ(result.value().allocated.bits_per_second() + result.value().unallocated.bits_per_second(),
              Bandwidth::kMaxBitsPerSecond);
  BB_CHECK_EQ(result.value().allocated.bits_per_second(), Bandwidth::kMaxBitsPerSecond);
}
