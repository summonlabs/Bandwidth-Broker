// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Integrity checking primitives used by the wire protocol, the durable store
// and the idempotency table. Both algorithms are implemented in-tree so that
// recorded digests are identical on every platform and compiler.

#ifndef BANDWIDTH_BROKER_CHECKSUM_HPP
#define BANDWIDTH_BROKER_CHECKSUM_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "bandwidth_broker/export.hpp"

namespace bandwidth_broker {

// CRC-32C (Castagnoli), reflected, initial value 0xFFFFFFFF, final xor 0xFFFFFFFF.
class BB_API Crc32c final {
 public:
  static constexpr std::uint32_t kPolynomial = 0x82F63B78u;

  Crc32c() noexcept = default;

  void reset() noexcept { value_ = 0xFFFFFFFFu; }
  void update(const void* data, std::size_t length) noexcept;
  void update(std::string_view data) noexcept { update(data.data(), data.size()); }
  [[nodiscard]] std::uint32_t value() const noexcept { return value_ ^ 0xFFFFFFFFu; }

  [[nodiscard]] static std::uint32_t compute(const void* data, std::size_t length) noexcept;
  [[nodiscard]] static std::uint32_t compute(std::string_view data) noexcept { return compute(data.data(), data.size()); }

 private:
  std::uint32_t value_{0xFFFFFFFFu};
};

// FNV-1a 64-bit. Used for canonical content hashing (identity conflict
// detection), not as a cryptographic digest.
[[nodiscard]] BB_API std::uint64_t fnv1a64(const void* data, std::size_t length) noexcept;
[[nodiscard]] BB_API std::uint64_t fnv1a64(std::string_view data) noexcept;
[[nodiscard]] BB_API std::uint64_t fnv1a64_continue(std::uint64_t state, const void* data, std::size_t length) noexcept;
[[nodiscard]] BB_API std::uint64_t fnv1a64_continue(std::uint64_t state, std::string_view data) noexcept;

// Canonical, length-prefixed content hasher.
//
// Every field is length-prefixed so that ("ab","c") and ("a","bc") hash
// differently. The resulting value is stable across platforms, compiler
// versions and process restarts, which is what makes identity-conflict
// detection meaningful across a coordinator restart.
class BB_API ContentHasher final {
 public:
  ContentHasher() noexcept = default;

  void add_u8(std::uint8_t value) noexcept;
  void add_u32(std::uint32_t value) noexcept;
  void add_u64(std::uint64_t value) noexcept;
  void add_bool(bool value) noexcept { add_u8(value ? 1u : 0u); }
  void add_text(std::string_view text) noexcept;
  void add_raw(const void* data, std::size_t length) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_{14695981039346656037ULL};
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_CHECKSUM_HPP
