// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/identity.hpp"

#include <chrono>
#include <cstring>
#include <random>

#include "bandwidth_broker/checksum.hpp"

namespace bandwidth_broker {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return (c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (c - 'A') + 10;
  return -1;
}

}  // namespace

Result<std::uint64_t> parse_u64_decimal(std::string_view text) noexcept {
  if (text.empty()) {
    return make_error<std::uint64_t>(ErrorCode::InvalidIdentity, "identity text is empty");
  }
  if (text.size() > 20) {
    return make_error<std::uint64_t>(ErrorCode::OutOfRange, "identity text is too long");
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return make_error<std::uint64_t>(ErrorCode::InvalidIdentity, "identity text contains a non-digit character");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10ULL) {
      return make_error<std::uint64_t>(ErrorCode::OutOfRange, "identity text overflows 64 bits");
    }
    value = value * 10ULL + digit;
  }
  return value;
}

Result<std::uint64_t> parse_u64_hex(std::string_view text) noexcept {
  if (text.empty() || text.size() > 16) {
    return make_error<std::uint64_t>(ErrorCode::InvalidIdentity, "hex identity text has an invalid length");
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    const int digit = hex_value(c);
    if (digit < 0) {
      return make_error<std::uint64_t>(ErrorCode::InvalidIdentity, "hex identity text contains a non-hex character");
    }
    value = (value << 4) | static_cast<std::uint64_t>(digit);
  }
  return value;
}

Result<BootId> BootId::from_bytes(const std::array<std::uint8_t, kSize>& bytes) noexcept {
  bool all_zero = true;
  for (const std::uint8_t b : bytes) {
    if (b != 0) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) {
    return make_error<BootId>(ErrorCode::InvalidIdentity, "boot identity must not be all zero");
  }
  BootId id;
  id.bytes_ = bytes;
  return id;
}

