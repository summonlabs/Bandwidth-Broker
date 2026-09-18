// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/checksum.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/numeric.hpp"
#include "bandwidth_broker/quantity.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;

BB_TEST(Numeric, MulMatchesKnownValues) {
  const U128 product = u128_mul(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
  BB_CHECK_EQ(product.hi, 0xFFFFFFFFFFFFFFFEULL);
  BB_CHECK_EQ(product.lo, 0x0000000000000001ULL);

  const U128 small = u128_mul(3, 5);
  BB_CHECK_EQ(small.hi, 0ULL);
  BB_CHECK_EQ(small.lo, 15ULL);
}

BB_TEST(Numeric, DivisionIsExact) {
  bool overflow = false;
  std::uint64_t rem = 0;
  const std::uint64_t quotient = u128_div_u64(u128_mul(1'000'000'000ULL, 7ULL), 3ULL, rem, overflow);
  BB_CHECK(!overflow);
  BB_CHECK_EQ(quotient, 2'333'333'333ULL);
  BB_CHECK_EQ(rem, 1ULL);
}

BB_TEST(Numeric, MulDivDetectsOverflow) {
  bool overflow = false;
  (void)mul_div_floor_u64(UINT64_MAX, UINT64_MAX, 1, overflow);
  BB_CHECK(overflow);
}

BB_TEST(Quantity, RejectsOutOfRange) {
  BB_CHECK_ERR(ErrorCode::OutOfRange, Bandwidth::from_bits_per_second(-1));
  BB_CHECK_ERR(ErrorCode::OutOfRange, Bandwidth::from_bits_per_second(Bandwidth::kMaxBitsPerSecond + 1));
  BB_CHECK_OK(Bandwidth::from_bits_per_second(Bandwidth::kMaxBitsPerSecond));
}

BB_TEST(Quantity, CheckedArithmetic) {
  BB_REQUIRE_OK(a, Bandwidth::from_bits_per_second(100));
  BB_REQUIRE_OK(b, Bandwidth::from_bits_per_second(40));
  BB_REQUIRE_OK(sum, a.checked_add(b));
  BB_CHECK_EQ(sum.bits_per_second(), 140);
  BB_REQUIRE_OK(diff, a.checked_sub(b));
  BB_CHECK_EQ(diff.bits_per_second(), 60);
  BB_CHECK_ERR(ErrorCode::NumericOverflow, b.checked_sub(a));
}

BB_TEST(Checksum, Crc32cKnownVector) {
  // Standard CRC-32C check value for "123456789".
  BB_CHECK_EQ(Crc32c::compute("123456789"), 0xE3069283u);
}

BB_TEST(Identity, IdRejectsZero) {
  BB_CHECK_ERR(ErrorCode::InvalidIdentity, BandwidthResourceId::make(0));
  BB_REQUIRE_OK(id, BandwidthResourceId::make(42));
  BB_CHECK_EQ(id.value(), 42ULL);
  BB_CHECK(id.valid());
}

BB_TEST(Identity, GenerationNeverWraps) {
  auto generation = BandwidthResourceGeneration::initial();
  for (int i = 0; i < 5; ++i) {
    BB_REQUIRE_OK(next, generation.next());
    generation = next;
  }
  BB_CHECK_EQ(generation.value(), 6ULL);

  const auto exhausted = BandwidthResourceGeneration::from_value(BandwidthResourceGeneration::kMaxValue);
  BB_CHECK_ERR(ErrorCode::OutOfRange, exhausted.next());
}

BB_TEST(Identity, BootIdRoundTrip) {
  const BootId boot = boot_id_from_u64(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL);
  BB_CHECK(boot.valid());
  BB_REQUIRE_OK(parsed, BootId::parse(boot.to_string()));
  BB_CHECK(parsed == boot);
}
