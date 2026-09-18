// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/checksum.hpp"

namespace bandwidth_broker {
namespace {

struct Crc32cTable final {
  std::uint32_t entries[256];

  constexpr Crc32cTable() noexcept : entries{} {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = ((crc & 1u) != 0u) ? ((crc >> 1) ^ Crc32c::kPolynomial) : (crc >> 1);
      }
      entries[i] = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

}  // namespace

void Crc32c::update(const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t crc = value_;
  for (std::size_t i = 0; i < length; ++i) {
    crc = kCrc32cTable.entries[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
  }
  value_ = crc;
}

std::uint32_t Crc32c::compute(const void* data, std::size_t length) noexcept {
  Crc32c crc;
  crc.update(data, length);
  return crc.value();
}

std::uint64_t fnv1a64_continue(std::uint64_t state, const void* data, std::size_t length) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t hash = state;
  for (std::size_t i = 0; i < length; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint64_t fnv1a64_continue(std::uint64_t state, std::string_view data) noexcept {
  return fnv1a64_continue(state, data.data(), data.size());
}

std::uint64_t fnv1a64(const void* data, std::size_t length) noexcept {
  return fnv1a64_continue(kFnvOffsetBasis, data, length);
}

std::uint64_t fnv1a64(std::string_view data) noexcept { return fnv1a64(data.data(), data.size()); }

void ContentHasher::add_raw(const void* data, std::size_t length) noexcept {
  state_ = fnv1a64_continue(state_, data, length);
}

void ContentHasher::add_u8(std::uint8_t value) noexcept {
  const unsigned char byte = value;
  add_raw(&byte, 1);
}

void ContentHasher::add_u32(std::uint32_t value) noexcept {
  unsigned char bytes[4] = {};
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
  }
  add_raw(bytes, sizeof(bytes));
}

void ContentHasher::add_u64(std::uint64_t value) noexcept {
  unsigned char bytes[8] = {};
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
  }
  add_raw(bytes, sizeof(bytes));
}

void ContentHasher::add_text(std::string_view text) noexcept {
  add_u64(text.size());
  add_raw(text.data(), text.size());
}

}  // namespace bandwidth_broker