Result<BootId> BootId::parse(std::string_view hex) noexcept {
  if (hex.size() != kSize * 2) {
    return make_error<BootId>(ErrorCode::InvalidIdentity, "boot identity must be exactly 32 hex characters");
  }
  std::array<std::uint8_t, kSize> bytes{};
  for (std::size_t i = 0; i < kSize; ++i) {
    const int high = hex_value(hex[i * 2]);
    const int low = hex_value(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return make_error<BootId>(ErrorCode::InvalidIdentity, "boot identity contains a non-hex character");
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return from_bytes(bytes);
}

bool BootId::valid() const noexcept {
  for (const std::uint8_t b : bytes_) {
    if (b != 0) {
      return true;
    }
  }
  return false;
}

std::string BootId::to_string() const {
  std::string out;
  out.resize(kSize * 2);
  for (std::size_t i = 0; i < kSize; ++i) {
    out[i * 2] = kHexDigits[(bytes_[i] >> 4) & 0x0Fu];
    out[i * 2 + 1] = kHexDigits[bytes_[i] & 0x0Fu];
  }
  return out;
}

std::uint64_t BootId::hash() const noexcept { return fnv1a64(bytes_.data(), bytes_.size()); }

EntropySource::~EntropySource() = default;

void SystemEntropySource::fill(unsigned char* buffer, std::size_t length) {
  std::random_device device;
  std::size_t written = 0;
  while (written < length) {
    const unsigned int chunk = device();
    const std::size_t take = (length - written) < sizeof(chunk) ? (length - written) : sizeof(chunk);
    std::memcpy(buffer + written, &chunk, take);
    written += take;
  }
}

BootId generate_boot_id(EntropySource& source) {
  std::array<std::uint8_t, BootId::kSize> bytes{};
  source.fill(bytes.data(), bytes.size());
  // A boot identity of all zeros is reserved for "invalid"; force a marker byte
  // so that an unlucky entropy draw can never produce an unrepresentable id.
  bytes[0] = static_cast<std::uint8_t>(bytes[0] | 0x01u);
  return BootId::from_bytes(bytes).value();
}

BootId boot_id_from_u64(std::uint64_t hi, std::uint64_t lo) noexcept {
  std::array<std::uint8_t, BootId::kSize> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>((hi >> (56 - (i * 8))) & 0xFFu);
    bytes[8 + i] = static_cast<std::uint8_t>((lo >> (56 - (i * 8))) & 0xFFu);
  }
  bytes[0] = static_cast<std::uint8_t>(bytes[0] | 0x01u);
  return BootId::from_bytes(bytes).value();
}

Result<FabricEpoch> next_fabric_epoch(FabricEpoch current) noexcept {
  if (!current.valid()) {
    return make_error<FabricEpoch>(ErrorCode::InvalidIdentity, "fabric epoch is not set");
  }
  if (current.value() == UINT64_MAX) {
    return make_error<FabricEpoch>(ErrorCode::OutOfRange, "fabric epoch space exhausted");
  }
  return FabricEpoch::from_value(current.value() + 1);
}

const char* to_string(NodeKind kind) noexcept {
  switch (kind) {
    case NodeKind::Unknown: return "unknown";
    case NodeKind::Operator: return "operator";
    case NodeKind::CapacityPublisher: return "capacity_publisher";
    case NodeKind::Requester: return "requester";
    case NodeKind::Coordinator: return "coordinator";
    case NodeKind::Recovery: return "recovery";
  }
  return "unknown";
}

std::string Provenance::describe() const {
  std::string out;
  out.reserve(96);
  out += to_string(source_kind);
  out += "#";
  out += std::to_string(source_id);
  out += "@epoch=";
  out += epoch.to_string();
  out += ",coord=";
  out += coordinator.to_string();
  out += ",seq=";
  out += std::to_string(source_sequence);
  if (!detail.empty()) {
    out += ",detail=";
    out += detail;
  }
  return out;
}

namespace {

void append_mismatch(std::string& out, std::string_view field) {
  if (!out.empty()) {
    out += ",";
  }
  out += field;
}

}  // namespace

std::string AuthorityVector::describe_mismatch(const AuthorityVector& other) const {
  std::string out;
  out.reserve(128);
  if (fabric_epoch != other.fabric_epoch) append_mismatch(out, "fabric_epoch");
  if (coordinator != other.coordinator) append_mismatch(out, "coordinator");
  if (resource != other.resource) append_mismatch(out, "resource");
  if (resource_generation != other.resource_generation) append_mismatch(out, "resource_generation");
  if (capacity_generation != other.capacity_generation) append_mismatch(out, "capacity_generation");
  if (policy != other.policy) append_mismatch(out, "policy");
  if (policy_generation != other.policy_generation) append_mismatch(out, "policy_generation");
  if (request != other.request) append_mismatch(out, "request");
  if (request_generation != other.request_generation) append_mismatch(out, "request_generation");
  if (reservation != other.reservation) append_mismatch(out, "reservation");
  if (reservation_generation != other.reservation_generation) append_mismatch(out, "reservation_generation");
  if (fairness_group != other.fairness_group) append_mismatch(out, "fairness_group");
  if (fairness_config_generation != other.fairness_config_generation) append_mismatch(out, "fairness_config_generation");
  if (tenant_config_generation != other.tenant_config_generation) append_mismatch(out, "tenant_config_generation");
  if (publisher != other.publisher) append_mismatch(out, "publisher");
  if (publisher_boot != other.publisher_boot) append_mismatch(out, "publisher_boot");
  if (publisher_sequence != other.publisher_sequence) append_mismatch(out, "publisher_sequence");
  if (monotonic_tick != other.monotonic_tick) append_mismatch(out, "monotonic_tick");
  if (out.empty()) {
    out = "none";
  }
  return out;
}

bool AuthorityVector::same_binding(const AuthorityVector& other) const noexcept {
  return fabric_epoch == other.fabric_epoch && coordinator == other.coordinator && resource == other.resource &&
         resource_generation == other.resource_generation && capacity_generation == other.capacity_generation &&
         policy == other.policy && policy_generation == other.policy_generation && request == other.request &&
         request_generation == other.request_generation && reservation == other.reservation &&
         reservation_generation == other.reservation_generation && fairness_group == other.fairness_group &&
         fairness_config_generation == other.fairness_config_generation &&
         tenant_config_generation == other.tenant_config_generation && publisher == other.publisher &&
         publisher_boot == other.publisher_boot;
}

bool AuthorityVector::operator==(const AuthorityVector& other) const noexcept {
  return fabric_epoch == other.fabric_epoch && coordinator == other.coordinator && resource == other.resource &&
         resource_generation == other.resource_generation && capacity_generation == other.capacity_generation &&
         policy == other.policy && policy_generation == other.policy_generation && request == other.request &&
         request_generation == other.request_generation && reservation == other.reservation &&
         reservation_generation == other.reservation_generation && fairness_group == other.fairness_group &&
         fairness_config_generation == other.fairness_config_generation &&
         tenant_config_generation == other.tenant_config_generation && publisher == other.publisher &&
         publisher_boot == other.publisher_boot && publisher_sequence == other.publisher_sequence &&
         monotonic_tick == other.monotonic_tick;
}

}  // namespace bandwidth_broker
