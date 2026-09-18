// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Wire format.
//
// A frame is a fixed 32-byte header followed by a payload:
//
//   offset  size  field
//        0     4  magic 0x42425231 ("BBR1")
//        4     2  protocol version
//        6     2  message type
//        8     4  flags (bit 0 reserved for payload compression; must be zero)
//       12     4  payload length (<= limits::kMaxPayloadBytes)
//       16     8  correlation id
//       24     4  CRC32C over bytes [0,24)
//       28     4  CRC32C over the payload (zero when the payload is empty)
//
// All integers are big endian. Payloads are canonical: fixed-width integers and
// length-prefixed byte strings, with no padding, no map iteration order and no
// pointer values, so encoding is byte-identical on every platform.
//
// Everything that arrives from the network is untrusted. Decoding validates
// every length against a hard bound before allocating, rejects unknown enum
// values, rejects trailing bytes and never reads past the end of a buffer.

#ifndef BANDWIDTH_BROKER_WIRE_HPP
#define BANDWIDTH_BROKER_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bandwidth_broker/accounting.hpp"
#include "bandwidth_broker/arbitrator.hpp"
#include "bandwidth_broker/checksum.hpp"
#include "bandwidth_broker/explanation.hpp"
#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/version.hpp"

namespace bandwidth_broker {

inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kFrameMagic = 0x42425231u;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  PublishCapacity = 3,
  PublishCapacityAck = 4,
  SetPolicy = 5,
  SetPolicyAck = 6,
  SubmitRequests = 7,
  SubmitRequestsAck = 8,
  Arbitrate = 9,
  ArbitrateAck = 10,
  QueryGrant = 11,
  QueryGrantAck = 12,
  QueryAccounting = 13,
  QueryAccountingAck = 14,
  Release = 15,
  ReleaseAck = 16,
  Revoke = 17,
  RevokeAck = 18,
  FencePublisher = 19,
  FencePublisherAck = 20,
  Explain = 21,
  ExplainAck = 22,
  Shutdown = 23,
  ShutdownAck = 24,
  ErrorResponse = 25
};

[[nodiscard]] BB_API const char* to_string(MessageType type) noexcept;
[[nodiscard]] BB_API bool is_known_message_type(std::uint16_t raw) noexcept;

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

struct BB_API FrameHeader final {
  MessageType type{MessageType::ErrorResponse};
  std::uint32_t flags{0};
  std::uint32_t payload_length{0};
  std::uint64_t correlation_id{0};
  std::uint32_t payload_crc{0};
};

BB_API void encode_frame_header(const FrameHeader& header, std::uint8_t out[kFrameHeaderBytes]) noexcept;

// Strict decode. Requires at least kFrameHeaderBytes available. Rejects bad
// magic, unsupported protocol version, unknown message type, reserved flags,
// oversized payload length and a header checksum mismatch.
[[nodiscard]] BB_API Result<FrameHeader> decode_frame_header(const std::uint8_t* in, std::size_t available) noexcept;

[[nodiscard]] BB_API Status verify_payload_crc(const FrameHeader& header,
                                               const std::uint8_t* payload,
                                               std::size_t length) noexcept;

[[nodiscard]] BB_API Result<std::vector<std::uint8_t>> encode_frame(MessageType type,
                                                                    std::uint64_t correlation_id,
                                                                    const std::vector<std::uint8_t>& payload);

// Incremental stream decoder. The buffered-but-incomplete budget is bounded by
// one frame, so a peer cannot make the decoder allocate without bound.
class BB_API FrameDecoder final {
 public:
  FrameDecoder() = default;

  [[nodiscard]] Status feed(const std::uint8_t* data, std::size_t length);
  // Returns false when no complete frame is buffered.
  [[nodiscard]] Result<bool> next(MessageType& type, std::uint64_t& correlation_id, std::vector<std::uint8_t>& payload);

  [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size() - read_offset_; }
  void reset() noexcept;

 private:
  std::vector<std::uint8_t> buffer_{};
  std::size_t read_offset_{0};
  bool failed_{false};
};

