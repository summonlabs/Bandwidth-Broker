// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durable state.
//
// Layout of a store directory:
//   MANIFEST              small binary header, rewritten atomically (temp + rename)
//   snapshot-<seq>.bbs    full state image, versioned, length-checked, CRC-checked
//   journal-<seq>.bbj     append-only record log, per-record magic/length/CRC
//
// Durability contract: a mutation is acknowledged only after its journal record
// has been written, flushed and (when configured) synced. Recovery replays the
// newest intact snapshot and then every intact journal record after it, and
// stops at the first damaged record - a torn tail is truncated, never guessed.
//
// A store never restores process liveness. It restores facts: configuration,
// policy, obligations, fences, history and the durable statement that a grant
// exists but requires revalidation.

#ifndef BANDWIDTH_BROKER_PERSISTENCE_HPP
#define BANDWIDTH_BROKER_PERSISTENCE_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bandwidth_broker/error.hpp"
#include "bandwidth_broker/identity.hpp"
#include "bandwidth_broker/limits.hpp"

namespace bandwidth_broker {

enum class RecordType : std::uint16_t {
  PolicySet = 1,
  CapacityPublished = 2,
  ObligationUpserted = 3,
  ObligationRetired = 4,
  RequestAccepted = 5,
  RequestRetired = 6,
  RoundCommitted = 7,
  GrantIssued = 8,
  GrantUpdated = 9,
  GrantReleased = 10,
  GrantRevoked = 11,
  GrantRevalidated = 12,
  PublisherFenced = 13,
  EpochAdvanced = 14,
  IdempotencyRecord = 15,
  CoordinatorBoot = 16
};

[[nodiscard]] BB_API const char* to_string(RecordType type) noexcept;
[[nodiscard]] BB_API bool is_known_record_type(std::uint16_t raw) noexcept;

// Every durable record carries the authority it was written under. Replay can
// therefore distinguish "written by this incarnation" from "written by a
// predecessor", which is what makes conservative recovery possible.
struct BB_API RecordHeader final {
  AuditSequence sequence{};
  std::uint64_t tick{0};
  FabricEpoch epoch{};
  CoordinatorIncarnation coordinator{};
};

struct BB_API Record final {
  RecordType type{RecordType::CoordinatorBoot};
  RecordHeader header{};
  std::vector<std::uint8_t> payload{};
};

struct BB_API SnapshotImage final {
  RecordHeader header{};
  std::uint64_t sequence{0};  // journal records up to and including this sequence
  std::vector<std::uint8_t> payload{};
};

struct BB_API StoreOptions final {
  bool fsync_on_commit{true};
  std::size_t compaction_records{50'000};
  std::size_t max_journal_records{limits::kMaxJournalRecords};
};

struct BB_API RecoveryReport final {
  bool durable{false};
  bool fresh{false};
  bool manifest_present{false};
  bool snapshot_loaded{false};
  bool snapshot_corrupt{false};
  bool journal_truncated{false};
  bool version_supported{true};
  std::uint64_t records_replayed{0};
  std::uint64_t records_discarded{0};
  std::uint64_t snapshot_sequence{0};
  std::uint64_t journal_records{0};
  std::uint64_t snapshots_written{0};
  std::uint64_t truncations{0};
  AuditSequence last_sequence{};
  std::string detail;
};

struct BB_API RecoveryResult final {
  RecoveryReport report{};
  std::vector<Record> records{};
  bool has_snapshot{false};
  SnapshotImage snapshot{};
};

class BB_API Store final {
 public:
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  // Opens the store directory, creating it when absent. Never mutates durable
  // state other than creating an empty layout.
  [[nodiscard]] static Result<std::unique_ptr<Store>> open(const std::string& directory, const StoreOptions& options);

  // Reads the whole durable state. Damaged snapshots fall back to the previous
  // snapshot; damaged journal tails are truncated and reported.
  [[nodiscard]] Result<RecoveryResult> recover();

  // Appends one record. The record is durable on return when fsync_on_commit is
  // set. Records are rejected before any byte is written when the journal is
  // full or the store is read-only.
  [[nodiscard]] Status append(RecordType type, const RecordHeader& header, const std::vector<std::uint8_t>& payload);

  [[nodiscard]] Status flush();

  [[nodiscard]] bool needs_compaction() const;
  [[nodiscard]] std::size_t journal_record_count() const;

  // Writes a new snapshot and retires the journals it covers. Callers must hold
  // whatever lock makes the supplied image consistent.
  [[nodiscard]] Status compact(const SnapshotImage& image);

  [[nodiscard]] RecoveryReport report() const;
  [[nodiscard]] std::string directory() const;

  // File-level integrity check used by the inspection tool: returns a report
  // describing what recovery would do, without replaying into a runtime.
  [[nodiscard]] Result<RecoveryReport> inspect() const;

  // Implementation state. Public only so that file-format helpers in the
  // translation unit can be free functions; it is never part of the API.
  struct Impl;

 private:
  Store();
  std::unique_ptr<Impl> impl_;
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_PERSISTENCE_HPP
