// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/broker.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "bandwidth_broker/text.hpp"
#include "bandwidth_broker/wire.hpp"

// Lock ordering (the only nesting allowed in this runtime):
//
//   Broker::Impl::mutex  (level 1, leaf for state)
//     -> Store::Impl::mutex (level 2, file handles only)
//
// The store never calls back into the broker, the broker never invokes a
// callback, listener or user code while holding any lock, and no read lock is
// ever upgraded. A broker operation therefore cannot deadlock against itself.

namespace bandwidth_broker {
namespace {

using RequestKey = std::pair<std::uint64_t, std::uint64_t>;

[[nodiscard]] RequestKey request_key(BandwidthRequestId id, BandwidthRequestGeneration generation) noexcept {
  return {id.value(), generation.value()};
}

struct RoundRecord final {
  DecisionId decision{};
  CapacityTarget target{};
  std::uint64_t tick{0};
  ResourceAccounting accounting{};
  std::uint64_t next_grant_id{1};
  std::uint64_t next_recall_id{1};
};

struct IdempotencyRecordPayload final {
  BandwidthRequestId request{};
  BandwidthRequestGeneration generation{};
  std::uint64_t content_hash{0};
  OutcomeReason reason{OutcomeReason::None};
  bool waiting{false};
  bool refused{false};
};

struct FenceRecord final {
  PublisherId publisher{};
  BootId boot{};
  AuditSequence sequence{};
  std::uint64_t tick{0};
  std::string reason;
};

struct BootRecord final {
  CoordinatorIncarnation coordinator{};
  FabricEpoch epoch{};
  std::uint64_t tick{0};
  AuditSequence sequence{};
};

// ---- record payload codecs -------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_reference(ReservationReferenceId id,
                                                         ReservationGeneration generation) {
  Writer writer;
  write_id(writer, id);
  write_generation(writer, generation);
  return writer.take();
}

[[nodiscard]] Result<std::pair<ReservationReferenceId, ReservationGeneration>> decode_reference(Reader& reader) {
  const auto id = read_id<ReservationReferenceTag>(reader);
  if (!id.ok()) {
    return id.error();
  }
  const auto generation = read_generation<ReservationGenerationTag>(reader);
  if (!generation.ok()) {
    return generation.error();
  }
  if (!reader.expect_end().ok()) {
    return make_error<std::pair<ReservationReferenceId, ReservationGeneration>>(ErrorCode::PersistenceCorrupt,
                                                                               "record carries trailing bytes");
  }
  return std::make_pair(id.value(), generation.value());
}

[[nodiscard]] std::vector<std::uint8_t> encode_request_key(BandwidthRequestId id,
                                                           BandwidthRequestGeneration generation) {
  Writer writer;
  write_id(writer, id);
  write_generation(writer, generation);
  return writer.take();
}

[[nodiscard]] Result<std::pair<BandwidthRequestId, BandwidthRequestGeneration>> decode_request_key(Reader& reader) {
  const auto id = read_id<BandwidthRequestTag>(reader);
  if (!id.ok()) {
    return id.error();
  }
  const auto generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!generation.ok()) {
    return generation.error();
  }
  return std::make_pair(id.value(), generation.value());
}

[[nodiscard]] std::vector<std::uint8_t> encode_round(const RoundRecord& value) {
  Writer writer;
  write_id(writer, value.decision);
  encode(writer, value.target);
  writer.u64(value.tick);
  encode(writer, value.accounting);
  writer.u64(value.next_grant_id);
  writer.u64(value.next_recall_id);
  return writer.take();
}

[[nodiscard]] Result<RoundRecord> decode_round(Reader& reader) {
  RoundRecord value;
  const auto decision = read_id<DecisionTag>(reader);
  if (!decision.ok()) return decision.error();
  value.decision = decision.value();
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  const auto tick = reader.u64();
  if (!tick.ok()) return tick.error();
  value.tick = tick.value();
  auto accounting = decode_accounting(reader);
  if (!accounting.ok()) return accounting.error();
  value.accounting = accounting.take();
  const auto next_grant = reader.u64();
  if (!next_grant.ok()) return next_grant.error();
  value.next_grant_id = next_grant.value();
  const auto next_recall = reader.u64();
  if (!next_recall.ok()) return next_recall.error();
  value.next_recall_id = next_recall.value();
  return value;
}

[[nodiscard]] std::vector<std::uint8_t> encode_idempotency(const IdempotencyRecordPayload& value) {
  Writer writer;
  write_id(writer, value.request);
  write_generation(writer, value.generation);
  writer.u64(value.content_hash);
  writer.u8(static_cast<std::uint8_t>(value.reason));
  writer.boolean(value.waiting);
  writer.boolean(value.refused);
  return writer.take();
}

[[nodiscard]] Result<IdempotencyRecordPayload> decode_idempotency(Reader& reader) {
  IdempotencyRecordPayload value;
  const auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  const auto generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  const auto hash = reader.u64();
  if (!hash.ok()) return hash.error();
  value.content_hash = hash.value();
  const auto reason = reader.u8();
  if (!reason.ok()) return reason.error();
  if (reason.value() > 46) {
    return make_error<IdempotencyRecordPayload>(ErrorCode::PersistenceCorrupt, "recorded outcome reason is unknown");
  }
  value.reason = static_cast<OutcomeReason>(reason.value());
  const auto waiting = reader.boolean();
  if (!waiting.ok()) return waiting.error();
  value.waiting = waiting.value();
  const auto refused = reader.boolean();
  if (!refused.ok()) return refused.error();
  value.refused = refused.value();
  return value;
}

[[nodiscard]] std::vector<std::uint8_t> encode_fence(const FenceRecord& value) {
  Writer writer;
  write_id(writer, value.publisher);
  encode(writer, value.boot);
  write_id(writer, value.sequence);
  writer.u64(value.tick);
  writer.text(value.reason);
  return writer.take();
}

[[nodiscard]] Result<FenceRecord> decode_fence(Reader& reader) {
  FenceRecord value;
  const auto publisher = read_id<PublisherTag>(reader);
  if (!publisher.ok()) return publisher.error();
  value.publisher = publisher.value();
  const auto boot = decode_boot_id(reader);
  if (!boot.ok()) return boot.error();
  value.boot = boot.value();
  const auto sequence = read_id<AuditSequenceTag>(reader);
  if (!sequence.ok()) return sequence.error();
  value.sequence = sequence.value();
  const auto tick = reader.u64();
  if (!tick.ok()) return tick.error();
  value.tick = tick.value();
  const auto reason = reader.bytes(128);
  if (!reason.ok()) return reason.error();
  value.reason.assign(reason.value());
  return value;
}

[[nodiscard]] std::vector<std::uint8_t> encode_boot(const BootRecord& value) {
  Writer writer;
  write_id(writer, value.coordinator);
  write_id(writer, value.epoch);
  writer.u64(value.tick);
  write_id(writer, value.sequence);
  return writer.take();
}

[[nodiscard]] Result<BootRecord> decode_boot(Reader& reader) {
  BootRecord value;
  const auto coordinator = read_id<CoordinatorIncarnationTag>(reader);
  if (!coordinator.ok()) return coordinator.error();
  value.coordinator = coordinator.value();
  const auto epoch = read_id<FabricEpochTag>(reader);
  if (!epoch.ok()) return epoch.error();
  value.epoch = epoch.value();
  const auto tick = reader.u64();
  if (!tick.ok()) return tick.error();
  value.tick = tick.value();
  const auto sequence = read_id<AuditSequenceTag>(reader);
  if (!sequence.ok()) return sequence.error();
  value.sequence = sequence.value();
  return value;
}