// ---------------------------------------------------------------------------
// Canonical payload codec
// ---------------------------------------------------------------------------

class BB_API Writer final {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void bytes(const std::uint8_t* data, std::size_t length);
  void text(std::string_view value);
  void bandwidth(Bandwidth value) { u64(static_cast<std::uint64_t>(value.bits_per_second())); }

  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(data_); }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

 private:
  std::vector<std::uint8_t> data_{};
};

class BB_API Reader final {
 public:
  Reader(const std::uint8_t* data, std::size_t length) noexcept : data_(data), size_(length) {}
  explicit Reader(const std::vector<std::uint8_t>& data) noexcept : data_(data.data()), size_(data.size()) {}

  [[nodiscard]] Result<std::uint8_t> u8();
  [[nodiscard]] Result<std::uint16_t> u16();
  [[nodiscard]] Result<std::uint32_t> u32();
  [[nodiscard]] Result<std::uint64_t> u64();
  [[nodiscard]] Result<bool> boolean();
  [[nodiscard]] Result<std::string_view> bytes(std::size_t max_bytes);
  [[nodiscard]] Result<std::string_view> text(std::size_t max_bytes);
  [[nodiscard]] Result<Bandwidth> bandwidth();

  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] Status expect_end() const;

 private:
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t offset_{0};
};

// Identity and generation helpers shared by every codec below.
//
// Id 0 and generation 0 encode "absent". Several model fields are legitimately
// absent - a requester-claimed authority carries no coordinator incarnation or
// capacity generation, and a request need not bind a reservation - so decoding
// preserves the raw value and the containing model's validate() decides whether
// the field was required. Nothing is invented and nothing is silently dropped.
template <typename Tag>
void write_id(Writer& writer, Id<Tag> id) {
  writer.u64(id.value());
}

template <typename Tag>
[[nodiscard]] Result<Id<Tag>> read_id(Reader& reader) {
  const auto raw = reader.u64();
  if (!raw.ok()) {
    return raw.error();
  }
  return Id<Tag>::from_value(raw.value());
}

template <typename Tag>
void write_generation(Writer& writer, Generation<Tag> generation) {
  writer.u64(generation.value());
}

template <typename Tag>
[[nodiscard]] Result<Generation<Tag>> read_generation(Reader& reader) {
  const auto raw = reader.u64();
  if (!raw.ok()) {
    return raw.error();
  }
  return Generation<Tag>::from_value(raw.value());
}

// ---------------------------------------------------------------------------
// Model codecs
// ---------------------------------------------------------------------------

BB_API void encode(Writer& writer, const BootId& value);
BB_API Result<BootId> decode_boot_id(Reader& reader);
BB_API void encode(Writer& writer, const Provenance& value);
BB_API Result<Provenance> decode_provenance(Reader& reader);
BB_API void encode(Writer& writer, const AuthorityVector& value);
BB_API Result<AuthorityVector> decode_authority(Reader& reader);
BB_API void encode(Writer& writer, const CapacityTarget& value);
BB_API Result<CapacityTarget> decode_capacity_target(Reader& reader);
BB_API void encode(Writer& writer, const CapacitySnapshot& value);
BB_API Result<CapacitySnapshot> decode_capacity_snapshot(Reader& reader);
BB_API void encode(Writer& writer, const Policy& value);
BB_API Result<Policy> decode_policy(Reader& reader);
BB_API void encode(Writer& writer, const Obligation& value);
BB_API Result<Obligation> decode_obligation(Reader& reader);
BB_API void encode(Writer& writer, const BandwidthRequest& value);
BB_API Result<BandwidthRequest> decode_request(Reader& reader);
BB_API void encode(Writer& writer, const GrantAllocation& value);
BB_API Result<GrantAllocation> decode_grant_allocation(Reader& reader);
BB_API void encode(Writer& writer, const RecallRecord& value);
BB_API Result<RecallRecord> decode_recall_record(Reader& reader);
BB_API void encode(Writer& writer, const Grant& value);
BB_API Result<Grant> decode_grant(Reader& reader);
BB_API void encode(Writer& writer, const ResourceAccounting& value);
BB_API Result<ResourceAccounting> decode_accounting(Reader& reader);
BB_API void encode(Writer& writer, const ArbitrationDecision& value);
BB_API Result<ArbitrationDecision> decode_arbitration_decision(Reader& reader);
BB_API void encode(Writer& writer, const RequestExplanation& value);
BB_API Result<RequestExplanation> decode_explanation(Reader& reader);

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

