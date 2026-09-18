// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Exact integer arithmetic helpers. The arbitration engine performs all fairness
// and capacity arithmetic in exact integer form; floating point is never used in
// an authority-bearing path. Products of two 64-bit quantities are computed in a
// portable 128-bit accumulator so that no intermediate value can wrap silently.

#ifndef BANDWIDTH_BROKER_NUMERIC_HPP
#define BANDWIDTH_BROKER_NUMERIC_HPP

#include <cstdint>
#include <limits>

namespace bandwidth_broker {

// Unsigned 128-bit accumulator, built from 64-bit limbs only so that the
// behaviour is identical on every supported compiler and platform.
struct U128 final {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }
  [[nodiscard]] constexpr bool fits_u64() const noexcept { return hi == 0; }
};

// Portable 64x64 -> 128 unsigned multiply (four 32-bit partial products).
[[nodiscard]] constexpr U128 u128_mul(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
  const std::uint64_t b_hi = b >> 32;

  const std::uint64_t p0 = a_lo * b_lo;
  const std::uint64_t p1 = a_lo * b_hi;
  const std::uint64_t p2 = a_hi * b_lo;
  const std::uint64_t p3 = a_hi * b_hi;

  const std::uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);

  U128 r;
  r.lo = (mid << 32) | (p0 & 0xFFFFFFFFULL);
  r.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
  return r;
}

// Unsigned 128-bit addition carrying a 128-bit overflow flag.
[[nodiscard]] constexpr U128 u128_add(U128 a, U128 b, bool& overflow) noexcept {
  U128 r;
  r.lo = a.lo + b.lo;
  const std::uint64_t carry = (r.lo < a.lo) ? 1ULL : 0ULL;
  r.hi = a.hi + b.hi;
  const bool hi_carry = (r.hi < a.hi);
  r.hi += carry;
  overflow = hi_carry || (r.hi < carry);
  return r;
}

// Three-way unsigned 128-bit comparison: -1, 0 or 1.
[[nodiscard]] constexpr int u128_cmp(U128 a, U128 b) noexcept {
  if (a.hi != b.hi) return (a.hi < b.hi) ? -1 : 1;
  if (a.lo != b.lo) return (a.lo < b.lo) ? -1 : 1;
  return 0;
}

// Overflow-safe comparison of two 64x64 products: returns -1, 0 or 1 for
// (a*b) <=> (c*d) without ever forming a truncated intermediate.
[[nodiscard]] constexpr int cmp_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept {
  return u128_cmp(u128_mul(a, b), u128_mul(c, d));
}

// 128 / 64 -> 64 division with remainder. Sets p overflow when the true
// quotient does not fit in 64 bits, in which case the returned value is
// unspecified and must not be used.
[[nodiscard]] constexpr std::uint64_t u128_div_u64(U128 n, std::uint64_t d, std::uint64_t& rem, bool& overflow) noexcept {
  overflow = false;
  rem = 0;
  if (d == 0) {
    overflow = true;
    return 0;
  }
  if (n.hi == 0) {
    rem = n.lo % d;
    return n.lo / d;
  }
  std::uint64_t q = 0;
  std::uint64_t r = 0;
  for (int i = 127; i >= 0; --i) {
    const std::uint64_t bit = (i >= 64) ? ((n.hi >> (i - 64)) & 1ULL) : ((n.lo >> i) & 1ULL);
    const std::uint64_t carry = r >> 63;
    r = (r << 1) | bit;
    if (carry != 0 || r >= d) {
      r -= d;
      if (i >= 64) {
        overflow = true;  // quotient would exceed 64 bits
        return 0;
      }
      q |= (1ULL << i);
    }
  }
  rem = r;
  return q;
}

// floor(a * b / d) computed exactly. Sets p overflow when the quotient does
// not fit in 64 bits.
[[nodiscard]] constexpr std::uint64_t mul_div_floor_u64(std::uint64_t a, std::uint64_t b, std::uint64_t d, bool& overflow) noexcept {
  std::uint64_t rem = 0;
  return u128_div_u64(u128_mul(a, b), d, rem, overflow);
}

// floor(a / b) for unsigned values with an explicit zero check.
[[nodiscard]] constexpr std::uint64_t div_floor_u64(std::uint64_t a, std::uint64_t b, bool& overflow) noexcept {
  overflow = (b == 0);
  return overflow ? 0 : (a / b);
}

[[nodiscard]] constexpr std::uint64_t add_u64(std::uint64_t a, std::uint64_t b, bool& overflow) noexcept {
  const std::uint64_t s = a + b;
  overflow = (s < a);
  return s;
}

// Exact ceiling division for non-negative values. Callers must guarantee b != 0.
[[nodiscard]] constexpr std::uint64_t div_ceil_u64(std::uint64_t a, std::uint64_t b) noexcept {
  return (a / b) + ((a % b) != 0 ? 1ULL : 0ULL);
}

template <typename T>
[[nodiscard]] constexpr bool fits_in_u64(T v) noexcept {
  return v >= 0 && static_cast<std::uint64_t>(v) <= std::numeric_limits<std::uint64_t>::max();
}

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_NUMERIC_HPP