[[nodiscard]] Status corrupt(std::string detail) {
  return make_error_status(ErrorCode::PersistenceCorrupt, "durable record rejected during recovery: " + detail);
}

}  // namespace

struct Broker::Impl final {
  BrokerConfig config;
  mutable std::mutex mutex;
  std::unique_ptr<Store> store;

  FabricEpoch epoch{FabricEpoch::from_value(1)};
  CoordinatorIncarnation incarnation{};
  std::uint64_t tick{0};
  std::uint64_t audit_sequence{0};
  std::uint64_t next_grant_id{1};
  std::uint64_t next_recall_id{1};
  std::uint64_t next_decision_id{1};
  std::uint64_t next_snapshot_id{1};
  std::uint64_t next_session_id{1};

  std::optional<Policy> policy;
  std::map<CapacityTarget, CapacitySnapshot> capacities;
  std::map<std::uint64_t, Obligation> obligations;
  std::map<RequestKey, BandwidthRequest> requests;
  std::map<RequestKey, std::uint64_t> wait_rounds;
  std::map<RequestKey, std::uint64_t> content_hashes;
  std::map<RequestKey, SubmitOutcome> idempotency;
  std::map<std::uint64_t, Grant> grants;
  std::vector<BootFence> fences;
  std::set<std::uint64_t> fence_hashes;
  std::map<CapacityTarget, ResourceAccounting> accounting;
  std::map<RequestKey, RequestExplanation> explanations;
  std::map<CapacityTarget, DecisionId> last_decision;
  std::map<std::uint64_t, std::uint64_t> retired_obligations;

  std::uint64_t recoveries{0};
  std::uint64_t journal_truncations{0};
  std::uint64_t snapshots_written{0};

  Impl() = default;

  [[nodiscard]] Status persist(RecordType type, const std::vector<std::uint8_t>& payload) {
    if (!store) {
      audit_sequence += 1;
      return Status::success();
    }
    const auto sequence = AuditSequence::make(audit_sequence + 1);
    if (!sequence.ok()) {
      return sequence.error();
    }
    RecordHeader header;
    header.sequence = sequence.value();
    header.tick = tick;
    header.epoch = epoch;
    header.coordinator = incarnation;
    BB_RETURN_IF_ERROR(store->append(type, header, payload));
    audit_sequence = sequence.value().value();
    return Status::success();
  }

  [[nodiscard]] Status persist_grant(RecordType type, const Grant& grant) {
    Writer writer;
    encode(writer, grant);
    return persist(type, writer.take());
  }

  // Writes a full state image and retires the journals it covers. Called only
  // while the broker lock is held, so the image is consistent by construction.
  [[nodiscard]] Status maybe_compact() {
    if (!store) {
      return Status::success();
    }
    SnapshotImage image;
    image.sequence = store->report().last_sequence.valid() ? store->report().last_sequence.value() : 0;
    image.sequence = std::max(image.sequence, audit_sequence);
    image.header.sequence = AuditSequence::from_value(image.sequence);
    image.header.tick = tick;
    image.header.epoch = epoch;
    image.header.coordinator = incarnation;
    image.payload = snapshot_payload();
    BB_RETURN_IF_ERROR(store->compact(image));
    snapshots_written += 1;
    return Status::success();
  }

  [[nodiscard]] std::vector<std::uint8_t> snapshot_payload() {
    Writer writer;
    write_id(writer, epoch);
    write_id(writer, incarnation);
    writer.u64(tick);
    writer.u64(audit_sequence);
    writer.u64(next_grant_id);
    writer.u64(next_recall_id);
    writer.u64(next_decision_id);
    writer.u64(next_snapshot_id);
    writer.u64(next_session_id);
    writer.boolean(policy.has_value());
    if (policy.has_value()) {
      encode(writer, policy.value());
    }
    writer.u32(static_cast<std::uint32_t>(capacities.size()));
    for (const auto& entry : capacities) {
      encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(obligations.size()));
    for (const auto& entry : obligations) {
      encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(requests.size()));
    for (const auto& entry : requests) {
      encode(writer, entry.second);
      const auto wait = wait_rounds.find(entry.first);
      writer.u64(wait == wait_rounds.end() ? 0 : wait->second);
    }
    writer.u32(static_cast<std::uint32_t>(grants.size()));
    for (const auto& entry : grants) {
      encode(writer, entry.second);
    }
    writer.u32(static_cast<std::uint32_t>(fences.size()));
    for (const BootFence& fence : fences) {
      write_id(writer, fence.publisher);
      encode(writer, fence.boot);
      write_id(writer, fence.sequence);
      writer.u64(fence.tick);
      writer.text(fence.reason);
    }
    writer.u32(static_cast<std::uint32_t>(idempotency.size()));
    for (const auto& entry : idempotency) {
      write_id(writer, entry.second.request);
      write_generation(writer, entry.second.generation);
      const auto hash = content_hashes.find(entry.first);
      writer.u64(hash == content_hashes.end() ? 0 : hash->second);
      writer.u8(static_cast<std::uint8_t>(entry.second.reason));
      writer.boolean(entry.second.waiting);
      writer.boolean(entry.second.refused);
    }
    writer.u32(static_cast<std::uint32_t>(retired_obligations.size()));
    for (const auto& entry : retired_obligations) {
      write_id(writer, ReservationReferenceId::from_value(entry.first));
      write_generation(writer, ReservationGeneration::from_value(entry.second));
    }
    return writer.take();
  }