struct BB_API WireStatus final {
  ErrorCode code{ErrorCode::Ok};
  std::string message;
};

BB_API void encode(Writer& writer, const WireStatus& value);
BB_API Result<WireStatus> decode_wire_status(Reader& reader);
[[nodiscard]] BB_API WireStatus to_wire_status(const Status& status);

struct BB_API HelloRequest final {
  PublisherId publisher{};
  BootId boot{};
  std::uint16_t protocol_version{0};
  std::string session_token;
  std::uint64_t nonce{0};
};

struct BB_API HelloAck final {
  WireStatus status{};
  CoordinatorIncarnation coordinator{};
  FabricEpoch epoch{};
  SessionId session{};
  std::uint64_t next_grant_id{1};
};

struct BB_API PublishCapacityRequest final {
  CapacitySnapshot snapshot{};
};

struct BB_API PublishCapacityAck final {
  WireStatus status{};
  CapacitySnapshotGeneration generation{};
};

struct BB_API SetPolicyRequest final {
  Policy policy{};
};

struct BB_API SetPolicyAck final {
  WireStatus status{};
  PolicyGeneration generation{};
};

struct BB_API SubmitRequestsRequest final {
  std::vector<BandwidthRequest> requests{};
};

struct BB_API SubmitRequestsAck final {
  WireStatus status{};
  std::uint32_t accepted{0};
  std::vector<BandwidthRequestId> refused{};
};

struct BB_API ArbitrateRequest final {
  CapacityTarget target{};
};

struct BB_API ArbitrateAck final {
  WireStatus status{};
  DecisionId decision{};
  ResourceAccounting accounting{};
  std::vector<ArbitrationDecision> decisions{};
};

struct BB_API QueryGrantRequest final {
  BandwidthGrantId grant{};
  BandwidthGrantGeneration generation{};
};

struct BB_API QueryGrantAck final {
  WireStatus status{};
  bool found{false};
  Grant grant{};
};

struct BB_API QueryAccountingRequest final {
  CapacityTarget target{};
};

struct BB_API QueryAccountingAck final {
  WireStatus status{};
  ResourceAccounting accounting{};
};

struct BB_API ReleaseRequest final {
  BandwidthGrantId grant{};
  BandwidthGrantGeneration generation{};
};

struct BB_API ReleaseAck final {
  WireStatus status{};
  GrantState state{GrantState::Released};
  bool already_released{false};
  Bandwidth released{};
};

struct BB_API RevokeRequest final {
  BandwidthGrantId grant{};
  BandwidthGrantGeneration generation{};
  std::string reason;
};

struct BB_API RevokeAck final {
  WireStatus status{};
  GrantState state{GrantState::Revoked};
};

struct BB_API FencePublisherRequest final {
  PublisherId publisher{};
  BootId boot{};
  std::string reason;
};

struct BB_API FencePublisherAck final {
  WireStatus status{};
  bool already_fenced{false};
};

struct BB_API ExplainRequest final {
  BandwidthRequestId request{};
  BandwidthRequestGeneration generation{};
};

struct BB_API ExplainAck final {
  WireStatus status{};
  RequestExplanation explanation{};
};

struct BB_API ShutdownRequest final {
  std::string reason;
};

struct BB_API ShutdownAck final {
  WireStatus status{};
};

struct BB_API ErrorResponse final {
  WireStatus status{};
};

// Maps a message type to its encoder/decoder pair so that callers can write
// generic send/receive helpers without naming the decoder themselves.
template <typename T>
struct MessageCodec;

