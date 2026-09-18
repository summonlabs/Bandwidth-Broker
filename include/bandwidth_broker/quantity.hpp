// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef BANDWIDTH_BROKER_QUANTITY_HPP
#define BANDWIDTH_BROKER_QUANTITY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/export.hpp"
#include "bandwidth_broker/numeric.hpp"

namespace bandwidth_broker {

// Exact capacity quantity.
//
// Bandwidth is an exact fixed-point quantity whose quantum is one bit per
// second. It is never a floating point value and never a percentage: policy
// converts percentages into exact integers before any authority-bearing
// computation. All arithmetic is checked; silent wraparound is impossible.
//
// Range: [0, kMaxBitsPerSecond]. Values outside the range are rejected with
// ErrorCode::OutOfRange, never clamped, so that a mis-scaled publisher cannot
// silently inflate or deflate fabric capacity.
class BB_API Bandwidth final {
 public:
  using rep = std::int64_t;

  static constexpr rep kMaxBitsPerSecond = 1'000'000'000'000'000LL;  // 1 Pbit/s

  constexpr Bandwidth() noexcept = default;

  [[nodiscard]] static Result<Bandwidth> from_bits_per_second(rep bits_per_second);
  [[nodiscard]] static Result<Bandwidth> from_kilobits_per_second(rep kbps);
  [[nodiscard]] static Result<Bandwidth> from_megabits_per_second(rep mbps);
  [[nodiscard]] static Result<Bandwidth> from_gigabits_per_second(rep gbps);
  [[nodiscard]] static constexpr Bandwidth zero() noexcept { return Bandwidth(0); }
  [[nodiscard]] static Result<Bandwidth> max() noexcept { return Bandwidth(kMaxBitsPerSecond); }

  [[nodiscard]] constexpr rep bits_per_second() const noexcept { return bits_per_second_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return bits_per_second_ == 0; }
  [[nodiscard]] constexpr bool is_positive() const noexcept { return bits_per_second_ > 0; }

  [[nodiscard]] Result<Bandwidth> checked_add(Bandwidth other) const noexcept;
  [[nodiscard]] Result<Bandwidth> checked_sub(Bandwidth other) const noexcept;
  [[nodiscard]] Result<Bandwidth> checked_mul(std::uint64_t factor) const noexcept;
  // floor(this * numerator / denominator); exact integer arithmetic.
  [[nodiscard]] Result<Bandwidth> scaled(std::uint64_t numerator, std::uint64_t denominator) const noexcept;

  // Exact sum. Returns ErrorCode::NumericOverflow if the total leaves range.
  [[nodiscard]] static Result<Bandwidth> sum(const Bandwidth* values, std::size_t count) noexcept;
  [[nodiscard]] static Result<Bandwidth> sum(const std::vector<Bandwidth>& values) noexcept;

  [[nodiscard]] static constexpr Bandwidth saturating_sub(Bandwidth a, Bandwidth b) noexcept {
    return Bandwidth(a.bits_per_second_ >= b.bits_per_second_ ? a.bits_per_second_ - b.bits_per_second_ : 0);
  }
  [[nodiscard]] static constexpr Bandwidth min(Bandwidth a, Bandwidth b) noexcept {
    return a.bits_per_second_ <= b.bits_per_second_ ? a : b;
  }
  [[nodiscard]] static constexpr Bandwidth max_of(Bandwidth a, Bandwidth b) noexcept {
    return a.bits_per_second_ >= b.bits_per_second_ ? a : b;
  }

  [[nodiscard]] friend constexpr bool operator==(Bandwidth a, Bandwidth b) noexcept { return a.bits_per_second_ == b.bits_per_second_; }
  [[nodiscard]] friend constexpr bool operator!=(Bandwidth a, Bandwidth b) noexcept { return a.bits_per_second_ != b.bits_per_second_; }
  [[nodiscard]] friend constexpr bool operator<(Bandwidth a, Bandwidth b) noexcept { return a.bits_per_second_ < b.bits_per_second_; }
  [[nodiscard]] friend constexpr bool operator<=(Bandwidth a, Bandwidth b) noexcept { return a.bits_per_second_ <= b.bits_per_second_; }
  [[nodiscard]] friend constexpr bool operator>(Bandwidth a, Bandwidth b) noexcept { return a.bits_per_second_ > b.bits_per_second_; }
  [[nodiscard]] friend constexpr bool operator>=(Bandwidth a, Bandwidth b) noexcept { return a.bits_per_second_ >= b.bits_per_second_; }

  [[nodiscard]] std::string to_string() const;

 private:
  explicit constexpr Bandwidth(rep bits_per_second) noexcept : bits_per_second_(bits_per_second) {}
  rep bits_per_second_{0};
};

// Exact time span in nanoseconds. Used for lease/recall deadlines that are
// expressed relative to the coordinator's monotonic clock.
class BB_API Duration final {
 public:
  using rep = std::int64_t;
  static constexpr rep kMaxNanoseconds = 1'000'000'000'000'000'000LL;  // ~31.7 years

  constexpr Duration() noexcept = default;

  [[nodiscard]] static Result<Duration> from_nanoseconds(rep ns) noexcept;
  [[nodiscard]] static Result<Duration> from_milliseconds(rep ms) noexcept;
  [[nodiscard]] static Result<Duration> from_seconds(rep s) noexcept;
  [[nodiscard]] static constexpr Duration zero() noexcept { return Duration(0); }

  [[nodiscard]] constexpr rep nanoseconds() const noexcept { return ns_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return ns_ == 0; }

  [[nodiscard]] friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.ns_ == b.ns_; }
  [[nodiscard]] friend constexpr bool operator!=(Duration a, Duration b) noexcept { return a.ns_ != b.ns_; }
  [[nodiscard]] friend constexpr bool operator<(Duration a, Duration b) noexcept { return a.ns_ < b.ns_; }
  [[nodiscard]] friend constexpr bool operator<=(Duration a, Duration b) noexcept { return a.ns_ <= b.ns_; }

 private:
  explicit constexpr Duration(rep ns) noexcept : ns_(ns) {}
  rep ns_{0};
};

// Wall-clock instant in nanoseconds since the Unix epoch.
//
// Timestamps are recorded for audit and human readability only. They never
// participate in an authority decision: ordering and expiry are driven by the
// coordinator's monotonic tick sequence so that clock changes cannot resurrect
// or expire authority.
class BB_API Timestamp final {
 public:
  using rep = std::int64_t;

  constexpr Timestamp() noexcept = default;
  [[nodiscard]] static Result<Timestamp> from_unix_nanoseconds(rep ns) noexcept;

  [[nodiscard]] constexpr rep unix_nanoseconds() const noexcept { return ns_; }
  [[nodiscard]] friend constexpr bool operator==(Timestamp a, Timestamp b) noexcept { return a.ns_ == b.ns_; }
  [[nodiscard]] friend constexpr bool operator<(Timestamp a, Timestamp b) noexcept { return a.ns_ < b.ns_; }

 private:
  explicit constexpr Timestamp(rep ns) noexcept : ns_(ns) {}
  rep ns_{0};
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_QUANTITY_HPP