  [[nodiscard]] Status load_snapshot(const SnapshotImage& image) {
    Reader reader(image.payload);
    const auto epoch_value = read_id<FabricEpochTag>(reader);
    if (!epoch_value.ok()) return epoch_value.error();
    epoch = epoch_value.value();
    const auto coordinator = read_id<CoordinatorIncarnationTag>(reader);
    if (!coordinator.ok()) return coordinator.error();
    incarnation = coordinator.value();
    const auto tick_value = reader.u64();
    if (!tick_value.ok()) return tick_value.error();
    tick = tick_value.value();
    const auto sequence = reader.u64();
    if (!sequence.ok()) return sequence.error();
    audit_sequence = sequence.value();
    std::uint64_t* counters[] = {&next_grant_id, &next_recall_id, &next_decision_id, &next_snapshot_id,
                                 &next_session_id};
    for (std::uint64_t* counter : counters) {
      const auto parsed = reader.u64();
      if (!parsed.ok()) return parsed.error();
      *counter = parsed.value();
    }
    const auto has_policy = reader.boolean();
    if (!has_policy.ok()) return has_policy.error();
    if (has_policy.value()) {
      auto restored = decode_policy(reader);
      if (!restored.ok()) return corrupt(restored.error().message);
      policy = restored.take();
    }
    const auto capacity_count = reader.u32();
    if (!capacity_count.ok()) return capacity_count.error();
    if (capacity_count.value() > limits::kMaxResources) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many resources");
    }
    for (std::uint32_t i = 0; i < capacity_count.value(); ++i) {
      auto snapshot = decode_capacity_snapshot(reader);
      if (!snapshot.ok()) return corrupt(snapshot.error().message);
      capacities[snapshot.value().target] = snapshot.take();
    }
    const auto obligation_count = reader.u32();
    if (!obligation_count.ok()) return obligation_count.error();
    if (obligation_count.value() > limits::kMaxObligationsPerResource * limits::kMaxResources) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many obligations");
    }
    for (std::uint32_t i = 0; i < obligation_count.value(); ++i) {
      auto obligation = decode_obligation(reader);
      if (!obligation.ok()) return corrupt(obligation.error().message);
      obligations[obligation.value().reservation.value()] = obligation.take();
    }
    const auto request_count = reader.u32();
    if (!request_count.ok()) return request_count.error();
    if (request_count.value() > limits::kMaxRequestsPerRound * 16) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many requests");
    }
    for (std::uint32_t i = 0; i < request_count.value(); ++i) {
      auto request = decode_request(reader);
      if (!request.ok()) return corrupt(request.error().message);
      const auto wait = reader.u64();
      if (!wait.ok()) return wait.error();
      const RequestKey key = request_key(request.value().id, request.value().generation);
      wait_rounds[key] = wait.value();
      requests[key] = request.take();
    }
    const auto grant_count = reader.u32();
    if (!grant_count.ok()) return grant_count.error();
    if (grant_count.value() > limits::kMaxGrantsPerResource * limits::kMaxResources) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many grants");
    }
    for (std::uint32_t i = 0; i < grant_count.value(); ++i) {
      auto grant = decode_grant(reader);
      if (!grant.ok()) return corrupt(grant.error().message);
      const std::uint64_t id = grant.value().id.value();
      grants[id] = grant.take();
      next_grant_id = std::max(next_grant_id, id + 1);
    }
    const auto fence_count = reader.u32();
    if (!fence_count.ok()) return fence_count.error();
    if (fence_count.value() > limits::kMaxBootFenceRecords) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many fences");
    }
    for (std::uint32_t i = 0; i < fence_count.value(); ++i) {
      BootFence fence;
      const auto publisher = read_id<PublisherTag>(reader);
      if (!publisher.ok()) return publisher.error();
      fence.publisher = publisher.value();
      const auto boot = decode_boot_id(reader);
      if (!boot.ok()) return boot.error();
      fence.boot = boot.value();
      const auto fence_sequence = read_id<AuditSequenceTag>(reader);
      if (!fence_sequence.ok()) return fence_sequence.error();
      fence.sequence = fence_sequence.value();
      const auto fence_tick = reader.u64();
      if (!fence_tick.ok()) return fence_tick.error();
      fence.tick = fence_tick.value();
      const auto reason = reader.bytes(128);
      if (!reason.ok()) return reason.error();
      fence.reason.assign(reason.value());
      fence_hashes.insert(fence.boot.hash());
      fences.push_back(std::move(fence));
    }
    const auto idempotency_count = reader.u32();
    if (!idempotency_count.ok()) return idempotency_count.error();
    if (idempotency_count.value() > limits::kMaxIdempotencyEntries) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many idempotency entries");
    }
    for (std::uint32_t i = 0; i < idempotency_count.value(); ++i) {
      IdempotencyRecordPayload record;
      const auto request = read_id<BandwidthRequestTag>(reader);
      if (!request.ok()) return request.error();
      record.request = request.value();
      const auto generation = read_generation<BandwidthRequestGenerationTag>(reader);
      if (!generation.ok()) return generation.error();
      record.generation = generation.value();
      const auto hash = reader.u64();
      if (!hash.ok()) return hash.error();
      record.content_hash = hash.value();
      const auto reason = reader.u8();
      if (!reason.ok()) return reason.error();
      if (reason.value() > 46) {
        return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot records an unknown outcome reason");
      }
      record.reason = static_cast<OutcomeReason>(reason.value());
      const auto waiting = reader.boolean();
      if (!waiting.ok()) return waiting.error();
      record.waiting = waiting.value();
      const auto refused = reader.boolean();
      if (!refused.ok()) return refused.error();
      record.refused = refused.value();
      const RequestKey key = request_key(record.request, record.generation);
      SubmitOutcome outcome;
      outcome.disposition = IdempotencyDisposition::Replay;
      outcome.request = record.request;
      outcome.generation = record.generation;
      outcome.waiting = record.waiting;
      outcome.refused = record.refused;
      outcome.reason = record.reason;
      idempotency[key] = outcome;
      content_hashes[key] = record.content_hash;
    }
    const auto retired_count = reader.u32();
    if (!retired_count.ok()) return retired_count.error();
    if (retired_count.value() > limits::kMaxObligationsPerResource * limits::kMaxResources) {
      return make_error_status(ErrorCode::PersistenceCorrupt, "snapshot declares too many retired obligations");
    }
    for (std::uint32_t i = 0; i < retired_count.value(); ++i) {
      const auto reservation = read_id<ReservationReferenceTag>(reader);
      if (!reservation.ok()) return reservation.error();
      const auto generation = read_generation<ReservationGenerationTag>(reader);
      if (!generation.ok()) return generation.error();
      retired_obligations[reservation.value().value()] = generation.value().value();
    }
    return reader.expect_end();
  }

  [[nodiscard]] Status replay_records(const std::vector<Record>& records) {
    for (const Record& record : records) {
      Reader reader(record.payload);
      switch (record.type) {
        case RecordType::PolicySet: {
          auto restored = decode_policy(reader);
          if (!restored.ok()) return corrupt(restored.error().message);
          policy = restored.take();
          break;
        }
        case RecordType::CapacityPublished: {
          auto snapshot = decode_capacity_snapshot(reader);
          if (!snapshot.ok()) return corrupt(snapshot.error().message);
          capacities[snapshot.value().target] = snapshot.take();
          break;
        }
        case RecordType::ObligationUpserted: {
          auto obligation = decode_obligation(reader);
          if (!obligation.ok()) return corrupt(obligation.error().message);
          const std::uint64_t id = obligation.value().reservation.value();
          obligations[id] = obligation.take();
          retired_obligations.erase(id);
          break;
        }
        case RecordType::ObligationRetired: {
          const auto reference = decode_reference(reader);
          if (!reference.ok()) return corrupt(reference.error().message);
          obligations.erase(reference.value().first.value());
          retired_obligations[reference.value().first.value()] = reference.value().second.value();
          break;
        }
        case RecordType::RequestAccepted: {
          auto request = decode_request(reader);
          if (!request.ok()) return corrupt(request.error().message);
          requests[request_key(request.value().id, request.value().generation)] = request.take();
          break;
        }
        case RecordType::RequestRetired: {
          const auto key = decode_request_key(reader);
          if (!key.ok()) return corrupt(key.error().message);
          requests.erase(request_key(key.value().first, key.value().second));
          break;
        }
        case RecordType::RoundCommitted: {
          const auto round = decode_round(reader);
          if (!round.ok()) return corrupt(round.error().message);
          accounting[round.value().target] = round.value().accounting;
          last_decision[round.value().target] = round.value().decision;
          next_grant_id = std::max(next_grant_id, round.value().next_grant_id);
          next_recall_id = std::max(next_recall_id, round.value().next_recall_id);
          break;
        }
        case RecordType::GrantIssued:
        case RecordType::GrantUpdated:
        case RecordType::GrantReleased:
        case RecordType::GrantRevoked:
        case RecordType::GrantRevalidated: {
          auto grant = decode_grant(reader);
          if (!grant.ok()) return corrupt(grant.error().message);
          const std::uint64_t id = grant.value().id.value();
          grants[id] = grant.take();
          next_grant_id = std::max(next_grant_id, id + 1);
          break;
        }
        case RecordType::PublisherFenced: {
          const auto fence = decode_fence(reader);
          if (!fence.ok()) return corrupt(fence.error().message);
          BootFence restored;
          restored.publisher = fence.value().publisher;
          restored.boot = fence.value().boot;
          restored.sequence = fence.value().sequence;
          restored.tick = fence.value().tick;
          restored.reason = fence.value().reason;
          fence_hashes.insert(restored.boot.hash());
          fences.push_back(std::move(restored));
          break;
        }
        case RecordType::EpochAdvanced: {
          const auto advanced = read_id<FabricEpochTag>(reader);
          if (!advanced.ok()) return corrupt(advanced.error().message);
          epoch = advanced.value();
          break;
        }
        case RecordType::IdempotencyRecord: {
          const auto entry = decode_idempotency(reader);
          if (!entry.ok()) return corrupt(entry.error().message);
          const RequestKey key = request_key(entry.value().request, entry.value().generation);
          SubmitOutcome outcome;
          outcome.disposition = IdempotencyDisposition::Replay;
          outcome.request = entry.value().request;
          outcome.generation = entry.value().generation;
          outcome.waiting = entry.value().waiting;
          outcome.refused = entry.value().refused;
          outcome.reason = entry.value().reason;
          idempotency[key] = outcome;
          content_hashes[key] = entry.value().content_hash;
          break;
        }
        case RecordType::CoordinatorBoot: {
          const auto boot = decode_boot(reader);
          if (!boot.ok()) return corrupt(boot.error().message);
          break;
        }
      }
      tick = std::max(tick, record.header.tick);
      audit_sequence = std::max(audit_sequence, record.header.sequence.value());
    }
    return Status::success();
  }
};

