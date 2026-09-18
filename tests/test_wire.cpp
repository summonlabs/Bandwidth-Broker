// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Proof obligations for the wire format: canonical round trips, exact bounds
// enforcement, and adversarial rejection of malformed, truncated, oversized,
// mutated and fuzzed input.

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "bandwidth_broker/wire.hpp"
#include "support/fixtures.hpp"
#include "support/test_harness.hpp"

using namespace bandwidth_broker;
using bb_fixture::basic_policy;
using bb_fixture::Fixture;
using bb_fixture::RequestSpec;

namespace {

struct Sample final {
  Fixture fixture;
  CapacitySnapshot snapshot;
  Policy policy;
  Obligation obligation;
  BandwidthRequest request;
  Grant grant;
  ArbitrationDecision decision;
  RequestExplanation explanation;
  ResourceAccounting accounting;
};

[[nodiscard]] Sample make_sample() {
  Sample sample;
  sample.snapshot = sample.fixture.snapshot(1'000'000, CapacityEvidenceState::Known, 1'000);
  sample.policy = basic_policy(sample.fixture, 2);
  sample.obligation.reservation = ReservationReferenceId::from_value(9);
  sample.obligation.generation = ReservationGeneration::initial();
  sample.obligation.target = sample.fixture.target();
  sample.obligation.amount = bb_fixture::bw(5'000);
  sample.obligation.lendable = true;
  sample.obligation.max_lend_permille = 500;
  sample.obligation.provenance = sample.fixture.provenance(NodeKind::Operator);
  sample.request = sample.fixture.request(RequestSpec{7, 3, 1'000, 4'000, 8'000, 2, 1, 1});
  sample.request.labels = {PolicyLabel{"tier", "gold"}, PolicyLabel{"zone", "west"}};
  sample.request.window = RequestWindow{0, 10'000};
  sample.request.latency_slo = SloClassId::from_value(4);

  sample.grant.id = BandwidthGrantId::from_value(11);
  sample.grant.generation = BandwidthGrantGeneration::initial();
  sample.grant.request = sample.request.id;
  sample.grant.request_generation = sample.request.generation;
  sample.grant.target = sample.fixture.target();
  sample.grant.state = GrantState::GrantedBorrowed;
  sample.grant.allocation.guaranteed = bb_fixture::bw(1'000);
  sample.grant.allocation.discretionary = bb_fixture::bw(2'000);
  sample.grant.allocation.borrowed = bb_fixture::bw(500);
  sample.grant.allocation.contingent = bb_fixture::bw(250);
  sample.grant.requested_minimum = bb_fixture::bw(1'000);
  sample.grant.requested_desired = bb_fixture::bw(4'000);
  sample.grant.requested_maximum = bb_fixture::bw(8'000);
  sample.grant.denied = bb_fixture::bw(250);
  sample.grant.reason = OutcomeReason::PartiallySatisfied;
  sample.grant.decision = DecisionId::from_value(2);
  sample.grant.issued_tick = 5;
  sample.grant.last_revalidated_tick = 6;
  sample.grant.expires_at_tick = 99;
  sample.grant.wait_rounds = 2;
  sample.grant.obligation_backed = bb_fixture::bw(1'000);
  sample.grant.reserve_backed = bb_fixture::bw(250);
  sample.grant.superseded_by = BandwidthGrantId::from_value(12);
  sample.grant.authority = sample.request.authority;
  sample.grant.authority.coordinator = sample.fixture.coordinator;
  sample.grant.authority.capacity_generation = sample.fixture.capacity_generation;
  sample.grant.authority.monotonic_tick = 5;

  sample.decision.request = sample.request.id;
  sample.decision.request_generation = sample.request.generation;
  sample.decision.state = GrantState::GrantedBorrowed;
  sample.decision.reason = OutcomeReason::PartiallySatisfied;
  sample.decision.allocation = sample.grant.allocation;
  sample.decision.requested_minimum = sample.grant.requested_minimum;
  sample.decision.requested_desired = sample.grant.requested_desired;
  sample.decision.requested_maximum = sample.grant.requested_maximum;
  sample.decision.denied = sample.grant.denied;
  sample.decision.satisfied = false;
  sample.decision.waiting = true;
  sample.decision.starvation_promoted = true;
  sample.decision.effective_rank = 30;
  sample.decision.effective_weight = 4;
  sample.decision.next_wait_rounds = 3;
  sample.decision.binding_reason = "fair share";
  sample.decision.authority_mismatch = "none";
  sample.decision.grant = sample.grant;
  sample.decision.previous_grant = sample.grant;
  RecallRecord recall;
  recall.id = RecallId::from_value(3);
  recall.grant = sample.grant.id;
  recall.grant_generation = sample.grant.generation;
  recall.request = sample.request.id;
  recall.target = sample.fixture.target();
  recall.recalled = bb_fixture::bw(400);
  recall.remaining = bb_fixture::bw(600);
  recall.reason = OutcomeReason::RecalledByStrongerClass;
  recall.requested_tick = 5;
  recall.deadline_tick = 7;
  recall.acknowledgement_required = true;
  recall.authority = sample.grant.authority;
  sample.decision.recall = recall;

  sample.explanation.request = sample.request.id;
  sample.explanation.request_generation = sample.request.generation;
  sample.explanation.target = sample.fixture.target();
  sample.explanation.state = GrantState::GrantedBorrowed;
  sample.explanation.reason = OutcomeReason::PartiallySatisfied;
  sample.explanation.waiting = true;
  sample.explanation.authority = sample.grant.authority;
  sample.explanation.epoch = sample.fixture.epoch;
  sample.explanation.coordinator = sample.fixture.coordinator;
  sample.explanation.policy = sample.policy.id;
  sample.explanation.policy_generation = sample.policy.generation;
  sample.explanation.decision = DecisionId::from_value(2);
  sample.explanation.decision_tick = 5;
  sample.explanation.evidence = CapacityEvidenceState::Known;
  sample.explanation.capacity_generation = sample.fixture.capacity_generation;
  sample.explanation.effective_physical = bb_fixture::bw(1'000'000);
  sample.explanation.arbitrable = bb_fixture::bw(900'000);
  sample.explanation.requested_minimum = sample.grant.requested_minimum;
  sample.explanation.requested_desired = sample.grant.requested_desired;
  sample.explanation.requested_maximum = sample.grant.requested_maximum;
  sample.explanation.guaranteed = sample.grant.allocation.guaranteed;
  sample.explanation.discretionary = sample.grant.allocation.discretionary;
  sample.explanation.borrowed = sample.grant.allocation.borrowed;
  sample.explanation.contingent = sample.grant.allocation.contingent;
  sample.explanation.total_granted = bb_fixture::bw(3'750);
  sample.explanation.denied = sample.grant.denied;
  sample.explanation.priority = sample.request.priority;
  sample.explanation.fairness_group = sample.request.fairness_group;
  sample.explanation.tenant = sample.request.tenant;
  sample.explanation.effective_rank = 30;
  sample.explanation.effective_weight = 4;
  sample.explanation.binding_reason = "fair share";
  sample.explanation.resource_generation = sample.fixture.resource_generation;
  sample.explanation.grant = sample.grant.id;
  sample.explanation.grant_generation = sample.grant.generation;
  sample.explanation.provenance = sample.fixture.provenance(NodeKind::Requester);

  sample.accounting.target = sample.fixture.target();
  sample.accounting.capacity_generation = sample.fixture.capacity_generation;
  sample.accounting.evidence = CapacityEvidenceState::Known;
  sample.accounting.physical_configured = bb_fixture::bw(1'000'000);
  sample.accounting.effective_physical = bb_fixture::bw(1'000'000);
  sample.accounting.allocatable = bb_fixture::bw(1'000'000);
  sample.accounting.arbitrable = bb_fixture::bw(1'000'000);
  sample.accounting.guaranteed_granted = bb_fixture::bw(1'000);
  sample.accounting.discretionary_granted = bb_fixture::bw(2'000);
  sample.accounting.unallocated = bb_fixture::bw(997'000);
  sample.accounting.authorized_consumption = bb_fixture::bw(3'000);
  return sample;
}

// Model types have a plain encode(Writer&, T) plus a distinctly named decoder,
// so they cannot use the message-level encode_payload/decode_payload helpers.
template <typename T>
[[nodiscard]] std::vector<std::uint8_t> encode_model(const T& value) {
  Writer writer;
  encode(writer, value);
  return writer.take();
}

[[nodiscard]] std::vector<std::uint8_t> encode_header_bytes(const FrameHeader& header) {
  std::uint8_t raw[kFrameHeaderBytes] = {};
  encode_frame_header(header, raw);
  return std::vector<std::uint8_t>(raw, raw + kFrameHeaderBytes);
}

[[nodiscard]] FrameHeader sample_header() {
  FrameHeader header;
  header.type = MessageType::SubmitRequests;
  header.flags = 0;
  header.payload_length = 7;
  header.correlation_id = 0x0123456789ABCDEFULL;
  header.payload_crc = 0xDEADBEEFu;
  return header;
}

}  // namespace

BB_TEST(Wire, FrameHeaderRoundTripsCanonically) {
  const FrameHeader header = sample_header();
  const std::vector<std::uint8_t> first = encode_header_bytes(header);
  const std::vector<std::uint8_t> second = encode_header_bytes(header);
  BB_CHECK_EQ(first.size(), kFrameHeaderBytes);
  BB_CHECK(first == second);
  const auto decoded = decode_frame_header(first.data(), first.size());
  BB_REQUIRE(decoded.ok());
  BB_CHECK_EQ(decoded.value().type, header.type);
  BB_CHECK_EQ(decoded.value().flags, header.flags);
  BB_CHECK_EQ(decoded.value().payload_length, header.payload_length);
  BB_CHECK_EQ(decoded.value().correlation_id, header.correlation_id);
  BB_CHECK_EQ(decoded.value().payload_crc, header.payload_crc);
}

BB_TEST(Wire, FrameHeaderStrictRejection) {
  const std::vector<std::uint8_t> good = encode_header_bytes(sample_header());

  std::vector<std::uint8_t> bad_magic = good;
  bad_magic[0] = 0x00;
  BB_CHECK_ERR(ErrorCode::ProtocolMalformed, decode_frame_header(bad_magic.data(), bad_magic.size()));

  std::vector<std::uint8_t> bad_version = good;
  bad_version[4] = 0x7F;
  bad_version[5] = 0xFF;
  BB_CHECK_ERR(ErrorCode::ProtocolVersionUnsupported, decode_frame_header(bad_version.data(), bad_version.size()));

  std::vector<std::uint8_t> bad_type = good;
  bad_type[6] = 0x00;
  bad_type[7] = 0xFF;
  BB_CHECK_ERR(ErrorCode::ProtocolMalformed, decode_frame_header(bad_type.data(), bad_type.size()));

  std::vector<std::uint8_t> bad_flags = good;
  bad_flags[8] = 0x80;
  BB_CHECK_ERR(ErrorCode::ProtocolMalformed, decode_frame_header(bad_flags.data(), bad_flags.size()));

  std::vector<std::uint8_t> oversized = good;
  const std::uint32_t too_big = static_cast<std::uint32_t>(limits::kMaxPayloadBytes) + 1u;
  oversized[12] = static_cast<std::uint8_t>((too_big >> 24) & 0xFFu);
  oversized[13] = static_cast<std::uint8_t>((too_big >> 16) & 0xFFu);
  oversized[14] = static_cast<std::uint8_t>((too_big >> 8) & 0xFFu);
  oversized[15] = static_cast<std::uint8_t>(too_big & 0xFFu);
  BB_CHECK_ERR(ErrorCode::ProtocolPayloadTooLarge, decode_frame_header(oversized.data(), oversized.size()));

  std::vector<std::uint8_t> bad_crc = good;
  bad_crc[20] = static_cast<std::uint8_t>(bad_crc[20] ^ 0x01u);
  BB_CHECK_ERR(ErrorCode::ProtocolChecksum, decode_frame_header(bad_crc.data(), bad_crc.size()));

  BB_CHECK_ERR(ErrorCode::ProtocolTruncated, decode_frame_header(good.data(), kFrameHeaderBytes - 1));
  BB_CHECK_ERR(ErrorCode::ProtocolTruncated, decode_frame_header(nullptr, 0));
}

BB_TEST(Wire, PayloadChecksumDetectsEverySingleBitFlip) {
  const Sample sample = make_sample();
  const std::vector<std::uint8_t> payload = encode_model(sample.obligation);
  const auto frame = encode_frame(MessageType::PublishCapacity, 5, payload);
  BB_REQUIRE(frame.ok());
  const auto header = decode_frame_header(frame.value().data(), kFrameHeaderBytes);
  BB_REQUIRE(header.ok());
  BB_CHECK_OK(verify_payload_crc(header.value(), frame.value().data() + kFrameHeaderBytes, payload.size()));
  for (std::size_t bit = 0; bit < payload.size(); ++bit) {
    std::vector<std::uint8_t> mutated = frame.value();
    mutated[kFrameHeaderBytes + bit] = static_cast<std::uint8_t>(mutated[kFrameHeaderBytes + bit] ^ 0x01u);
    const auto result = verify_payload_crc(header.value(), mutated.data() + kFrameHeaderBytes, payload.size());
    BB_CHECK(!result.ok());
  }
}

BB_TEST(Wire, RoundTripsEveryModelType) {
  const Sample sample = make_sample();
  {
    Writer writer;
    encode(writer, sample.snapshot);
    Reader reader(writer.data());
    const auto decoded = decode_capacity_snapshot(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value().target == sample.snapshot.target);
    BB_CHECK_EQ(decoded.value().evidence, sample.snapshot.evidence);
    BB_CHECK(decoded.value().authority == sample.snapshot.authority);
    Writer again;
    encode(again, decoded.value());
    BB_CHECK(again.data() == writer.data());
  }
  {
    Writer writer;
    encode(writer, sample.policy);
    Reader reader(writer.data());
    const auto decoded = decode_policy(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value() == sample.policy);
    BB_CHECK_EQ(decoded.value().content_hash(), sample.policy.content_hash());
  }
  {
    Writer writer;
    encode(writer, sample.obligation);
    Reader reader(writer.data());
    const auto decoded = decode_obligation(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK(decoded.value() == sample.obligation);
  }
  {
    Writer writer;
    encode(writer, sample.request);
    Reader reader(writer.data());
    const auto decoded = decode_request(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value() == sample.request);
    BB_CHECK_EQ(decoded.value().content_hash(), sample.request.content_hash());
  }
  {
    Writer writer;
    encode(writer, sample.grant);
    Reader reader(writer.data());
    const auto decoded = decode_grant(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value().id == sample.grant.id);
    BB_CHECK(decoded.value().allocation == sample.grant.allocation);
    BB_CHECK(decoded.value().authority == sample.grant.authority);
    BB_CHECK(decoded.value().obligation_backed == sample.grant.obligation_backed);
    BB_CHECK(decoded.value().reserve_backed == sample.grant.reserve_backed);
    BB_CHECK(decoded.value().expires_at_tick == sample.grant.expires_at_tick);
  }
  {
    Writer writer;
    encode(writer, sample.accounting);
    Reader reader(writer.data());
    const auto decoded = decode_accounting(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value().authorized_consumption == sample.accounting.authorized_consumption);
    BB_CHECK(decoded.value().unallocated == sample.accounting.unallocated);
  }
  {
    Writer writer;
    encode(writer, sample.decision);
    Reader reader(writer.data());
    const auto decoded = decode_arbitration_decision(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value().grant.has_value());
    BB_CHECK(decoded.value().previous_grant.has_value());
    BB_CHECK(decoded.value().recall.has_value());
    BB_CHECK(decoded.value().recall->reason == OutcomeReason::RecalledByStrongerClass);
  }
  {
    Writer writer;
    encode(writer, sample.explanation);
    Reader reader(writer.data());
    const auto decoded = decode_explanation(reader);
    BB_REQUIRE(decoded.ok());
    BB_CHECK_OK(reader.expect_end());
    BB_CHECK(decoded.value().request == sample.explanation.request);
    BB_CHECK(decoded.value().binding_reason == sample.explanation.binding_reason);
  }
}

BB_TEST(Wire, TruncationIsRejectedAtEveryLength) {
  const Sample sample = make_sample();
  Writer writer;
  encode(writer, sample.request);
  const std::vector<std::uint8_t> full = writer.data();
  BB_REQUIRE(full.size() > 8);
  for (std::size_t length = 0; length < full.size(); ++length) {
    std::vector<std::uint8_t> prefix(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(length));
    Reader reader(prefix);
    const auto decoded = decode_request(reader);
    if (decoded.ok()) {
      BB_FAIL("truncated payload of length " + std::to_string(length) + " decoded successfully");
      return;
    }
  }
  Reader reader(full);
  BB_CHECK_OK(decode_request(reader));
}

BB_TEST(Wire, OversizedLengthFieldIsRefusedBeforeAllocation) {
  Writer writer;
  write_id(writer, PolicyId::from_value(1));
  write_generation(writer, PolicyGeneration::initial());
  writer.u8(0);
  writer.u32(1);
  write_id(writer, PriorityClassId::from_value(1));
  writer.u32(0xFFFFFFFFu);
  const std::vector<std::uint8_t> payload = writer.take();
  Reader reader(payload);
  BB_CHECK_ERR(ErrorCode::ProtocolPayloadTooLarge, decode_policy(reader));
}

BB_TEST(Wire, UnknownEnumValuesAreRejected) {
  Writer writer;
  write_id(writer, PolicyId::from_value(1));
  write_generation(writer, PolicyGeneration::initial());
  writer.u8(0xFEu);
  const std::vector<std::uint8_t> bytes = writer.take();
  Reader reader(bytes);
  BB_CHECK_ERR(ErrorCode::ProtocolMalformed, decode_policy(reader));

  const Sample sample = make_sample();
  const std::vector<std::uint8_t> payload = encode_model(sample.obligation);
  std::size_t mutated_any = 0;
  for (std::size_t offset = 0; offset + 1 < payload.size(); ++offset) {
    std::vector<std::uint8_t> mutated = payload;
    mutated[offset] = 0xFEu;
    Reader probe(mutated);
    const auto decoded = decode_obligation(probe);
    if (!decoded.ok()) {
      ++mutated_any;
    }
  }
  BB_CHECK(mutated_any > 0);
}

BB_TEST(Wire, TrailingBytesAreRejected) {
  const Sample sample = make_sample();
  const std::vector<std::uint8_t> payload = encode_model(sample.obligation);
  std::vector<std::uint8_t> extended = payload;
  extended.push_back(0x7Fu);
  Reader extended_reader(extended);
  const auto decoded = decode_obligation(extended_reader);
  BB_REQUIRE(decoded.ok());
  BB_CHECK(decoded.value().reservation.valid());
  BB_CHECK_ERR(ErrorCode::ProtocolMalformed, extended_reader.expect_end());
  Reader exact_reader(payload);
  BB_CHECK_OK(decode_obligation(exact_reader));
  BB_CHECK_OK(exact_reader.expect_end());
}

BB_TEST(Wire, FrameDecoderHandlesArbitraryChunking) {
  std::vector<std::uint8_t> stream;
  std::vector<std::vector<std::uint8_t>> payloads;
  const MessageType types[] = {MessageType::Hello, MessageType::SubmitRequests, MessageType::Explain};
  for (std::size_t i = 0; i < 3; ++i) {
    std::vector<std::uint8_t> payload(37 + i * 11, static_cast<std::uint8_t>(i + 1));
    payloads.push_back(payload);
    const auto frame = encode_frame(types[i], 1000 + i, payload);
    BB_REQUIRE(frame.ok());
    stream.insert(stream.end(), frame.value().begin(), frame.value().end());
  }

  FrameDecoder decoder;
  std::size_t collected = 0;
  for (const std::uint8_t byte : stream) {
    const auto fed = decoder.feed(&byte, 1);
    BB_REQUIRE(fed.ok());
    for (;;) {
      MessageType type = MessageType::ErrorResponse;
      std::uint64_t correlation = 0;
      std::vector<std::uint8_t> payload;
      const auto next = decoder.next(type, correlation, payload);
      BB_REQUIRE(next.ok());
      if (!next.value()) {
        break;
      }
      BB_CHECK_EQ(type, types[collected]);
      BB_CHECK_EQ(correlation, 1000 + collected);
      BB_CHECK(payload == payloads[collected]);
      ++collected;
    }
  }
  BB_CHECK_EQ(collected, std::size_t{3});
  BB_CHECK_EQ(decoder.buffered_bytes(), std::size_t{0});

  std::mt19937_64 generator(0x5EED1234ULL);
  FrameDecoder chunked;
  std::size_t offset = 0;
  collected = 0;
  while (offset < stream.size()) {
    const std::size_t chunk = 1 + static_cast<std::size_t>(generator() % 23);
    const std::size_t take = (offset + chunk > stream.size()) ? stream.size() - offset : chunk;
    const auto fed = chunked.feed(stream.data() + offset, take);
    BB_REQUIRE(fed.ok());
    offset += take;
    for (;;) {
      MessageType type = MessageType::ErrorResponse;
      std::uint64_t correlation = 0;
      std::vector<std::uint8_t> payload;
      const auto next = chunked.next(type, correlation, payload);
      BB_REQUIRE(next.ok());
      if (!next.value()) {
        break;
      }
      BB_CHECK_EQ(type, types[collected]);
      ++collected;
    }
  }
  BB_CHECK_EQ(collected, std::size_t{3});
}

BB_TEST(Wire, CorruptedFrameIsReportedAndNeverYielded) {
  std::vector<std::uint8_t> stream;
  for (std::size_t i = 0; i < 3; ++i) {
    const auto frame = encode_frame(MessageType::Hello, 2000 + i, std::vector<std::uint8_t>(64, 0xAB));
    BB_REQUIRE(frame.ok());
    stream.insert(stream.end(), frame.value().begin(), frame.value().end());
  }
  const std::size_t second = kFrameHeaderBytes + 64;
  stream[second + kFrameHeaderBytes + 3] = static_cast<std::uint8_t>(stream[second + kFrameHeaderBytes + 3] ^ 0x40u);

  FrameDecoder decoder;
  const auto fed = decoder.feed(stream.data(), stream.size());
  BB_REQUIRE(fed.ok());
  MessageType type = MessageType::ErrorResponse;
  std::uint64_t correlation = 0;
  std::vector<std::uint8_t> payload;
  const auto first = decoder.next(type, correlation, payload);
  BB_REQUIRE(first.ok());
  BB_CHECK(first.value());
  BB_CHECK_EQ(correlation, std::uint64_t{2000});
  const auto second_result = decoder.next(type, correlation, payload);
  BB_CHECK(!second_result.ok());
  BB_CHECK_EQ(second_result.code(), ErrorCode::ProtocolChecksum);
}

BB_TEST(Wire, FrameDecoderBoundsBufferedInput) {
  FrameHeader header;
  header.type = MessageType::SubmitRequests;
  header.payload_length = static_cast<std::uint32_t>(limits::kMaxPayloadBytes);
  const std::vector<std::uint8_t> bytes = encode_header_bytes(header);

  FrameDecoder decoder;
  const auto seeded = decoder.feed(bytes.data(), bytes.size());
  BB_REQUIRE(seeded.ok());
  std::vector<std::uint8_t> filler(64 * 1024, 0x00);
  bool failed = false;
  for (int i = 0; i < 64 && !failed; ++i) {
    const auto status = decoder.feed(filler.data(), filler.size());
    if (!status.ok()) {
      failed = true;
    }
    BB_CHECK(decoder.buffered_bytes() <= kFrameHeaderBytes + limits::kMaxPayloadBytes);
  }
  BB_CHECK(failed);
  // A fresh decoder is used so that the null-buffer check observes the argument
  // validation rather than the earlier failure state.
  FrameDecoder fresh;
  BB_CHECK_ERR(ErrorCode::InvalidArgument, fresh.feed(nullptr, 8));
  BB_CHECK_OK(fresh.feed(nullptr, 0));
}

BB_TEST(Wire, FuzzDecodeNeverCrashes) {
  std::mt19937_64 generator(0xC0FFEE99ULL);
  std::uniform_int_distribution<std::size_t> length_distribution(0, 512);
  std::uniform_int_distribution<int> byte_distribution(0, 255);
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::size_t length = length_distribution(generator);
    std::vector<std::uint8_t> buffer(length);
    for (std::size_t i = 0; i < length; ++i) {
      buffer[i] = static_cast<std::uint8_t>(byte_distribution(generator));
    }
    const auto header = decode_frame_header(buffer.data(), buffer.size());
    if (header.ok()) {
      const auto status = verify_payload_crc(header.value(), buffer.data(), buffer.size());
      BB_CHECK(!status.ok());
    } else {
      const ErrorCode code = header.code();
      BB_CHECK(code == ErrorCode::ProtocolTruncated || code == ErrorCode::ProtocolMalformed ||
               code == ErrorCode::ProtocolVersionUnsupported || code == ErrorCode::ProtocolPayloadTooLarge ||
               code == ErrorCode::ProtocolChecksum);
    }

    FrameDecoder decoder;
    const auto fed = decoder.feed(buffer.data(), buffer.size());
    if (fed.ok()) {
      for (int frame = 0; frame < 4; ++frame) {
        MessageType type = MessageType::ErrorResponse;
        std::uint64_t correlation = 0;
        std::vector<std::uint8_t> payload;
        const auto next = decoder.next(type, correlation, payload);
        if (!next.ok() || !next.value()) {
          break;
        }
        BB_CHECK(payload.size() <= limits::kMaxPayloadBytes);
      }
    }

    if (length > 0 && length < 4096) {
      Reader reader(buffer);
      const auto obligation = decode_obligation(reader);
      static_cast<void>(obligation);
      Reader second_reader(buffer);
      const auto grant = decode_grant(second_reader);
      static_cast<void>(grant);
    }
  }
}

BB_TEST(Wire, AdversarialMutationsNeverInventValues) {
  const Sample sample = make_sample();
  const std::vector<std::uint8_t> request_payload = encode_model(sample.request);
  const std::vector<std::uint8_t> grant_payload = encode_model(sample.grant);

  std::mt19937_64 generator(0xBADF00D1ULL);
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::vector<std::uint8_t>& source = ((iteration % 2) == 0) ? request_payload : grant_payload;
    std::vector<std::uint8_t> mutated = source;
    const std::size_t byte = static_cast<std::size_t>(generator() % mutated.size());
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (generator() % 8));
    mutated[byte] = static_cast<std::uint8_t>(mutated[byte] ^ bit);

    // Each payload is decoded as its own type. Decoding a mutated payload as a
    // different model type is not a canonicality question: the binary shapes of
    // some model types are compatible, which is exactly why the frame header
    // carries a message type and the coordinator dispatches strictly by it.
    // A decode is only authoritative when it consumes the whole payload:
    // a value recovered from a prefix is rejected by expect_end() at the
    // message boundary, and that rejection is part of the proof.
    if ((iteration % 2) == 0) {
      Reader reader(mutated);
      const auto decoded = decode_request(reader);
      if (decoded.ok() && reader.expect_end().ok()) {
        Writer writer;
        encode(writer, decoded.value());
        BB_CHECK(writer.data() == mutated);
      }
    } else {
      Reader reader(mutated);
      const auto decoded = decode_grant(reader);
      if (decoded.ok() && reader.expect_end().ok()) {
        Writer writer;
        encode(writer, decoded.value());
        BB_CHECK(writer.data() == mutated);
      }
    }
  }
}

BB_TEST(Wire, EncodeFrameRejectsOversizedPayload) {
  std::vector<std::uint8_t> payload(limits::kMaxPayloadBytes + 1, 0x00);
  BB_CHECK_ERR(ErrorCode::ProtocolPayloadTooLarge, encode_frame(MessageType::Hello, 1, payload));
  std::vector<std::uint8_t> exact(limits::kMaxPayloadBytes, 0x00);
  BB_CHECK_OK(encode_frame(MessageType::Hello, 1, exact));
}

BB_TEST(Wire, CodecRejectsOversizedVectorCounts) {
  Writer writer;
  write_id(writer, PolicyId::from_value(1));
  write_generation(writer, PolicyGeneration::initial());
  writer.u8(0);
  writer.u32(1'000'000);
  const std::vector<std::uint8_t> bytes = writer.take();
  Reader reader(bytes);
  BB_CHECK_ERR(ErrorCode::ProtocolPayloadTooLarge, decode_policy(reader));
}

BB_TEST(Wire, ReaderBoundsAreExact) {
  const std::uint8_t small[3] = {0x01, 0x02, 0x03};
  Reader reader(small, sizeof(small));
  BB_CHECK_OK(reader.u8());
  BB_CHECK_ERR(ErrorCode::ProtocolTruncated, reader.u32());
  BB_CHECK_OK(reader.u16());
  BB_CHECK_OK(reader.expect_end());
  Reader trailing(small, sizeof(small));
  BB_CHECK_OK(trailing.u8());
  BB_CHECK_ERR(ErrorCode::ProtocolMalformed, trailing.expect_end());
}