// Encodes a value into a payload and back. Decoding rejects trailing bytes, so
// a smuggled suffix can never be mistaken for a valid message.
template <typename T>
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_payload(const T& value) {
  Writer writer;
  MessageCodec<T>::encode(writer, value);
  return writer.take();
}

template <typename T>
[[nodiscard]] Result<T> decode_payload(const std::vector<std::uint8_t>& payload) {
  Reader reader(payload);
  auto decoded = MessageCodec<T>::decode(reader);
  if (!decoded.ok()) {
    return decoded.error();
  }
  const auto end = reader.expect_end();
  if (!end.ok()) {
    return end.error();
  }
  return decoded.take();
}

// Each message has a distinctly named decoder: a decoder cannot be selected by
// return type alone, and the explicit name keeps call sites unambiguous.
#define BB_DECLARE_MESSAGE_CODEC(Type, Decoder)                     \
  BB_API void encode(Writer& writer, const Type& value);            \
  BB_API Result<Type> Decoder(Reader& reader);                      \
  template <>                                                       \
  struct MessageCodec<Type> {                                       \
    static void encode(Writer& writer, const Type& value) { ::bandwidth_broker::encode(writer, value); } \
    static Result<Type> decode(Reader& reader) { return ::bandwidth_broker::Decoder(reader); } \
  };

BB_DECLARE_MESSAGE_CODEC(HelloRequest, decode_hello_request)
BB_DECLARE_MESSAGE_CODEC(HelloAck, decode_hello_ack)
BB_DECLARE_MESSAGE_CODEC(PublishCapacityRequest, decode_publish_capacity_request)
BB_DECLARE_MESSAGE_CODEC(PublishCapacityAck, decode_publish_capacity_ack)
BB_DECLARE_MESSAGE_CODEC(SetPolicyRequest, decode_set_policy_request)
BB_DECLARE_MESSAGE_CODEC(SetPolicyAck, decode_set_policy_ack)
BB_DECLARE_MESSAGE_CODEC(SubmitRequestsRequest, decode_submit_requests_request)
BB_DECLARE_MESSAGE_CODEC(SubmitRequestsAck, decode_submit_requests_ack)
BB_DECLARE_MESSAGE_CODEC(ArbitrateRequest, decode_arbitrate_request)
BB_DECLARE_MESSAGE_CODEC(ArbitrateAck, decode_arbitrate_ack)
BB_DECLARE_MESSAGE_CODEC(QueryGrantRequest, decode_query_grant_request)
BB_DECLARE_MESSAGE_CODEC(QueryGrantAck, decode_query_grant_ack)
BB_DECLARE_MESSAGE_CODEC(QueryAccountingRequest, decode_query_accounting_request)
BB_DECLARE_MESSAGE_CODEC(QueryAccountingAck, decode_query_accounting_ack)
BB_DECLARE_MESSAGE_CODEC(ReleaseRequest, decode_release_request)
BB_DECLARE_MESSAGE_CODEC(ReleaseAck, decode_release_ack)
BB_DECLARE_MESSAGE_CODEC(RevokeRequest, decode_revoke_request)
BB_DECLARE_MESSAGE_CODEC(RevokeAck, decode_revoke_ack)
BB_DECLARE_MESSAGE_CODEC(FencePublisherRequest, decode_fence_publisher_request)
BB_DECLARE_MESSAGE_CODEC(FencePublisherAck, decode_fence_publisher_ack)
BB_DECLARE_MESSAGE_CODEC(ExplainRequest, decode_explain_request)
BB_DECLARE_MESSAGE_CODEC(ExplainAck, decode_explain_ack)
BB_DECLARE_MESSAGE_CODEC(ShutdownRequest, decode_shutdown_request)
BB_DECLARE_MESSAGE_CODEC(ShutdownAck, decode_shutdown_ack)
BB_DECLARE_MESSAGE_CODEC(ErrorResponse, decode_error_response)
#undef BB_DECLARE_MESSAGE_CODEC

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_WIRE_HPP