Broker::Broker() : impl_(std::make_unique<Impl>()) {}
Broker::~Broker() = default;

Result<std::unique_ptr<Broker>> Broker::open(const BrokerConfig& config) {
  if (!config.epoch.valid()) {
    return make_error<std::unique_ptr<Broker>>(ErrorCode::InvalidIdentity, "coordinator epoch is not set");
  }
  if (!config.incarnation.valid()) {
    return make_error<std::unique_ptr<Broker>>(ErrorCode::InvalidIdentity, "coordinator incarnation is not set");
  }
  if (config.max_idempotency_entries == 0) {
    return make_error<std::unique_ptr<Broker>>(ErrorCode::InvalidArgument, "idempotency table must have a bound");
  }

  auto broker = std::unique_ptr<Broker>(new Broker());
  Impl& impl = *broker->impl_;
  impl.config = config;
  impl.incarnation = config.incarnation;
  impl.epoch = config.epoch;

  bool had_durable_state = false;
  if (!config.store_directory.empty()) {
    StoreOptions options;
    options.fsync_on_commit = config.fsync_on_commit;
    options.compaction_records = config.journal_compaction_records;
    auto store = Store::open(config.store_directory, options);
    if (!store.ok()) {
      return store.error();
    }
    impl.store = store.take();

    auto recovered = impl.store->recover();
    if (!recovered.ok()) {
      return recovered.error();
    }
    impl.recoveries = 1;
    impl.journal_truncations = recovered.value().report.truncations;
    if (recovered.value().has_snapshot) {
      BB_RETURN_IF_ERROR(impl.load_snapshot(recovered.value().snapshot));
    }
    BB_RETURN_IF_ERROR(impl.replay_records(recovered.value().records));
    had_durable_state = recovered.value().has_snapshot || !recovered.value().records.empty();
    impl.tick = std::max(impl.tick, config.epoch.valid() ? std::uint64_t{0} : std::uint64_t{0});
  }

  // Dynamic evidence never becomes current again by being reloaded: capacity
  // evidence is demoted to Stale and every live grant is moved to
  // RevalidationRequired before the coordinator accepts new work.
  for (auto& entry : impl.capacities) {
    entry.second.evidence = CapacityEvidenceState::Stale;
  }
  for (auto& entry : impl.grants) {
    if (is_live(entry.second.state)) {
      entry.second.state = GrantState::RevalidationRequired;
    }
  }
  impl.accounting.clear();
  impl.explanations.clear();
  impl.last_decision.clear();

  if (had_durable_state) {
    const auto next = next_fabric_epoch(impl.epoch);
    if (!next.ok()) {
      return next.error();
    }
    impl.epoch = next.value();
  }
  impl.tick += 1;

  if (impl.store) {
    Writer epoch_writer;
    write_id(epoch_writer, impl.epoch);
    BB_RETURN_IF_ERROR(impl.persist(RecordType::EpochAdvanced, epoch_writer.take()));

    BootRecord boot;
    boot.coordinator = impl.incarnation;
    boot.epoch = impl.epoch;
    boot.tick = impl.tick;
    boot.sequence = AuditSequence::from_value(impl.audit_sequence + 1);
    BB_RETURN_IF_ERROR(impl.persist(RecordType::CoordinatorBoot, encode_boot(boot)));
    BB_RETURN_IF_ERROR(impl.store->flush());
  }
  impl.snapshots_written = 0;
  return broker;
}

namespace {

[[nodiscard]] RequestExplanation build_explanation(const ArbitrationDecision& decision,
                                                   const ArbitrationInput& input,
                                                   const ResourceAccounting& accounting,
                                                   const BandwidthRequest& request) {
  RequestExplanation explanation;
  explanation.request = decision.request;
  explanation.request_generation = decision.request_generation;
  explanation.target = input.target;
  explanation.state = decision.grant.has_value() ? decision.grant->state : decision.state;
  explanation.reason = decision.reason;
  explanation.satisfied = decision.satisfied;
  explanation.waiting = decision.waiting;
  explanation.refused = decision.refused;
  explanation.authority = decision.grant.has_value() ? decision.grant->authority : request.authority;
  explanation.epoch = input.epoch;
  explanation.coordinator = input.coordinator;
  explanation.policy = input.policy.id;
  explanation.policy_generation = input.policy.generation;
  explanation.decision = input.decision;
  explanation.decision_tick = input.tick;

  explanation.evidence = accounting.evidence;
  explanation.capacity_generation = accounting.capacity_generation;
  explanation.effective_physical = accounting.effective_physical;
  explanation.obligations_applied = accounting.obligations_reserved;
  explanation.allocatable = accounting.allocatable;
  explanation.headroom_preserved = accounting.headroom;
  explanation.emergency_reserve_preserved = accounting.emergency_reserve_unused;
  explanation.arbitrable = accounting.arbitrable;

  explanation.requested_minimum = decision.requested_minimum;
  explanation.requested_desired = decision.requested_desired;
  explanation.requested_maximum = decision.requested_maximum;
  explanation.guaranteed = decision.allocation.guaranteed;
  explanation.discretionary = decision.allocation.discretionary;
  explanation.borrowed = decision.allocation.borrowed;
  explanation.contingent = decision.allocation.contingent;
  const auto total = decision.allocation.total();
  explanation.total_granted = total.ok() ? total.value() : Bandwidth::zero();
  explanation.denied = decision.denied;

  explanation.priority = request.priority;
  explanation.fairness_group = request.fairness_group;
  explanation.tenant = request.tenant;
  explanation.effective_rank = decision.effective_rank;
  explanation.effective_weight = decision.effective_weight;
  explanation.starvation_promoted = decision.starvation_promoted;
  explanation.resource_generation = input.target.resource_generation;
  explanation.binding_reason = decision.binding_reason;
  explanation.authority_mismatch = decision.authority_mismatch;
  explanation.provenance = request.provenance;
  if (decision.grant.has_value()) {
    explanation.grant = decision.grant->id;
    explanation.grant_generation = decision.grant->generation;
  }
  if (decision.recall.has_value()) {
    explanation.recall_pending = decision.grant.has_value() &&
                                 decision.grant->state == GrantState::RecallPending;
    explanation.recalled = decision.recall->recalled;
    explanation.recall = decision.recall->id;
    explanation.recall_reason = decision.recall->reason;
    explanation.recall_deadline_tick = decision.recall->deadline_tick;
  }
  return explanation;
}

}  // namespace

