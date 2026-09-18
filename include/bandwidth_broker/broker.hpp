// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The coordinator runtime.
//
// The broker owns authoritative grant state, generations, idempotency, fences,
// recall history and durable configuration. It is the only component that
// mutates authority, and it does so under a single lock with a strict
// validate -> bind authority -> plan -> reserve -> journal -> apply -> commit
// sequence. Callbacks and events are delivered only after the lock is released.
//
// The broker produces allocation state. It never enforces it: the Rate Governor,
// Flow Scheduler and Traffic Engineering Fabric apply what the broker grants.

#ifndef BANDWIDTH_BROKER_BROKER_HPP
#define BANDWIDTH_BROKER_BROKER_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "bandwidth_broker/accounting.hpp"
#include "bandwidth_broker/arbitrator.hpp"
#include "bandwidth_broker/explanation.hpp"
#include "bandwidth_broker/persistence.hpp"

namespace bandwidth_broker {

struct BB_API BrokerConfig final {
  FabricEpoch epoch{FabricEpoch::from_value(1)};
  CoordinatorIncarnation incarnation{};
  // Empty means an in-memory broker with no durable state.
  std::string store_directory{};
  std::size_t max_idempotency_entries{limits::kMaxIdempotencyEntries};
  std::size_t max_audit_records{limits::kMaxAuditRecordsInMemory};
  std::size_t journal_compaction_records{50'000};
  bool fsync_on_commit{true};
};

enum class IdempotencyDisposition : std::uint8_t {
  New = 0,        // first time this identity and content were seen
  Replay = 1,     // identical identity and content; the stored outcome is returned
  Conflict = 2    // same identity, different content: refused
};

struct BB_API SubmitOutcome final {
  IdempotencyDisposition disposition{IdempotencyDisposition::New};
  BandwidthRequestId request{};
  BandwidthRequestGeneration generation{};
  bool waiting{false};
  bool refused{false};
  OutcomeReason reason{OutcomeReason::None};
};

struct BB_API RoundSummary final {
  DecisionId decision{};
  CapacityTarget target{};
  std::uint64_t tick{0};
  std::size_t granted{0};
  std::size_t waiting{0};
  std::size_t refused{0};
  std::size_t recalls{0};
  std::uint64_t next_grant_id{1};
  ResourceAccounting accounting{};
};

struct BB_API GrantView final {
  Grant grant{};
  bool found{false};
  bool authoritative{false};
  bool generation_current{false};
  std::string note;
};

struct BB_API ReleaseOutcome final {
  bool already_released{false};
  GrantState state{GrantState::Released};
  Bandwidth released{};
  std::uint64_t tick{0};
};

struct BB_API CoordinatorStatus final {
  FabricEpoch epoch{};
  CoordinatorIncarnation incarnation{};
  std::uint64_t tick{0};
  std::uint64_t audit_sequence{0};
  std::size_t policies{0};
  std::size_t resources{0};
  std::size_t obligations{0};
  std::size_t requests{0};
  std::size_t live_grants{0};
  std::size_t revalidation_required{0};
  std::size_t fences{0};
  bool durable{false};
  std::uint64_t journal_records{0};
  std::uint64_t snapshots_written{0};
  std::uint64_t recoveries{0};
  std::uint64_t journal_truncations{0};
};

// A durable fence record. Fencing is permanent for the lifetime of the store.
struct BB_API BootFence final {
  PublisherId publisher{};
  BootId boot{};
  AuditSequence sequence{};
  std::uint64_t tick{0};
  std::string reason;
};

class BB_API Broker final {
 public:
  Broker(const Broker&) = delete;
  Broker& operator=(const Broker&) = delete;
  ~Broker();

  // Opens (and, when a store directory is configured, recovers) a coordinator.
  // Recovery never restores live authority: every grant that was live before a
  // restart is moved to RevalidationRequired and the fabric epoch is advanced.
  [[nodiscard]] static Result<std::unique_ptr<Broker>> open(const BrokerConfig& config);

  // ---- configuration -------------------------------------------------
  [[nodiscard]] Status set_policy(const Policy& policy);
  [[nodiscard]] Result<Policy> policy() const;

  // ---- capacity ------------------------------------------------------
  [[nodiscard]] Status publish_capacity(const CapacitySnapshot& snapshot);
  [[nodiscard]] Result<CapacitySnapshot> capacity(const CapacityTarget& target) const;

  // ---- obligations ---------------------------------------------------
  [[nodiscard]] Status upsert_obligation(const Obligation& obligation);
  [[nodiscard]] Status retire_obligation(ReservationReferenceId reservation, ReservationGeneration generation);
  [[nodiscard]] Result<std::vector<Obligation>> obligations(const CapacityTarget& target) const;

  // ---- requests ------------------------------------------------------
  [[nodiscard]] Result<SubmitOutcome> submit(const BandwidthRequest& request);
  [[nodiscard]] Status retire_request(BandwidthRequestId id, BandwidthRequestGeneration generation);

  // ---- arbitration ---------------------------------------------------
  [[nodiscard]] Result<RoundSummary> arbitrate(const CapacityTarget& target);

  // ---- grants --------------------------------------------------------
  [[nodiscard]] Result<GrantView> query_grant(BandwidthGrantId id, BandwidthGrantGeneration generation) const;
  [[nodiscard]] Result<ReleaseOutcome> release(BandwidthGrantId id, BandwidthGrantGeneration generation);
  [[nodiscard]] Result<GrantView> revoke(BandwidthGrantId id,
                                         BandwidthGrantGeneration generation,
                                         OutcomeReason reason,
                                         const std::string& detail);
  [[nodiscard]] Result<std::vector<Grant>> grants(const CapacityTarget& target, bool live_only) const;

  // Revalidation after a restart: the requester must present a fresh claim.
  [[nodiscard]] Result<GrantView> revalidate(BandwidthGrantId id, BandwidthGrantGeneration generation);

  // ---- authority -----------------------------------------------------
  [[nodiscard]] Status fence_publisher(PublisherId publisher, BootId boot, const std::string& reason);
  [[nodiscard]] Result<bool> is_fenced(PublisherId publisher, BootId boot) const;
  [[nodiscard]] Result<std::vector<BootFence>> fences() const;

  // ---- inspection ----------------------------------------------------
  [[nodiscard]] Result<RequestExplanation> explain(BandwidthRequestId id,
                                                   BandwidthRequestGeneration generation) const;
  [[nodiscard]] Result<ResourceAccounting> accounting(const CapacityTarget& target) const;
  [[nodiscard]] Result<std::vector<ResourceAccounting>> accounting_all() const;
  [[nodiscard]] CoordinatorStatus status() const;
  [[nodiscard]] Result<std::vector<CapacityTarget>> targets() const;

  // ---- lifecycle -----------------------------------------------------
  [[nodiscard]] Status flush();

  // Advances the fabric epoch by one. Every live grant becomes
  // RevalidationRequired: authority bound to a superseded epoch never survives.
  [[nodiscard]] Result<FabricEpoch> advance_epoch();

 private:
  Broker();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_BROKER_HPP
