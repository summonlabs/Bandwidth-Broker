// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Strongly typed identities, generations, boot identities and the authority
// vector. Every authority-bearing record in this runtime is bound to the exact
// identities and generations listed in its AuthorityVector.

#ifndef BANDWIDTH_BROKER_IDENTITY_HPP
#define BANDWIDTH_BROKER_IDENTITY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/export.hpp"
#include "bandwidth_broker/quantity.hpp"

namespace bandwidth_broker {

// ---------------------------------------------------------------------------
// Primitive parsing helpers (strict; reject empty, sign, overflow, trailing
// characters, and non-canonical forms).
// ---------------------------------------------------------------------------

[[nodiscard]] BB_API Result<std::uint64_t> parse_u64_decimal(std::string_view text) noexcept;
[[nodiscard]] BB_API Result<std::uint64_t> parse_u64_hex(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Id<Tag>: opaque 64-bit identity. Value 0 is reserved for "invalid / absent"
// and can never be produced by make(), parse() or any allocation path.
// ---------------------------------------------------------------------------

template <typename Tag>
class Id final {
 public:
  using rep = std::uint64_t;
  using tag_type = Tag;

  constexpr Id() noexcept = default;

  [[nodiscard]] static constexpr Id from_value(rep value) noexcept { return Id(value); }

  [[nodiscard]] static Result<Id> make(rep value) noexcept {
    if (value == 0) {
      return make_error<Id>(ErrorCode::InvalidIdentity, "identity value must be non-zero");
    }
    return Id(value);
  }

  [[nodiscard]] static Result<Id> parse(std::string_view text) noexcept {
    const auto parsed = parse_u64_decimal(text);
    if (!parsed.ok()) {
      return parsed.error();
    }
    return make(parsed.value());
  }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  [[nodiscard]] friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  [[nodiscard]] friend constexpr bool operator!=(Id a, Id b) noexcept { return a.value_ != b.value_; }
  [[nodiscard]] friend constexpr bool operator<(Id a, Id b) noexcept { return a.value_ < b.value_; }
  [[nodiscard]] friend constexpr bool operator>(Id a, Id b) noexcept { return a.value_ > b.value_; }
  [[nodiscard]] friend constexpr bool operator<=(Id a, Id b) noexcept { return a.value_ <= b.value_; }
  [[nodiscard]] friend constexpr bool operator>=(Id a, Id b) noexcept { return a.value_ >= b.value_; }

 private:
  explicit constexpr Id(rep value) noexcept : value_(value) {}
  rep value_{0};
};

// ---------------------------------------------------------------------------
// Generation<Tag>: strictly monotonic revision counter. Generation 0 means
// "never established" and is never a valid authoritative generation.
// ---------------------------------------------------------------------------

template <typename Tag>
class Generation final {
 public:
  using rep = std::uint64_t;
  using tag_type = Tag;

  constexpr Generation() noexcept = default;

  [[nodiscard]] static constexpr Generation from_value(rep value) noexcept { return Generation(value); }

  [[nodiscard]] static Result<Generation> make(rep value) noexcept {
    if (value == 0) {
      return make_error<Generation>(ErrorCode::InvalidIdentity, "generation value must be non-zero");
    }
    return Generation(value);
  }

  [[nodiscard]] static constexpr Generation initial() noexcept { return Generation(1); }

  // Advances by one. Fails instead of wrapping; a wrapped generation would
  // silently re-authorise superseded authority.
  [[nodiscard]] Result<Generation> next() const noexcept {
    if (value_ == 0) {
      return make_error<Generation>(ErrorCode::InvalidIdentity, "cannot advance an unset generation");
    }
    if (value_ == kMaxValue) {
      return make_error<Generation>(ErrorCode::OutOfRange, "generation space exhausted");
    }
    return Generation(value_ + 1);
  }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  [[nodiscard]] friend constexpr bool operator==(Generation a, Generation b) noexcept { return a.value_ == b.value_; }
  [[nodiscard]] friend constexpr bool operator!=(Generation a, Generation b) noexcept { return a.value_ != b.value_; }
  [[nodiscard]] friend constexpr bool operator<(Generation a, Generation b) noexcept { return a.value_ < b.value_; }
  [[nodiscard]] friend constexpr bool operator>(Generation a, Generation b) noexcept { return a.value_ > b.value_; }
  [[nodiscard]] friend constexpr bool operator<=(Generation a, Generation b) noexcept { return a.value_ <= b.value_; }
  [[nodiscard]] friend constexpr bool operator>=(Generation a, Generation b) noexcept { return a.value_ >= b.value_; }

  static constexpr rep kMaxValue = UINT64_MAX;

 private:
  explicit constexpr Generation(rep value) noexcept : value_(value) {}
  rep value_{0};
};

// ---------------------------------------------------------------------------
// BootId: 16-byte identity of a single OS process incarnation. A boot identity
// is fenced permanently once its process is declared dead; a fresh incarnation
// always carries a distinct BootId.
// ---------------------------------------------------------------------------

class BB_API BootId final {
 public:
  static constexpr std::size_t kSize = 16;

  BootId() noexcept = default;

  [[nodiscard]] static Result<BootId> from_bytes(const std::array<std::uint8_t, kSize>& bytes) noexcept;
  [[nodiscard]] static Result<BootId> parse(std::string_view hex) noexcept;

  [[nodiscard]] const std::array<std::uint8_t, kSize>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] std::uint64_t hash() const noexcept;

  [[nodiscard]] friend bool operator==(const BootId& a, const BootId& b) noexcept { return a.bytes_ == b.bytes_; }
  [[nodiscard]] friend bool operator!=(const BootId& a, const BootId& b) noexcept { return a.bytes_ != b.bytes_; }
  [[nodiscard]] friend bool operator<(const BootId& a, const BootId& b) noexcept { return a.bytes_ < b.bytes_; }

 private:
  std::array<std::uint8_t, kSize> bytes_{};
};

class BB_API EntropySource {
 public:
  EntropySource() = default;
  virtual ~EntropySource();
  EntropySource(const EntropySource&) = delete;
  EntropySource& operator=(const EntropySource&) = delete;
  virtual void fill(unsigned char* buffer, std::size_t length) = 0;
};

class BB_API SystemEntropySource final : public EntropySource {
 public:
  void fill(unsigned char* buffer, std::size_t length) override;
};

[[nodiscard]] BB_API BootId generate_boot_id(EntropySource& source);

// Boot identities are opaque fencing tokens. They are not credentials: the
// wire protocol additionally requires a shared session token.
[[nodiscard]] BB_API BootId boot_id_from_u64(std::uint64_t hi, std::uint64_t lo) noexcept;

// ---------------------------------------------------------------------------
// Identity tags and aliases.
// ---------------------------------------------------------------------------

struct BandwidthResourceTag;
struct BandwidthPoolTag;
struct BandwidthRequestTag;
struct BandwidthGrantTag;
struct TenantTag;
struct FairnessGroupTag;
struct PriorityClassTag;
struct PolicyTag;
struct ReservationReferenceTag;
struct FabricEpochTag;
struct CoordinatorIncarnationTag;
struct PublisherTag;
struct WorkerTag;
struct AttemptTag;
struct SessionTag;
struct DecisionTag;
struct RecallTag;
struct RevocationTag;
struct AuditSequenceTag;
struct CapacitySnapshotTag;
struct SloClassTag;

struct BandwidthResourceGenerationTag;
struct BandwidthPoolGenerationTag;
struct BandwidthRequestGenerationTag;
struct BandwidthGrantGenerationTag;
struct PolicyGenerationTag;
struct ReservationGenerationTag;
struct CapacitySnapshotGenerationTag;
struct FairnessConfigGenerationTag;
struct TenantConfigGenerationTag;

using BandwidthResourceId = Id<BandwidthResourceTag>;
using BandwidthPoolId = Id<BandwidthPoolTag>;
using BandwidthRequestId = Id<BandwidthRequestTag>;
using BandwidthGrantId = Id<BandwidthGrantTag>;
using TenantId = Id<TenantTag>;
using FairnessGroupId = Id<FairnessGroupTag>;
using PriorityClassId = Id<PriorityClassTag>;
using PolicyId = Id<PolicyTag>;
using ReservationReferenceId = Id<ReservationReferenceTag>;
using FabricEpoch = Id<FabricEpochTag>;
using CoordinatorIncarnation = Id<CoordinatorIncarnationTag>;
using PublisherId = Id<PublisherTag>;
using WorkerId = Id<WorkerTag>;
using AttemptId = Id<AttemptTag>;
using SessionId = Id<SessionTag>;
using DecisionId = Id<DecisionTag>;
using RecallId = Id<RecallTag>;
using RevocationId = Id<RevocationTag>;
using AuditSequence = Id<AuditSequenceTag>;
using CapacitySnapshotId = Id<CapacitySnapshotTag>;
using SloClassId = Id<SloClassTag>;

using BandwidthResourceGeneration = Generation<BandwidthResourceGenerationTag>;
using BandwidthPoolGeneration = Generation<BandwidthPoolGenerationTag>;
using BandwidthRequestGeneration = Generation<BandwidthRequestGenerationTag>;
using BandwidthGrantGeneration = Generation<BandwidthGrantGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using ReservationGeneration = Generation<ReservationGenerationTag>;
using CapacitySnapshotGeneration = Generation<CapacitySnapshotGenerationTag>;
using FairnessConfigGeneration = Generation<FairnessConfigGenerationTag>;
using TenantConfigGeneration = Generation<TenantConfigGenerationTag>;

// The FabricEpoch is a generation of the fabric-wide authority scope. It is
// carried in an Id so that it can never be confused with a resource or policy
// generation; its numeric value increases by one on every coordinator restart.
inline constexpr std::uint64_t kFirstFabricEpoch = 1;

// Advances the fabric epoch. Fails instead of wrapping: a wrapped epoch would
// silently re-authorise authority bound to a superseded epoch.
[[nodiscard]] BB_API Result<FabricEpoch> next_fabric_epoch(FabricEpoch current) noexcept;

// ---------------------------------------------------------------------------
// Provenance: who produced a record and under which incarnation.
// ---------------------------------------------------------------------------

enum class NodeKind : std::uint8_t {
  Unknown = 0,
  Operator = 1,
  CapacityPublisher = 2,
  Requester = 3,
  Coordinator = 4,
  Recovery = 5
};

[[nodiscard]] BB_API const char* to_string(NodeKind kind) noexcept;

inline constexpr std::size_t kMaxProvenanceDetailBytes = 96;

struct BB_API Provenance final {
  NodeKind source_kind{NodeKind::Unknown};
  std::uint64_t source_id{0};
  BootId source_boot{};
  FabricEpoch epoch{};
  CoordinatorIncarnation coordinator{};
  std::uint64_t source_sequence{0};
  Timestamp recorded_at{};
  std::string detail;