Status Broker::set_policy(const Policy& policy) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto valid = policy.validate();
  if (!valid.ok()) {
    return valid;
  }
  if (impl_->policy.has_value()) {
    const Policy& installed = impl_->policy.value();
    if (policy.generation < installed.generation) {
      return make_error_status(ErrorCode::PolicyGenerationMismatch,
                               "a new policy must not carry a generation below the installed policy generation");
    }
    if (policy.generation == installed.generation) {
      if (policy.content_hash() == installed.content_hash()) {
        // Re-installing byte-identical content at the same generation is
        // idempotent: nothing authoritative changes.
        return Status::success();
      }
      return make_error_status(ErrorCode::IdentityConflict,
                               "the installed policy generation was reused with different content");
    }
  }
  Writer writer;
  encode(writer, policy);
  BB_RETURN_IF_ERROR(impl_->persist(RecordType::PolicySet, writer.take()));
  impl_->policy = policy;
  return Status::success();
}

Result<Policy> Broker::policy() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->policy.has_value()) {
    return make_error<Policy>(ErrorCode::NotFound, "no policy has been installed");
  }
  return impl_->policy.value();
}

Status Broker::publish_capacity(const CapacitySnapshot& snapshot) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto valid = snapshot.validate();
  if (!valid.ok()) {
    return valid;
  }
  if (snapshot.authority.fabric_epoch != impl_->epoch) {
    return make_error_status(ErrorCode::EpochMismatch,
                             "capacity publication carries a fabric epoch that is not the current epoch");
  }
  if (snapshot.authority.coordinator != impl_->incarnation) {
    return make_error_status(ErrorCode::NotAuthoritative,
                             "capacity publication names a different coordinator incarnation");
  }
  const auto existing = impl_->capacities.find(snapshot.target);
  if (existing != impl_->capacities.end() && snapshot.generation <= existing->second.generation) {
    return make_error_status(ErrorCode::ResourceGenerationMismatch,
                             "capacity publication generation is not above the installed generation");
  }
  Writer writer;
  encode(writer, snapshot);
  BB_RETURN_IF_ERROR(impl_->persist(RecordType::CapacityPublished, writer.take()));
  impl_->capacities[snapshot.target] = snapshot;
  // Every grant is bound to a capacity generation. A publication that advances
  // that generation therefore supersedes every live grant on the target before
  // any round runs: the commitment can never silently continue under capacity
  // evidence that no longer justifies it.
  for (auto& entry : impl_->grants) {
    if (entry.second.target == snapshot.target && is_live(entry.second.state)) {
      entry.second.state = GrantState::RevalidationRequired;
    }
  }
  impl_->accounting.erase(snapshot.target);
  return Status::success();
}

Result<CapacitySnapshot> Broker::capacity(const CapacityTarget& target) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->capacities.find(target);
  if (found == impl_->capacities.end()) {
    return make_error<CapacitySnapshot>(ErrorCode::NotFound, "no capacity has been published for this target");
  }
  return found->second;
}

Status Broker::upsert_obligation(const Obligation& obligation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto valid = obligation.validate();
  if (!valid.ok()) {
    return valid;
  }
  const auto existing = impl_->obligations.find(obligation.reservation.value());
  if (existing != impl_->obligations.end() && obligation.generation < existing->second.generation) {
    return make_error_status(ErrorCode::ReservationGenerationMismatch,
                             "obligation generation is below the installed obligation generation");
  }
  Writer writer;
  encode(writer, obligation);
  BB_RETURN_IF_ERROR(impl_->persist(RecordType::ObligationUpserted, writer.take()));
  impl_->obligations[obligation.reservation.value()] = obligation;
  impl_->retired_obligations.erase(obligation.reservation.value());
  return Status::success();
}

Status Broker::retire_obligation(ReservationReferenceId reservation, ReservationGeneration generation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto existing = impl_->obligations.find(reservation.value());
  if (existing == impl_->obligations.end()) {
    return make_error_status(ErrorCode::NotFound, "no such obligation is installed");
  }
  if (existing->second.generation != generation) {
    return make_error_status(ErrorCode::ReservationGenerationMismatch,
                             "obligation retirement names a stale obligation generation");
  }
  BB_RETURN_IF_ERROR(impl_->persist(RecordType::ObligationRetired, encode_reference(reservation, generation)));
  impl_->obligations.erase(reservation.value());
  impl_->retired_obligations[reservation.value()] = generation.value();
  return Status::success();
}

Result<std::vector<Obligation>> Broker::obligations(const CapacityTarget& target) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<Obligation> out;
  for (const auto& entry : impl_->obligations) {
    if (entry.second.target == target) {
      out.push_back(entry.second);
    }
  }
  return out;
}

Result<SubmitOutcome> Broker::submit(const BandwidthRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->policy.has_value()) {
    return make_error<SubmitOutcome>(ErrorCode::PolicyInvalid, "no policy has been installed");
  }
  const auto valid = request.validate();
  if (!valid.ok()) {
    return valid.error();
  }
  if (request.authority.fabric_epoch != impl_->epoch) {
    return make_error<SubmitOutcome>(ErrorCode::EpochMismatch,
                                     "request was built against a superseded fabric epoch");
  }
  const RequestKey key = request_key(request.id, request.generation);
  const std::uint64_t hash = request.content_hash();
  const auto existing = impl_->idempotency.find(key);
  if (existing != impl_->idempotency.end()) {
    const auto recorded_hash = impl_->content_hashes.find(key);
    if (recorded_hash != impl_->content_hashes.end() && recorded_hash->second != hash) {
      return make_error<SubmitOutcome>(ErrorCode::IdentityConflict,
                                       "the request identity was reused with different content");
    }
    SubmitOutcome replay = existing->second;
    replay.disposition = IdempotencyDisposition::Replay;
    return replay;
  }
  if (impl_->idempotency.size() >= impl_->config.max_idempotency_entries) {
    return make_error<SubmitOutcome>(ErrorCode::ResourceExhausted, "idempotency table is full");
  }

  SubmitOutcome outcome;
  outcome.disposition = IdempotencyDisposition::New;
  outcome.request = request.id;
  outcome.generation = request.generation;
  if (impl_->fence_hashes.count(request.authority.publisher_boot.hash()) != 0) {
    outcome.refused = true;
    outcome.reason = OutcomeReason::RefusedBootFenced;
  }

  Writer request_writer;
  encode(request_writer, request);
  BB_RETURN_IF_ERROR(impl_->persist(RecordType::RequestAccepted, request_writer.take()));

  IdempotencyRecordPayload record;
  record.request = request.id;
  record.generation = request.generation;
  record.content_hash = hash;
  record.reason = outcome.reason;
  record.waiting = outcome.waiting;
  record.refused = outcome.refused;
  BB_RETURN_IF_ERROR(impl_->persist(RecordType::IdempotencyRecord, encode_idempotency(record)));

  impl_->requests[key] = request;
  impl_->content_hashes[key] = hash;
  impl_->wait_rounds[key] = 0;
  impl_->idempotency[key] = outcome;
  return outcome;
}

Status Broker::retire_request(BandwidthRequestId id, BandwidthRequestGeneration generation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const RequestKey key = request_key(id, generation);
  if (impl_->requests.erase(key) == 0) {
    return make_error_status(ErrorCode::NotFound, "no such request is installed");
  }
  impl_->wait_rounds.erase(key);
  return impl_->persist(RecordType::RequestRetired, encode_request_key(id, generation));
}

