// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Hard resource bounds. Every externally influenced dimension of this runtime
// is bounded here so that no input can cause unbounded memory, time or journal
// growth. Bounds are enforced at the boundary that first observes the value.

#ifndef BANDWIDTH_BROKER_LIMITS_HPP
#define BANDWIDTH_BROKER_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace bandwidth_broker {
namespace limits {

// Text and label bounds.
inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr std::size_t kMaxProvenanceDetailBytes = 96;
inline constexpr std::size_t kMaxPolicyLabels = 16;
inline constexpr std::size_t kMaxLabelKeyBytes = 32;
inline constexpr std::size_t kMaxLabelValueBytes = 64;
inline constexpr std::size_t kMaxExplanationTextBytes = 256;

// Policy bounds.
inline constexpr std::size_t kMaxPriorityClasses = 64;
inline constexpr std::size_t kMaxFairnessGroups = 1024;
inline constexpr std::uint64_t kMaxWeight = 1'000'000;
inline constexpr std::uint32_t kMaxPermille = 1000;
inline constexpr std::uint32_t kMaxAgingMultiplier = 64;
inline constexpr std::uint32_t kMaxAgingThresholdRounds = 4096;
inline constexpr std::uint32_t kMaxPromotions = 8;
inline constexpr std::uint32_t kMaxHysteresisRounds = 4096;
inline constexpr std::uint32_t kMaxRecallGraceRounds = 4096;
inline constexpr std::uint32_t kMaxOversubscriptionNumerator = 16;

// Per-round population bounds.
inline constexpr std::size_t kMaxRequestsPerRound = 100'000;
inline constexpr std::size_t kMaxObligationsPerResource = 100'000;
inline constexpr std::size_t kMaxGrantsPerResource = 200'000;
inline constexpr std::size_t kMaxResources = 65'536;
inline constexpr std::size_t kMaxPendingRecalls = 100'000;

// Persistence bounds.
inline constexpr std::size_t kMaxJournalRecords = 1'000'000;
inline constexpr std::size_t kMaxStoreFileBytes = 1ull << 31;   // 2 GiB
inline constexpr std::size_t kMaxSnapshotCount = 2;
inline constexpr std::size_t kMaxIdempotencyEntries = 1'000'000;
inline constexpr std::size_t kMaxAuditRecordsInMemory = 4'096;

// Protocol bounds.
inline constexpr std::size_t kMaxPayloadBytes = 1u << 20;   // 1 MiB
inline constexpr std::size_t kMaxFrameHeaderBytes = 32;
inline constexpr std::size_t kMaxConnections = 128;
inline constexpr std::size_t kMaxOutboundQueueBytes = 4u << 20;
inline constexpr std::size_t kMaxStringBytesInFrame = 4096;
inline constexpr std::size_t kMaxVectorElementsInFrame = 100'000;
inline constexpr std::size_t kMaxNestingDepth = 4;
inline constexpr std::size_t kMaxBootFenceRecords = 100'000;

}  // namespace limits
}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_LIMITS_HPP