  [[nodiscard]] bool valid() const noexcept {
    return source_kind != NodeKind::Unknown && epoch.valid() && source_boot.valid();
  }
  [[nodiscard]] std::string describe() const;
};

// ---------------------------------------------------------------------------
// AuthorityVector: the complete set of bindings that justified one decision.
// Two records with different authority vectors never authorise each other.
// ---------------------------------------------------------------------------

struct BB_API AuthorityVector final {
  FabricEpoch fabric_epoch{};
  CoordinatorIncarnation coordinator{};
  BandwidthResourceId resource{};
  BandwidthResourceGeneration resource_generation{};
  CapacitySnapshotGeneration capacity_generation{};
  PolicyId policy{};
  PolicyGeneration policy_generation{};
  BandwidthRequestId request{};
  BandwidthRequestGeneration request_generation{};
  ReservationReferenceId reservation{};
  ReservationGeneration reservation_generation{};
  FairnessGroupId fairness_group{};
  FairnessConfigGeneration fairness_config_generation{};
  TenantConfigGeneration tenant_config_generation{};
  PublisherId publisher{};
  BootId publisher_boot{};
  std::uint64_t publisher_sequence{0};
  std::uint64_t monotonic_tick{0};

  // Returns a bounded, deterministic list of the fields that differ.
  [[nodiscard]] std::string describe_mismatch(const AuthorityVector& other) const;