Result<RoundSummary> Broker::arbitrate(const CapacityTarget& target) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  BB_RETURN_IF_ERROR(target.validate());
  if (!impl.policy.has_value()) {
    return make_error<RoundSummary>(ErrorCode::PolicyInvalid, "no policy has been installed");
  }

  // Requests that were retired or released stop competing immediately; their
  // live grants are revoked before the round so the round sees complete state.
  std::size_t orphan_recalls = 0;
  std::vector<BandwidthGrantId> orphans;
  for (const auto& entry : impl.grants) {
    const Grant& grant = entry.second;
    if (grant.target != target || !is_live(grant.state)) {
      continue;
    }
    if (impl.requests.count(request_key(grant.request, grant.request_generation)) == 0) {
      orphans.push_back(grant.id);
    }
  }
  for (const BandwidthGrantId id : orphans) {
    Grant& grant = impl.grants[id.value()];
    const Grant before = grant;
    const auto bumped = grant.advance_generation();
    if (!bumped.ok()) {
      return bumped.error();
    }
    grant.reason = OutcomeReason::Revoked;
    const auto transitioned = grant.transition_to(GrantState::Revoked);
    if (!transitioned.ok()) {
      return transitioned.error();
    }
    BB_RETURN_IF_ERROR(impl.persist_grant(RecordType::GrantRevoked, grant));
    const auto account = impl.accounting.find(target);
    if (account != impl.accounting.end()) {
      BB_RETURN_IF_ERROR(apply_grant_retirement(account->second, before));
    }
    if (before.allocation.total().ok() && before.allocation.total().value().is_positive()) {
      ++orphan_recalls;
    }
  }

  ArbitrationInput input;
  input.target = target;
  const auto snapshot = impl.capacities.find(target);
  // Capacity evidence published under a superseded fabric epoch or by a
  // superseded coordinator incarnation is no longer usable. It is demoted to
  // Stale and the round proceeds with unusable evidence, so every request is
  // explicitly waiting instead of the round failing outright.
  const bool snapshot_current = snapshot != impl.capacities.end() &&
                                snapshot->second.authority.fabric_epoch == impl.epoch &&
                                snapshot->second.authority.coordinator == impl.incarnation;
  if (snapshot_current) {
    input.snapshot = snapshot->second;
  } else {
    if (snapshot != impl.capacities.end()) {
      snapshot->second.evidence = CapacityEvidenceState::Stale;
    }
    // No capacity has ever been published: arbitration proceeds with UNKNOWN
    // evidence so that every request is explicitly waiting, never silently
    // granted from imagined capacity.
    const bool had_snapshot = snapshot != impl.capacities.end();
    input.snapshot.target = target;
    input.snapshot.snapshot = CapacitySnapshotId::from_value(impl.next_snapshot_id);
    input.snapshot.generation =
        had_snapshot ? snapshot->second.generation : CapacitySnapshotGeneration::initial();
    input.snapshot.evidence = had_snapshot ? CapacityEvidenceState::Stale : CapacityEvidenceState::Unknown;
    input.snapshot.provenance.source_kind = NodeKind::Recovery;
    input.snapshot.provenance.epoch = impl.epoch;
    input.snapshot.provenance.coordinator = impl.incarnation;
    input.snapshot.authority.fabric_epoch = impl.epoch;
    input.snapshot.authority.coordinator = impl.incarnation;
    input.snapshot.authority.resource = target.resource;
    input.snapshot.authority.resource_generation = target.resource_generation;
    input.snapshot.authority.capacity_generation = CapacitySnapshotGeneration::initial();
    input.snapshot.authority.publisher = PublisherId::from_value(1);
    input.snapshot.authority.publisher_boot = boot_id_from_u64(0, 1);
  }
  input.policy = impl.policy.value();
  for (const auto& entry : impl.obligations) {
    if (entry.second.target == target) {
      input.obligations.push_back(entry.second);
    }
  }
  for (const auto& fence : impl.fences) {
    input.fenced_boots.push_back(fence.boot);
  }
  for (const auto& entry : impl.requests) {
    if (entry.second.target != target) {
      continue;
    }
    ArbitrationCandidate candidate;
    candidate.request = entry.second;
    const auto wait = impl.wait_rounds.find(entry.first);
    candidate.wait_rounds = wait == impl.wait_rounds.end() ? 0 : wait->second;
    for (const auto& grant_entry : impl.grants) {
      const Grant& grant = grant_entry.second;
      if (grant.request == entry.second.id && grant.request_generation == entry.second.generation &&
          is_live(grant.state)) {
        candidate.existing_grant = grant;
        break;
      }
    }
    input.candidates.push_back(candidate);
  }
  input.epoch = impl.epoch;
  input.coordinator = impl.incarnation;
  impl.tick += 1;
  input.tick = impl.tick;
  const auto decision_id = DecisionId::make(impl.next_decision_id);
  if (!decision_id.ok()) {
    return decision_id.error();
  }
  input.decision = decision_id.value();
  input.next_grant_id = impl.next_grant_id;
  input.next_recall_id = impl.next_recall_id;

  auto outcome = bandwidth_broker::arbitrate(input);
  if (!outcome.ok()) {
    return outcome.error();
  }
  ArbitrationOutcome& result = outcome.value();

  for (const ArbitrationDecision& decision : result.decisions) {
    const RequestKey key = request_key(decision.request, decision.request_generation);
    const auto previous = impl.grants.end();
    bool had_previous = false;
    if (decision.grant.has_value()) {
      const auto existing = impl.grants.find(decision.grant->id.value());
      had_previous = existing != impl.grants.end();
      (void)previous;
      impl.grants[decision.grant->id.value()] = decision.grant.value();
      RecordType type = had_previous ? RecordType::GrantUpdated : RecordType::GrantIssued;
      if (decision.grant->state == GrantState::Released) {
        type = RecordType::GrantReleased;
      } else if (is_terminal(decision.grant->state)) {
        type = RecordType::GrantRevoked;
      }
      BB_RETURN_IF_ERROR(impl.persist_grant(type, decision.grant.value()));
    }
    impl.wait_rounds[key] = decision.next_wait_rounds;
    const auto request_entry = impl.requests.find(key);
    if (request_entry != impl.requests.end()) {
      const auto explanation =
          build_explanation(decision, input, result.accounting, request_entry->second);
      const auto valid_explanation = explanation.validate();
      if (!valid_explanation.ok()) {
        return valid_explanation.error();
      }
      impl.explanations[key] = explanation;
    }
  }
  impl.next_grant_id = std::max(impl.next_grant_id, result.next_grant_id);
  impl.next_recall_id = std::max(impl.next_recall_id, result.next_recall_id);
  impl.next_decision_id += 1;
  impl.accounting[target] = result.accounting;
  impl.last_decision[target] = result.decision;

  RoundRecord round;
  round.decision = result.decision;
  round.target = target;
  round.tick = result.tick;
  round.accounting = result.accounting;
  round.next_grant_id = result.next_grant_id;
  round.next_recall_id = result.next_recall_id;
  BB_RETURN_IF_ERROR(impl.persist(RecordType::RoundCommitted, encode_round(round)));

  RoundSummary summary;
  summary.decision = result.decision;
  summary.target = target;
  summary.tick = result.tick;
  summary.next_grant_id = result.next_grant_id;
  summary.accounting = result.accounting;
  for (const ArbitrationDecision& decision : result.decisions) {
    if (decision.grant.has_value()) {
      ++summary.granted;
    } else if (decision.refused) {
      ++summary.refused;
    } else {
      ++summary.waiting;
    }
    if (decision.recall.has_value()) {
      ++summary.recalls;
    }
  }
  summary.recalls += orphan_recalls;

  if (impl.store && impl.store->needs_compaction()) {
    BB_RETURN_IF_ERROR(impl.maybe_compact());
  }
  return summary;
}

