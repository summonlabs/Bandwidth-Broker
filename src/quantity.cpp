// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/quantity.hpp"

#include <cstdio>
#include <limits>

namespace bandwidth_broker {
namespace {

[[nodiscard]] Result<Bandwidth> checked_scale(std::int64_t value, std::int64_t factor) {
  if (value < 0) {
    return make_error<Bandwidth>(ErrorCode::OutOfRange, "bandwidth value must not be negative");
  }
  if (value > Bandwidth::kMaxBitsPerSecond / factor) {
    return make_error<Bandwidth>(ErrorCode::OutOfRange, "bandwidth value exceeds the supported maximum");
  }
  return Bandwidth::from_bits_per_second(value * factor);
}

}  // namespace

Result<Bandwidth> Bandwidth::from_bits_per_second(rep bits_per_second) {
  if (bits_per_second < 0) {
    return make_error<Bandwidth>(ErrorCode::OutOfRange, "bandwidth must not be negative");
  }
  if (bits_per_second > kMaxBitsPerSecond) {
    return make_error<Bandwidth>(ErrorCode::OutOfRange, "bandwidth exceeds the supported maximum of 1 Pbit/s");
  }
  return Bandwidth(bits_per_second);
}

Result<Bandwidth> Bandwidth::from_kilobits_per_second(rep kbps) { return checked_scale(kbps, 1'000LL); }
Result<Bandwidth> Bandwidth::from_megabits_per_second(rep mbps) { return checked_scale(mbps, 1'000'000LL); }
Result<Bandwidth> Bandwidth::from_gigabits_per_second(rep gbps) { return checked_scale(gbps, 1'000'000'000LL); }

Result<Bandwidth> Bandwidth::checked_add(Bandwidth other) const noexcept {
  if (other.bits_per_second_ > kMaxBitsPerSecond - bits_per_second_) {
    return make_error<Bandwidth>(ErrorCode::NumericOverflow, "bandwidth addition overflowed");
  }
  return Bandwidth(bits_per_second_ + other.bits_per_second_);
}

Result<Bandwidth> Bandwidth::checked_sub(Bandwidth other) const noexcept {
  if (other.bits_per_second_ > bits_per_second_) {
    return make_error<Bandwidth>(ErrorCode::NumericOverflow, "bandwidth subtraction would go negative");
  }
  return Bandwidth(bits_per_second_ - other.bits_per_second_);
}

Result<Bandwidth> Bandwidth::checked_mul(std::uint64_t factor) const noexcept {
  bool overflow = false;
  const std::uint64_t product = mul_div_floor_u64(static_cast<std::uint64_t>(bits_per_second_), factor, 1, overflow);
  if (overflow || product > static_cast<std::uint64_t>(kMaxBitsPerSecond)) {
    return make_error<Bandwidth>(ErrorCode::NumericOverflow, "bandwidth multiplication overflowed");
  }
  return Bandwidth(static_cast<rep>(product));
}

Result<Bandwidth> Bandwidth::scaled(std::uint64_t numerator, std::uint64_t denominator) const noexcept {
  if (denominator == 0) {
    return make_error<Bandwidth>(ErrorCode::InvalidArgument, "bandwidth scale denominator must not be zero");
  }
  bool overflow = false;
  const std::uint64_t quotient =
      mul_div_floor_u64(static_cast<std::uint64_t>(bits_per_second_), numerator, denominator, overflow);
  if (overflow || quotient > static_cast<std::uint64_t>(kMaxBitsPerSecond)) {
    return make_error<Bandwidth>(ErrorCode::NumericOverflow, "bandwidth scaling overflowed");
  }
  return Bandwidth(static_cast<rep>(quotient));
}

Result<Bandwidth> Bandwidth::sum(const Bandwidth* values, std::size_t count) noexcept {
  Bandwidth total = zero();
  for (std::size_t i = 0; i < count; ++i) {
    const auto next = total.checked_add(values[i]);
    if (!next.ok()) {
      return next.error();
    }
    total = next.value();
  }
  return total;
}

Result<Bandwidth> Bandwidth::sum(const std::vector<Bandwidth>& values) noexcept {
  return sum(values.data(), values.size());
}

std::string Bandwidth::to_string() const {
  char buffer[64] = {};
  const int written = std::snprintf(buffer, sizeof(buffer), "%lld bps", static_cast<long long>(bits_per_second_));
  if (written <= 0) {
    return "0 bps";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<Duration> Duration::from_nanoseconds(rep ns) noexcept {
  if (ns < 0) {
    return make_error<Duration>(ErrorCode::OutOfRange, "duration must not be negative");
  }
  if (ns > kMaxNanoseconds) {
    return make_error<Duration>(ErrorCode::OutOfRange, "duration exceeds the supported maximum");
  }
  return Duration(ns);
}

Result<Duration> Duration::from_milliseconds(rep ms) noexcept {
  if (ms < 0 || ms > kMaxNanoseconds / 1'000'000LL) {
    return make_error<Duration>(ErrorCode::OutOfRange, "duration in milliseconds is out of range");
  }
  return Duration(ms * 1'000'000LL);
}

Result<Duration> Duration::from_seconds(rep s) noexcept {
  if (s < 0 || s > kMaxNanoseconds / 1'000'000'000LL) {
    return make_error<Duration>(ErrorCode::OutOfRange, "duration in seconds is out of range");
  }
  return Duration(s * 1'000'000'000LL);
}

Result<Timestamp> Timestamp::from_unix_nanoseconds(rep ns) noexcept {
  if (ns < 0) {
    return make_error<Timestamp>(ErrorCode::OutOfRange, "timestamp must not be negative");
  }
  return Timestamp(ns);
}

}  // namespace bandwidth_broker