  // Authority binding comparison.
  //
  // Compares every field that binds authority and deliberately ignores the two
  // fields that advance for reasons unrelated to authority: monotonic_tick
  // (which advances every committed round) and publisher_sequence (which
  // advances on every submission by the same client). A grant may therefore be
  // renewed across rounds without a generation bump while it stays bound to the
  // same policy, capacity, resource, reservation and requester incarnation.
  [[nodiscard]] bool same_binding(const AuthorityVector& other) const noexcept;
  [[nodiscard]] bool operator==(const AuthorityVector& other) const noexcept;
  [[nodiscard]] bool operator!=(const AuthorityVector& other) const noexcept { return !(*this == other); }
};

}  // namespace bandwidth_broker

namespace std {

template <typename Tag>
struct hash<bandwidth_broker::Id<Tag>> {
  [[nodiscard]] std::size_t operator()(const bandwidth_broker::Id<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

template <typename Tag>
struct hash<bandwidth_broker::Generation<Tag>> {
  [[nodiscard]] std::size_t operator()(const bandwidth_broker::Generation<Tag>& generation) const noexcept {
    return std::hash<std::uint64_t>{}(generation.value());
  }
};

template <>
struct hash<bandwidth_broker::BootId> {
  [[nodiscard]] std::size_t operator()(const bandwidth_broker::BootId& boot) const noexcept { return static_cast<std::size_t>(boot.hash()); }
};

}  // namespace std

#endif  // BANDWIDTH_BROKER_IDENTITY_HPP