Result<GrantView> Broker::query_grant(BandwidthGrantId id, BandwidthGrantGeneration generation) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  GrantView view;
  const auto found = impl_->grants.find(id.value());
  if (found == impl_->grants.end()) {
    view.note = "no such grant";
    return view;
  }
  view.found = true;
  view.grant = found->second;
  view.generation_current = found->second.generation == generation;
  view.authoritative = found->second.authorises_consumption() && view.generation_current;
  if (!view.generation_current) {
    view.note = "grant generation is superseded; the presented generation authorises nothing";
  } else if (!view.authoritative) {
    view.note = std::string("grant state ") + to_string(view.grant.state) + " authorises no consumption";
  } else {
    view.note = "authoritative";
  }
  return view;
}

Result<std::vector<Grant>> Broker::grants(const CapacityTarget& target, bool live_only) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<Grant> out;
  for (const auto& entry : impl_->grants) {
    if (entry.second.target != target) {
      continue;
    }
    if (live_only && !is_live(entry.second.state)) {
      continue;
    }
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(),
            [](const Grant& a, const Grant& b) { return a.id < b.id; });
  return out;
}

Result<ReleaseOutcome> Broker::release(BandwidthGrantId id, BandwidthGrantGeneration generation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const auto found = impl.grants.find(id.value());
  if (found == impl.grants.end()) {
    return make_error<ReleaseOutcome>(ErrorCode::NotFound, "no such grant");
  }
  Grant& grant = found->second;
  if (grant.generation != generation) {
    return make_error<ReleaseOutcome>(ErrorCode::StaleGrant,
                                      "the presented grant generation is superseded and authorises nothing");
  }
  ReleaseOutcome outcome;
  outcome.tick = impl.tick;
  if (grant.state == GrantState::Released) {
    // Releasing twice is idempotent: accounting never changes twice for the
    // same grant.
    outcome.already_released = true;
    outcome.state = GrantState::Released;
    return outcome;
  }
  if (is_terminal(grant.state)) {
    return make_error<ReleaseOutcome>(ErrorCode::InvalidStateTransition,
                                      std::string("grant is already ") + to_string(grant.state));
  }
  const Grant before = grant;
  const auto bumped = grant.advance_generation();
  if (!bumped.ok()) {
    return bumped.error();
  }
  grant.reason = OutcomeReason::Released;
  const auto transitioned = grant.transition_to(GrantState::Released);
  if (!transitioned.ok()) {
    return transitioned.error();
  }
  BB_RETURN_IF_ERROR(impl.persist_grant(RecordType::GrantReleased, grant));
  const auto account = impl.accounting.find(grant.target);
  if (account != impl.accounting.end()) {
    BB_RETURN_IF_ERROR(apply_grant_retirement(account->second, before));
  }
  // Releasing withdraws the request as well: a released requester that still
  // wants capacity submits a new request generation.
  const RequestKey key = request_key(grant.request, grant.request_generation);
  impl.requests.erase(key);
  impl.wait_rounds.erase(key);
  BB_RETURN_IF_ERROR(impl.persist(RecordType::RequestRetired,
                                  encode_request_key(grant.request, grant.request_generation)));

  const auto released = before.allocation.total();
  outcome.state = GrantState::Released;
  outcome.released = released.ok() ? released.value() : Bandwidth::zero();
  outcome.tick = impl.tick;
  return outcome;
}

Result<GrantView> Broker::revoke(BandwidthGrantId id,
                                 BandwidthGrantGeneration generation,
                                 OutcomeReason reason,
                                 const std::string& detail) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const auto text_check = validate_text(detail, limits::kMaxExplanationTextBytes, "revocation detail");
  if (!text_check.ok()) {
    return text_check.error();
  }
  const auto found = impl.grants.find(id.value());
  if (found == impl.grants.end()) {
    return make_error<GrantView>(ErrorCode::NotFound, "no such grant");
  }
  Grant& grant = found->second;
  if (grant.generation != generation) {
    return make_error<GrantView>(ErrorCode::StaleGrant,
                                 "the presented grant generation is superseded and authorises nothing");
  }
  if (is_terminal(grant.state)) {
    GrantView view;
    view.found = true;
    view.grant = grant;
    view.generation_current = true;
    view.authoritative = false;
    view.note = std::string("grant is already ") + to_string(grant.state);
    return view;
  }
  const Grant before = grant;
  const auto bumped = grant.advance_generation();
  if (!bumped.ok()) {
    return bumped.error();
  }
  grant.reason = reason;
  const auto transitioned = grant.transition_to(GrantState::Revoked);
  if (!transitioned.ok()) {
    return transitioned.error();
  }
  BB_RETURN_IF_ERROR(impl.persist_grant(RecordType::GrantRevoked, grant));
  const auto account = impl.accounting.find(grant.target);
  if (account != impl.accounting.end()) {
    BB_RETURN_IF_ERROR(apply_grant_retirement(account->second, before));
  }

  GrantView view;
  view.found = true;
  view.grant = grant;
  view.generation_current = true;
  view.authoritative = false;
  view.note = detail.empty() ? "revoked" : detail;
  return view;
}

Result<GrantView> Broker::revalidate(BandwidthGrantId id, BandwidthGrantGeneration generation) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const auto found = impl.grants.find(id.value());
  if (found == impl.grants.end()) {
    return make_error<GrantView>(ErrorCode::NotFound, "no such grant");
  }
  Grant& grant = found->second;
  if (grant.generation != generation) {
    return make_error<GrantView>(ErrorCode::StaleGrant,
                                 "the presented grant generation is superseded and authorises nothing");
  }
  if (grant.state != GrantState::RevalidationRequired) {
    return make_error<GrantView>(ErrorCode::InvalidStateTransition,
                                 "only a grant awaiting revalidation can be revalidated");
  }
  const auto requested = impl.requests.find(request_key(grant.request, grant.request_generation));
  if (requested == impl.requests.end()) {
    const Grant before = grant;
    const auto bumped = grant.advance_generation();
    if (!bumped.ok()) {
      return bumped.error();
    }
    grant.reason = OutcomeReason::Stale;
    BB_RETURN_IF_ERROR(grant.transition_to(GrantState::Stale));
    BB_RETURN_IF_ERROR(impl.persist_grant(RecordType::GrantRevoked, grant));
    const auto account = impl.accounting.find(grant.target);
    if (account != impl.accounting.end()) {
      BB_RETURN_IF_ERROR(apply_grant_retirement(account->second, before));
    }
    return make_error<GrantView>(ErrorCode::RevalidationRequired,
                                 "the request behind this grant was not restored; the grant is stale");
  }
  const auto capacity = impl.capacities.find(grant.target);
  if (capacity == impl.capacities.end() || capacity->second.evidence != CapacityEvidenceState::Known) {
    return make_error<GrantView>(ErrorCode::CapacityUnknown,
                                 "capacity evidence is not current; the grant cannot be revalidated yet");
  }
  if (!impl.policy.has_value()) {
    return make_error<GrantView>(ErrorCode::PolicyInvalid, "no policy has been installed");
  }
  if (impl.fence_hashes.count(requested->second.authority.publisher_boot.hash()) != 0) {
    return make_error<GrantView>(ErrorCode::BootFenced,
                                 "the requester incarnation is permanently fenced and cannot be revalidated");
  }
  // Revalidation re-binds the restored request to the current fabric epoch,
  // current coordinator incarnation and current capacity generation. The
  // request's other bindings are re-derived from the installed policy.
  AuthorityVector authority = requested->second.authority;
  authority.fabric_epoch = impl.epoch;
  authority.coordinator = impl.incarnation;
  authority.capacity_generation = capacity->second.generation;
  authority.policy = impl.policy->id;
  authority.policy_generation = impl.policy->generation;
  authority.fairness_config_generation = impl.policy->fairness_config_generation;
  authority.tenant_config_generation = impl.policy->tenant_config_generation;
  authority.monotonic_tick = impl.tick;
  const auto bumped = grant.advance_generation();
  if (!bumped.ok()) {
    return bumped.error();
  }
  grant.authority = authority;
  grant.last_revalidated_tick = impl.tick;
  grant.reason = OutcomeReason::Stale;
  grant.state = grant.allocation.has_revocable() ? GrantState::GrantedBorrowed : GrantState::GrantedGuaranteed;
  BB_RETURN_IF_ERROR(impl.persist_grant(RecordType::GrantRevalidated, grant));

  GrantView view;
  view.found = true;
  view.grant = grant;
  view.generation_current = true;
  view.authoritative = true;
  view.note = "revalidated under the current epoch, capacity generation and policy";
  return view;
}

Status Broker::fence_publisher(PublisherId publisher, BootId boot, const std::string& reason) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  if (!publisher.valid() || !boot.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "fence requires a publisher id and a boot identity");
  }
  const auto text_check = validate_text(reason, 128, "fence reason");
  if (!text_check.ok()) {
    return text_check.error();
  }
  if (impl.fence_hashes.count(boot.hash()) != 0) {
    return Status::success();
  }
  if (impl.fences.size() >= limits::kMaxBootFenceRecords) {
    return make_error_status(ErrorCode::ResourceExhausted, "boot fence table is full");
  }
  BootFence fence;
  fence.publisher = publisher;
  fence.boot = boot;
  fence.tick = impl.tick;
  fence.reason = reason;
  fence.sequence = AuditSequence::from_value(impl.audit_sequence + 1);
  FenceRecord record;
  record.publisher = fence.publisher;
  record.boot = fence.boot;
  record.sequence = fence.sequence;
  record.tick = fence.tick;
  record.reason = fence.reason;
  BB_RETURN_IF_ERROR(impl.persist(RecordType::PublisherFenced, encode_fence(record)));
  impl.fence_hashes.insert(boot.hash());
  impl.fences.push_back(fence);

  // Fencing immediately withdraws anything the fenced incarnation still holds.
  std::vector<BandwidthGrantId> revoked;
  for (const auto& entry : impl.grants) {
    if (entry.second.authority.publisher_boot == boot && is_live(entry.second.state)) {
      revoked.push_back(entry.first == 0 ? BandwidthGrantId{} : BandwidthGrantId::from_value(entry.first));
    }
  }
  for (const BandwidthGrantId id : revoked) {
    Grant& grant = impl.grants[id.value()];
    const Grant before = grant;
    const auto bumped = grant.advance_generation();
    if (!bumped.ok()) {
      return bumped.error();
    }
    grant.reason = OutcomeReason::Fenced;
    BB_RETURN_IF_ERROR(grant.transition_to(GrantState::Fenced));
    BB_RETURN_IF_ERROR(impl.persist_grant(RecordType::GrantRevoked, grant));
    const auto account = impl.accounting.find(grant.target);
    if (account != impl.accounting.end()) {
      BB_RETURN_IF_ERROR(apply_grant_retirement(account->second, before));
    }
  }
  return Status::success();
}

Result<bool> Broker::is_fenced(PublisherId publisher, BootId boot) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  (void)publisher;
  if (!boot.valid()) {
    return make_error<bool>(ErrorCode::InvalidIdentity, "boot identity is not set");
  }
  return impl_->fence_hashes.count(boot.hash()) != 0;
}

Result<std::vector<BootFence>> Broker::fences() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->fences;
}

Result<RequestExplanation> Broker::explain(BandwidthRequestId id, BandwidthRequestGeneration generation) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->explanations.find(request_key(id, generation));
  if (found == impl_->explanations.end()) {
    return make_error<RequestExplanation>(ErrorCode::NotFound, "no committed decision explains that request");
  }
  return found->second;
}

Result<ResourceAccounting> Broker::accounting(const CapacityTarget& target) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->accounting.find(target);
  if (found != impl_->accounting.end()) {
    return found->second;
  }
  // Before the first committed round for a target the view is deliberately
  // empty: every term is zero, which is internally consistent and understates
  // rather than overstates capacity. It becomes meaningful once a round commits.
  const auto capacity = impl_->capacities.find(target);
  if (capacity != impl_->capacities.end()) {
    ResourceAccounting empty;
    empty.target = target;
    empty.capacity_generation = capacity->second.generation;
    empty.evidence = capacity->second.evidence;
    return empty;
  }
  return make_error<ResourceAccounting>(ErrorCode::NotFound,
                                        "no capacity has been published for this target");
}

Result<std::vector<ResourceAccounting>> Broker::accounting_all() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<ResourceAccounting> out;
  for (const auto& entry : impl_->accounting) {
    out.push_back(entry.second);
  }
  for (const auto& entry : impl_->capacities) {
    if (impl_->accounting.count(entry.first) == 0) {
      ResourceAccounting empty;
      empty.target = entry.first;
      empty.capacity_generation = entry.second.generation;
      empty.evidence = entry.second.evidence;
      out.push_back(empty);
    }
  }
  return out;
}

Result<std::vector<CapacityTarget>> Broker::targets() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<CapacityTarget> out;
  for (const auto& entry : impl_->capacities) {
    out.push_back(entry.first);
  }
  return out;
}

CoordinatorStatus Broker::status() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Impl& impl = *impl_;
  CoordinatorStatus status;
  status.epoch = impl.epoch;
  status.incarnation = impl.incarnation;
  status.tick = impl.tick;
  status.audit_sequence = impl.audit_sequence;
  status.policies = impl.policy.has_value() ? 1 : 0;
  status.resources = impl.capacities.size();
  status.obligations = impl.obligations.size();
  status.requests = impl.requests.size();
  status.fences = impl.fences.size();
  status.durable = impl.store != nullptr;
  status.snapshots_written = impl.snapshots_written;
  status.recoveries = impl.recoveries;
  status.journal_truncations = impl.journal_truncations;
  for (const auto& entry : impl.grants) {
    if (entry.second.state == GrantState::RevalidationRequired) {
      ++status.revalidation_required;
    } else if (is_live(entry.second.state)) {
      ++status.live_grants;
    }
  }
  if (impl.store) {
    status.journal_records = impl.store->report().journal_records;
  }
  return status;
}

Status Broker::flush() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!impl_->store) {
    return Status::success();
  }
  return impl_->store->flush();
}

Result<FabricEpoch> Broker::advance_epoch() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Impl& impl = *impl_;
  const auto next = next_fabric_epoch(impl.epoch);
  if (!next.ok()) {
    return next.error();
  }
  Writer writer;
  write_id(writer, next.value());
  BB_RETURN_IF_ERROR(impl.persist(RecordType::EpochAdvanced, writer.take()));
  impl.epoch = next.value();
  impl.tick += 1;
  for (auto& entry : impl.grants) {
    if (is_live(entry.second.state) && entry.second.state != GrantState::RevalidationRequired) {
      entry.second.state = GrantState::RevalidationRequired;
    }
  }
  impl.accounting.clear();
  for (auto& entry : impl.capacities) {
    entry.second.evidence = CapacityEvidenceState::Stale;
  }
  return impl.epoch;
}

}  // namespace bandwidth_broker
