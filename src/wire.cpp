// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/wire.hpp"

#include <cstring>

#include "bandwidth_broker/numeric.hpp"
#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {
namespace {

constexpr std::uint32_t kAllowedFlags = 0x0u;

void put_u16(std::uint8_t* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[1] = static_cast<std::uint8_t>(value & 0xFFu);
}

void put_u32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * (3 - i))) & 0xFFu);
  }
}

void put_u64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * (7 - i))) & 0xFFu);
  }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* in) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) | in[1]);
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) noexcept {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8) | in[i];
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) noexcept {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | in[i];
  }
  return value;
}

template <typename Enum>
void write_enum(Writer& writer, Enum value) {
  writer.u8(static_cast<std::uint8_t>(value));
}

template <typename Enum>
[[nodiscard]] Result<Enum> read_enum(Reader& reader, std::uint8_t maximum) {
  const auto raw = reader.u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > maximum) {
    return make_error<Enum>(ErrorCode::ProtocolMalformed, "enum value is not defined by this protocol revision");
  }
  return static_cast<Enum>(raw.value());
}

void write_text(Writer& writer, std::string_view text) { writer.text(text); }

[[nodiscard]] Result<std::string> read_text(Reader& reader, std::size_t max_bytes, const char* field) {
  const auto view = reader.text(max_bytes);
  if (!view.ok()) {
    return view.error();
  }
  if (!is_valid_text(view.value())) {
    return make_error<std::string>(ErrorCode::ProtocolMalformed, std::string(field) + " is not valid UTF-8 text");
  }
  return std::string(view.value());
}

[[nodiscard]] Status check_text(std::string_view text, const char* field) {
  if (text.size() > limits::kMaxStringBytesInFrame) {
    return make_error_status(ErrorCode::BoundsExceeded, std::string(field) + " exceeds the frame string limit");
  }
  if (!is_valid_text(text)) {
    return make_error_status(ErrorCode::InvalidArgument, std::string(field) + " is not valid text");
  }
  return Status::success();
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello: return "hello";
    case MessageType::HelloAck: return "hello_ack";
    case MessageType::PublishCapacity: return "publish_capacity";
    case MessageType::PublishCapacityAck: return "publish_capacity_ack";
    case MessageType::SetPolicy: return "set_policy";
    case MessageType::SetPolicyAck: return "set_policy_ack";
    case MessageType::SubmitRequests: return "submit_requests";
    case MessageType::SubmitRequestsAck: return "submit_requests_ack";
    case MessageType::Arbitrate: return "arbitrate";
    case MessageType::ArbitrateAck: return "arbitrate_ack";
    case MessageType::QueryGrant: return "query_grant";
    case MessageType::QueryGrantAck: return "query_grant_ack";
    case MessageType::QueryAccounting: return "query_accounting";
    case MessageType::QueryAccountingAck: return "query_accounting_ack";
    case MessageType::Release: return "release";
    case MessageType::ReleaseAck: return "release_ack";
    case MessageType::Revoke: return "revoke";
    case MessageType::RevokeAck: return "revoke_ack";
    case MessageType::FencePublisher: return "fence_publisher";
    case MessageType::FencePublisherAck: return "fence_publisher_ack";
    case MessageType::Explain: return "explain";
    case MessageType::ExplainAck: return "explain_ack";
    case MessageType::Shutdown: return "shutdown";
    case MessageType::ShutdownAck: return "shutdown_ack";
    case MessageType::ErrorResponse: return "error";
  }
  return "unknown";
}

bool is_known_message_type(std::uint16_t raw) noexcept { return raw >= 1 && raw <= 25; }

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

void encode_frame_header(const FrameHeader& header, std::uint8_t out[kFrameHeaderBytes]) noexcept {
  put_u32(out, kFrameMagic);
  put_u16(out + 4, static_cast<std::uint16_t>(BB_PROTOCOL_VERSION));
  put_u16(out + 6, static_cast<std::uint16_t>(header.type));
  put_u32(out + 8, header.flags);
  put_u32(out + 12, header.payload_length);
  put_u64(out + 16, header.correlation_id);
  put_u32(out + 24, Crc32c::compute(out, 24));
  put_u32(out + 28, header.payload_crc);
}

Result<FrameHeader> decode_frame_header(const std::uint8_t* in, std::size_t available) noexcept {
  if (in == nullptr || available < kFrameHeaderBytes) {
    return make_error<FrameHeader>(ErrorCode::ProtocolTruncated, "frame header is truncated");
  }
  if (get_u32(in) != kFrameMagic) {
    return make_error<FrameHeader>(ErrorCode::ProtocolMalformed, "frame magic mismatch");
  }
  const std::uint16_t version = get_u16(in + 4);
  if (version != static_cast<std::uint16_t>(BB_PROTOCOL_VERSION)) {
    return make_error<FrameHeader>(ErrorCode::ProtocolVersionUnsupported,
                                   "frame protocol version is not supported by this build");
  }
  const std::uint16_t type_raw = get_u16(in + 6);
  if (!is_known_message_type(type_raw)) {
    return make_error<FrameHeader>(ErrorCode::ProtocolMalformed, "frame message type is not defined");
  }
  const std::uint32_t flags = get_u32(in + 8);
  if ((flags & ~kAllowedFlags) != 0) {
    return make_error<FrameHeader>(ErrorCode::ProtocolMalformed, "frame carries reserved flag bits");
  }
  const std::uint32_t payload_length = get_u32(in + 12);
  if (payload_length > limits::kMaxPayloadBytes) {
    return make_error<FrameHeader>(ErrorCode::ProtocolPayloadTooLarge, "frame payload exceeds the supported maximum");
  }
  if (get_u32(in + 24) != Crc32c::compute(in, 24)) {
    return make_error<FrameHeader>(ErrorCode::ProtocolChecksum, "frame header checksum mismatch");
  }

  FrameHeader header;
  header.type = static_cast<MessageType>(type_raw);
  header.flags = flags;
  header.payload_length = payload_length;
  header.correlation_id = get_u64(in + 16);
  header.payload_crc = get_u32(in + 28);
  return header;
}

Status verify_payload_crc(const FrameHeader& header, const std::uint8_t* payload, std::size_t length) noexcept {
  if (length != header.payload_length) {
    return make_error_status(ErrorCode::ProtocolTruncated, "payload length does not match the frame header");
  }
  std::uint32_t actual = 0;
  if (length != 0) {
    if (payload == nullptr) {
      return make_error_status(ErrorCode::ProtocolMalformed, "frame declares a payload but supplied none");
    }
    actual = Crc32c::compute(payload, length);
  }
  if (actual != header.payload_crc) {
    return make_error_status(ErrorCode::ProtocolChecksum, "frame payload checksum mismatch");
  }
  return Status::success();
}

Result<std::vector<std::uint8_t>> encode_frame(MessageType type,
                                               std::uint64_t correlation_id,
                                               const std::vector<std::uint8_t>& payload) {
  if (payload.size() > limits::kMaxPayloadBytes) {
    return make_error<std::vector<std::uint8_t>>(ErrorCode::ProtocolPayloadTooLarge,
                                                 "frame payload exceeds the supported maximum");
  }
  FrameHeader header;
  header.type = type;
  header.flags = 0;
  header.payload_length = static_cast<std::uint32_t>(payload.size());
  header.correlation_id = correlation_id;
  header.payload_crc = payload.empty() ? 0u : Crc32c::compute(payload.data(), payload.size());

  std::vector<std::uint8_t> frame(kFrameHeaderBytes + payload.size());
  encode_frame_header(header, frame.data());
  if (!payload.empty()) {
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
  }
  return frame;
}

Status FrameDecoder::feed(const std::uint8_t* data, std::size_t length) {
  if (failed_) {
    return make_error_status(ErrorCode::ProtocolMalformed, "frame decoder is in a failed state");
  }
  if (data == nullptr && length != 0) {
    return make_error_status(ErrorCode::InvalidArgument, "frame decoder received a null buffer");
  }
  const std::size_t budget = kFrameHeaderBytes + limits::kMaxPayloadBytes;
  const std::size_t pending = buffered_bytes();
  if (length > budget || pending > budget - length) {
    failed_ = true;
    return make_error_status(ErrorCode::ProtocolPayloadTooLarge,
                             "streamed frame exceeds the maximum frame size; the connection is refused");
  }
  if (read_offset_ != 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
    read_offset_ = 0;
  }
  if (length != 0) {
    buffer_.insert(buffer_.end(), data, data + length);
  }
  return Status::success();
}

Result<bool> FrameDecoder::next(MessageType& type, std::uint64_t& correlation_id, std::vector<std::uint8_t>& payload) {
  if (failed_) {
    return make_error<bool>(ErrorCode::ProtocolMalformed, "frame decoder is in a failed state");
  }
  if (buffered_bytes() < kFrameHeaderBytes) {
    return false;
  }
  const auto header = decode_frame_header(buffer_.data() + read_offset_, kFrameHeaderBytes);
  if (!header.ok()) {
    failed_ = true;
    return header.error();
  }
  const std::size_t total = kFrameHeaderBytes + header.value().payload_length;
  if (buffered_bytes() < total) {
    return false;
  }
  const std::uint8_t* body = buffer_.data() + read_offset_ + kFrameHeaderBytes;
  const auto verified = verify_payload_crc(header.value(), body, header.value().payload_length);
  if (!verified.ok()) {
    failed_ = true;
    return verified.error();
  }
  type = header.value().type;
  correlation_id = header.value().correlation_id;
  payload.assign(body, body + header.value().payload_length);
  read_offset_ += total;
  if (read_offset_ == buffer_.size()) {
    buffer_.clear();
    read_offset_ = 0;
  }
  return true;
}

void FrameDecoder::reset() noexcept {
  buffer_.clear();
  read_offset_ = 0;
  failed_ = false;
}

// ---------------------------------------------------------------------------
// Codec primitives
// ---------------------------------------------------------------------------

void Writer::u8(std::uint8_t value) { data_.push_back(value); }

void Writer::u16(std::uint16_t value) {
  std::uint8_t raw[2];
  put_u16(raw, value);
  data_.insert(data_.end(), raw, raw + 2);
}

void Writer::u32(std::uint32_t value) {
  std::uint8_t raw[4];
  put_u32(raw, value);
  data_.insert(data_.end(), raw, raw + 4);
}

void Writer::u64(std::uint64_t value) {
  std::uint8_t raw[8];
  put_u64(raw, value);
  data_.insert(data_.end(), raw, raw + 8);
}

void Writer::bytes(const std::uint8_t* data, std::size_t length) {
  u32(static_cast<std::uint32_t>(length));
  if (length != 0) {
    data_.insert(data_.end(), data, data + length);
  }
}

void Writer::text(std::string_view value) { bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()); }

Result<std::uint8_t> Reader::u8() {
  if (remaining() < 1) {
    return make_error<std::uint8_t>(ErrorCode::ProtocolTruncated, "payload ended while reading a byte");
  }
  return data_[offset_++];
}

Result<std::uint16_t> Reader::u16() {
  if (remaining() < 2) {
    return make_error<std::uint16_t>(ErrorCode::ProtocolTruncated, "payload ended while reading a 16-bit field");
  }
  const std::uint16_t value = get_u16(data_ + offset_);
  offset_ += 2;
  return value;
}

Result<std::uint32_t> Reader::u32() {
  if (remaining() < 4) {
    return make_error<std::uint32_t>(ErrorCode::ProtocolTruncated, "payload ended while reading a 32-bit field");
  }
  const std::uint32_t value = get_u32(data_ + offset_);
  offset_ += 4;
  return value;
}

Result<std::uint64_t> Reader::u64() {
  if (remaining() < 8) {
    return make_error<std::uint64_t>(ErrorCode::ProtocolTruncated, "payload ended while reading a 64-bit field");
  }
  const std::uint64_t value = get_u64(data_ + offset_);
  offset_ += 8;
  return value;
}

Result<bool> Reader::boolean() {
  const auto raw = u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > 1u) {
    return make_error<bool>(ErrorCode::ProtocolMalformed, "boolean field is neither 0 nor 1");
  }
  return raw.value() == 1u;
}

Result<std::string_view> Reader::bytes(std::size_t max_bytes) {
  const auto length = u32();
  if (!length.ok()) {
    return length.error();
  }
  const std::size_t declared = length.value();
  if (declared > max_bytes || declared > limits::kMaxStringBytesInFrame) {
    return make_error<std::string_view>(ErrorCode::ProtocolPayloadTooLarge,
                                        "length-prefixed field exceeds the supported maximum");
  }
  if (remaining() < declared) {
    return make_error<std::string_view>(ErrorCode::ProtocolTruncated, "payload ended inside a length-prefixed field");
  }
  const std::string_view view(reinterpret_cast<const char*>(data_ + offset_), declared);
  offset_ += declared;
  return view;
}

Result<std::string_view> Reader::text(std::size_t max_bytes) { return bytes(max_bytes); }

Result<Bandwidth> Reader::bandwidth() {
  const auto raw = u64();
  if (!raw.ok()) {
    return raw.error();
  }
  return Bandwidth::from_bits_per_second(static_cast<Bandwidth::rep>(raw.value()));
}

Status Reader::expect_end() const {
  if (remaining() != 0) {
    return make_error_status(ErrorCode::ProtocolMalformed, "payload carries trailing bytes");
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Model codecs
// ---------------------------------------------------------------------------

void encode(Writer& writer, const BootId& value) { writer.bytes(value.bytes().data(), value.bytes().size()); }

Result<BootId> decode_boot_id(Reader& reader) {
  const auto view = reader.bytes(BootId::kSize);
  if (!view.ok()) {
    return view.error();
  }
  if (view.value().size() != BootId::kSize) {
    return make_error<BootId>(ErrorCode::ProtocolMalformed, "boot identity field has the wrong length");
  }
  std::array<std::uint8_t, BootId::kSize> bytes{};
  std::memcpy(bytes.data(), view.value().data(), BootId::kSize);
  return BootId::from_bytes(bytes);
}

void encode(Writer& writer, const Provenance& value) {
  // Provenance may legitimately be absent (for example a policy installed by an
  // operator action rather than by a publishing process). Absence is encoded
  // explicitly so that decoding never has to invent a boot identity.
  writer.boolean(value.valid());
  if (!value.valid()) {
    return;
  }
  write_enum(writer, value.source_kind);
  writer.u64(value.source_id);
  encode(writer, value.source_boot);
  write_id(writer, value.epoch);
  write_id(writer, value.coordinator);
  writer.u64(value.source_sequence);
  writer.u64(static_cast<std::uint64_t>(value.recorded_at.unix_nanoseconds()));
  write_text(writer, value.detail);
}

Result<Provenance> decode_provenance(Reader& reader) {
  Provenance value;
  const auto present = reader.boolean();
  if (!present.ok()) {
    return present.error();
  }
  if (!present.value()) {
    return value;
  }
  const auto kind = read_enum<NodeKind>(reader, 5);
  if (!kind.ok()) return kind.error();
  value.source_kind = kind.value();
  const auto source_id = reader.u64();
  if (!source_id.ok()) return source_id.error();
  value.source_id = source_id.value();
  const auto boot = decode_boot_id(reader);
  if (!boot.ok()) return boot.error();
  value.source_boot = boot.value();
  const auto epoch = read_id<FabricEpochTag>(reader);
  if (!epoch.ok()) return epoch.error();
  value.epoch = epoch.value();
  const auto coordinator = read_id<CoordinatorIncarnationTag>(reader);
  if (!coordinator.ok()) return coordinator.error();
  value.coordinator = coordinator.value();
  const auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.error();
  value.source_sequence = sequence.value();
  const auto recorded = reader.u64();
  if (!recorded.ok()) return recorded.error();
  const auto timestamp = Timestamp::from_unix_nanoseconds(static_cast<Timestamp::rep>(recorded.value()));
  if (!timestamp.ok()) return timestamp.error();
  value.recorded_at = timestamp.value();
  auto detail = read_text(reader, limits::kMaxProvenanceDetailBytes, "provenance detail");
  if (!detail.ok()) return detail.error();
  value.detail = detail.take();
  // The encoder writes the presence flag from Provenance::valid(), so a present
  // provenance that is not a complete record is rejected instead of being
  // accepted here and re-encoded differently: decoding must be the exact
  // inverse of encoding, or a mutated payload could decode into a value that
  // does not round-trip.
  if (!value.valid()) {
    return make_error<Provenance>(ErrorCode::ProtocolMalformed,
                                  "provenance is marked present but is not a complete record");
  }
  return value;
}

void encode(Writer& writer, const AuthorityVector& value) {
  write_id(writer, value.fabric_epoch);
  write_id(writer, value.coordinator);
  write_id(writer, value.resource);
  write_generation(writer, value.resource_generation);
  write_generation(writer, value.capacity_generation);
  write_id(writer, value.policy);
  write_generation(writer, value.policy_generation);
  write_id(writer, value.request);
  write_generation(writer, value.request_generation);
  write_id(writer, value.reservation);
  write_generation(writer, value.reservation_generation);
  write_id(writer, value.fairness_group);
  write_generation(writer, value.fairness_config_generation);
  write_generation(writer, value.tenant_config_generation);
  write_id(writer, value.publisher);
  encode(writer, value.publisher_boot);
  writer.u64(value.publisher_sequence);
  writer.u64(value.monotonic_tick);
}

Result<AuthorityVector> decode_authority(Reader& reader) {
  AuthorityVector value;
  auto epoch = read_id<FabricEpochTag>(reader);
  if (!epoch.ok()) return epoch.error();
  value.fabric_epoch = epoch.value();
  auto coordinator = read_id<CoordinatorIncarnationTag>(reader);
  if (!coordinator.ok()) return coordinator.error();
  value.coordinator = coordinator.value();
  auto resource = read_id<BandwidthResourceTag>(reader);
  if (!resource.ok()) return resource.error();
  value.resource = resource.value();
  auto resource_generation = read_generation<BandwidthResourceGenerationTag>(reader);
  if (!resource_generation.ok()) return resource_generation.error();
  value.resource_generation = resource_generation.value();
  auto capacity_generation = read_generation<CapacitySnapshotGenerationTag>(reader);
  if (!capacity_generation.ok()) return capacity_generation.error();
  value.capacity_generation = capacity_generation.value();
  auto policy = read_id<PolicyTag>(reader);
  if (!policy.ok()) return policy.error();
  value.policy = policy.value();
  auto policy_generation = read_generation<PolicyGenerationTag>(reader);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  auto request_generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!request_generation.ok()) return request_generation.error();
  value.request_generation = request_generation.value();
  auto reservation = read_id<ReservationReferenceTag>(reader);
  if (!reservation.ok()) return reservation.error();
  value.reservation = reservation.value();
  auto reservation_generation = read_generation<ReservationGenerationTag>(reader);
  if (!reservation_generation.ok()) return reservation_generation.error();
  value.reservation_generation = reservation_generation.value();
  auto group = read_id<FairnessGroupTag>(reader);
  if (!group.ok()) return group.error();
  value.fairness_group = group.value();
  auto fairness_config_generation = read_generation<FairnessConfigGenerationTag>(reader);
  if (!fairness_config_generation.ok()) return fairness_config_generation.error();
  value.fairness_config_generation = fairness_config_generation.value();
  auto tenant_config_generation = read_generation<TenantConfigGenerationTag>(reader);
  if (!tenant_config_generation.ok()) return tenant_config_generation.error();
  value.tenant_config_generation = tenant_config_generation.value();
  auto publisher = read_id<PublisherTag>(reader);
  if (!publisher.ok()) return publisher.error();
  value.publisher = publisher.value();
  auto boot = decode_boot_id(reader);
  if (!boot.ok()) return boot.error();
  value.publisher_boot = boot.value();
  auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.error();
  value.publisher_sequence = sequence.value();
  auto tick = reader.u64();
  if (!tick.ok()) return tick.error();
  value.monotonic_tick = tick.value();
  return value;
}

void encode(Writer& writer, const CapacityTarget& value) {
  write_id(writer, value.resource);
  write_generation(writer, value.resource_generation);
  write_id(writer, value.pool);
  write_generation(writer, value.pool_generation);
}

Result<CapacityTarget> decode_capacity_target(Reader& reader) {
  CapacityTarget value;
  auto resource = read_id<BandwidthResourceTag>(reader);
  if (!resource.ok()) return resource.error();
  value.resource = resource.value();
  auto resource_generation = read_generation<BandwidthResourceGenerationTag>(reader);
  if (!resource_generation.ok()) return resource_generation.error();
  value.resource_generation = resource_generation.value();
  auto pool = read_id<BandwidthPoolTag>(reader);
  if (!pool.ok()) return pool.error();
  value.pool = pool.value();
  auto pool_generation = read_generation<BandwidthPoolGenerationTag>(reader);
  if (!pool_generation.ok()) return pool_generation.error();
  value.pool_generation = pool_generation.value();
  return value;
}

void encode(Writer& writer, const CapacitySnapshot& value) {
  encode(writer, value.target);
  write_id(writer, value.snapshot);
  write_generation(writer, value.generation);
  write_enum(writer, value.evidence);
  writer.boolean(value.physical_configured.has_value());
  if (value.physical_configured.has_value()) {
    writer.bandwidth(value.physical_configured.value());
  }
  writer.bandwidth(value.administratively_unavailable);
  writer.bandwidth(value.degraded_loss);
  writer.bandwidth(value.headroom_target);
  writer.bandwidth(value.reserved_committed);
  encode(writer, value.provenance);
  encode(writer, value.authority);
  writer.u64(static_cast<std::uint64_t>(value.observed_at.unix_nanoseconds()));
  writer.u64(value.publisher_sequence);
}

Result<CapacitySnapshot> decode_capacity_snapshot(Reader& reader) {
  CapacitySnapshot value;
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  auto snapshot = read_id<CapacitySnapshotTag>(reader);
  if (!snapshot.ok()) return snapshot.error();
  value.snapshot = snapshot.value();
  auto generation = read_generation<CapacitySnapshotGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  const auto evidence = read_enum<CapacityEvidenceState>(reader, 2);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.value();
  const auto has_physical = reader.boolean();
  if (!has_physical.ok()) return has_physical.error();
  if (has_physical.value()) {
    const auto physical = reader.bandwidth();
    if (!physical.ok()) return physical.error();
    value.physical_configured = physical.value();
  }
  const auto admin = reader.bandwidth();
  if (!admin.ok()) return admin.error();
  value.administratively_unavailable = admin.value();
  const auto degraded = reader.bandwidth();
  if (!degraded.ok()) return degraded.error();
  value.degraded_loss = degraded.value();
  const auto headroom = reader.bandwidth();
  if (!headroom.ok()) return headroom.error();
  value.headroom_target = headroom.value();
  const auto reserved = reader.bandwidth();
  if (!reserved.ok()) return reserved.error();
  value.reserved_committed = reserved.value();
  auto provenance = decode_provenance(reader);
  if (!provenance.ok()) return provenance.error();
  value.provenance = provenance.take();
  auto authority = decode_authority(reader);
  if (!authority.ok()) return authority.error();
  value.authority = authority.take();
  const auto observed = reader.u64();
  if (!observed.ok()) return observed.error();
  const auto timestamp = Timestamp::from_unix_nanoseconds(static_cast<Timestamp::rep>(observed.value()));
  if (!timestamp.ok()) return timestamp.error();
  value.observed_at = timestamp.value();
  const auto sequence = reader.u64();
  if (!sequence.ok()) return sequence.error();
  value.publisher_sequence = sequence.value();
  return value;
}

void encode(Writer& writer, const Policy& value) {
  write_id(writer, value.id);
  write_generation(writer, value.generation);
  write_enum(writer, value.scheduling);
  writer.u32(static_cast<std::uint32_t>(value.priority_classes.size()));
  for (const PriorityClass& priority_class : value.priority_classes) {
    write_id(writer, priority_class.id);
    write_text(writer, priority_class.name);
    writer.u32(priority_class.rank);
    writer.u64(priority_class.weight);
    writer.boolean(priority_class.emergency);
    writer.boolean(priority_class.preemptible);
    writer.boolean(priority_class.borrowing_eligible);
    writer.boolean(priority_class.allow_discretionary);
  }
  writer.u32(static_cast<std::uint32_t>(value.fairness_groups.size()));
  for (const FairnessGroupConfig& group : value.fairness_groups) {
    write_id(writer, group.id);
    write_id(writer, group.tenant);
    write_text(writer, group.name);
    writer.u64(group.weight);
    writer.bandwidth(group.maximum_cap);
    writer.u32(group.maximum_cap_permille);
    writer.boolean(group.borrowing_eligible);
    writer.boolean(group.preemptible);
  }
  writer.bandwidth(value.headroom_target);
  writer.u32(value.headroom_permille);
  writer.u32(value.emergency_reserve_permille);
  writer.boolean(value.borrow.enabled);
  writer.boolean(value.borrow.lend_obligations);
  writer.u32(value.borrow.max_lend_permille);
  writer.u32(value.borrow.max_borrow_permille);
  writer.boolean(value.borrow.borrow_from_headroom);
  writer.boolean(value.starvation.enabled);
  writer.u32(value.starvation.aging_threshold_rounds);
  writer.u32(value.starvation.aging_weight_multiplier);
  writer.u32(value.starvation.maximum_promotions);
  writer.boolean(value.preemption.enabled);
  writer.boolean(value.preemption.recall_discretionary);
  writer.u32(value.preemption.recall_grace_rounds);
  writer.u32(value.oversubscription.numerator);
  writer.u32(value.oversubscription.denominator);
  writer.bandwidth(value.minimum_allocation_quantum);
  writer.u32(value.hysteresis_rounds);
  write_generation(writer, value.fairness_config_generation);
  write_generation(writer, value.tenant_config_generation);
  encode(writer, value.provenance);
}

Result<Policy> decode_policy(Reader& reader) {
  Policy value;
  auto id = read_id<PolicyTag>(reader);
  if (!id.ok()) return id.error();
  value.id = id.value();
  auto generation = read_generation<PolicyGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  const auto scheduling = read_enum<SchedulingMode>(reader, 1);
  if (!scheduling.ok()) return scheduling.error();
  value.scheduling = scheduling.value();

  const auto class_count = reader.u32();
  if (!class_count.ok()) return class_count.error();
  if (class_count.value() > limits::kMaxPriorityClasses) {
    return make_error<Policy>(ErrorCode::ProtocolPayloadTooLarge, "policy declares too many priority classes");
  }
  value.priority_classes.reserve(class_count.value());
  for (std::uint32_t i = 0; i < class_count.value(); ++i) {
    PriorityClass priority_class;
    auto class_id = read_id<PriorityClassTag>(reader);
    if (!class_id.ok()) return class_id.error();
    priority_class.id = class_id.value();
    auto name = read_text(reader, limits::kMaxNameBytes, "priority class name");
    if (!name.ok()) return name.error();
    priority_class.name = name.take();
    const auto rank = reader.u32();
    if (!rank.ok()) return rank.error();
    priority_class.rank = rank.value();
    const auto weight = reader.u64();
    if (!weight.ok()) return weight.error();
    priority_class.weight = weight.value();
    const auto emergency = reader.boolean();
    if (!emergency.ok()) return emergency.error();
    priority_class.emergency = emergency.value();
    const auto preemptible = reader.boolean();
    if (!preemptible.ok()) return preemptible.error();
    priority_class.preemptible = preemptible.value();
    const auto borrowing = reader.boolean();
    if (!borrowing.ok()) return borrowing.error();
    priority_class.borrowing_eligible = borrowing.value();
    const auto discretionary = reader.boolean();
    if (!discretionary.ok()) return discretionary.error();
    priority_class.allow_discretionary = discretionary.value();
    value.priority_classes.push_back(std::move(priority_class));
  }

  const auto group_count = reader.u32();
  if (!group_count.ok()) return group_count.error();
  if (group_count.value() > limits::kMaxFairnessGroups) {
    return make_error<Policy>(ErrorCode::ProtocolPayloadTooLarge, "policy declares too many fairness groups");
  }
  value.fairness_groups.reserve(group_count.value());
  for (std::uint32_t i = 0; i < group_count.value(); ++i) {
    FairnessGroupConfig group;
    auto group_id = read_id<FairnessGroupTag>(reader);
    if (!group_id.ok()) return group_id.error();
    group.id = group_id.value();
    auto tenant = read_id<TenantTag>(reader);
    if (!tenant.ok()) return tenant.error();
    group.tenant = tenant.value();
    auto name = read_text(reader, limits::kMaxNameBytes, "fairness group name");
    if (!name.ok()) return name.error();
    group.name = name.take();
    const auto weight = reader.u64();
    if (!weight.ok()) return weight.error();
    group.weight = weight.value();
    const auto cap = reader.bandwidth();
    if (!cap.ok()) return cap.error();
    group.maximum_cap = cap.value();
    const auto cap_permille = reader.u32();
    if (!cap_permille.ok()) return cap_permille.error();
    group.maximum_cap_permille = cap_permille.value();
    const auto borrowing = reader.boolean();
    if (!borrowing.ok()) return borrowing.error();
    group.borrowing_eligible = borrowing.value();
    const auto preemptible = reader.boolean();
    if (!preemptible.ok()) return preemptible.error();
    group.preemptible = preemptible.value();
    value.fairness_groups.push_back(std::move(group));
  }

  const auto headroom_target = reader.bandwidth();
  if (!headroom_target.ok()) return headroom_target.error();
  value.headroom_target = headroom_target.value();

  const auto headroom_permille = reader.u32();
  if (!headroom_permille.ok()) return headroom_permille.error();
  value.headroom_permille = headroom_permille.value();
  const auto emergency_permille = reader.u32();
  if (!emergency_permille.ok()) return emergency_permille.error();
  value.emergency_reserve_permille = emergency_permille.value();

  const auto borrow_enabled = reader.boolean();
  if (!borrow_enabled.ok()) return borrow_enabled.error();
  value.borrow.enabled = borrow_enabled.value();
  const auto lend_obligations = reader.boolean();
  if (!lend_obligations.ok()) return lend_obligations.error();
  value.borrow.lend_obligations = lend_obligations.value();
  const auto max_lend = reader.u32();
  if (!max_lend.ok()) return max_lend.error();
  value.borrow.max_lend_permille = max_lend.value();
  const auto max_borrow = reader.u32();
  if (!max_borrow.ok()) return max_borrow.error();
  value.borrow.max_borrow_permille = max_borrow.value();
  const auto from_headroom = reader.boolean();
  if (!from_headroom.ok()) return from_headroom.error();
  value.borrow.borrow_from_headroom = from_headroom.value();

  const auto starvation_enabled = reader.boolean();
  if (!starvation_enabled.ok()) return starvation_enabled.error();
  value.starvation.enabled = starvation_enabled.value();
  const auto threshold = reader.u32();
  if (!threshold.ok()) return threshold.error();
  value.starvation.aging_threshold_rounds = threshold.value();
  const auto multiplier = reader.u32();
  if (!multiplier.ok()) return multiplier.error();
  value.starvation.aging_weight_multiplier = multiplier.value();
  const auto promotions = reader.u32();
  if (!promotions.ok()) return promotions.error();
  value.starvation.maximum_promotions = promotions.value();

  const auto preemption_enabled = reader.boolean();
  if (!preemption_enabled.ok()) return preemption_enabled.error();
  value.preemption.enabled = preemption_enabled.value();
  const auto recall_discretionary = reader.boolean();
  if (!recall_discretionary.ok()) return recall_discretionary.error();
  value.preemption.recall_discretionary = recall_discretionary.value();
  const auto grace = reader.u32();
  if (!grace.ok()) return grace.error();
  value.preemption.recall_grace_rounds = grace.value();

  const auto numerator = reader.u32();
  if (!numerator.ok()) return numerator.error();
  value.oversubscription.numerator = numerator.value();
  const auto denominator = reader.u32();
  if (!denominator.ok()) return denominator.error();
  value.oversubscription.denominator = denominator.value();

  const auto quantum = reader.bandwidth();
  if (!quantum.ok()) return quantum.error();
  value.minimum_allocation_quantum = quantum.value();
  const auto hysteresis = reader.u32();
  if (!hysteresis.ok()) return hysteresis.error();
  value.hysteresis_rounds = hysteresis.value();

  auto fairness_generation = read_generation<FairnessConfigGenerationTag>(reader);
  if (!fairness_generation.ok()) return fairness_generation.error();
  value.fairness_config_generation = fairness_generation.value();
  auto tenant_generation = read_generation<TenantConfigGenerationTag>(reader);
  if (!tenant_generation.ok()) return tenant_generation.error();
  value.tenant_config_generation = tenant_generation.value();

  auto provenance = decode_provenance(reader);
  if (!provenance.ok()) return provenance.error();
  value.provenance = provenance.take();
  return value;
}

void encode(Writer& writer, const Obligation& value) {
  write_id(writer, value.reservation);
  write_generation(writer, value.generation);
  encode(writer, value.target);
  writer.bandwidth(value.amount);
  writer.boolean(value.lendable);
  writer.u32(value.max_lend_permille);
  encode(writer, value.provenance);
}

Result<Obligation> decode_obligation(Reader& reader) {
  Obligation value;
  auto reservation = read_id<ReservationReferenceTag>(reader);
  if (!reservation.ok()) return reservation.error();
  value.reservation = reservation.value();
  auto generation = read_generation<ReservationGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  const auto amount = reader.bandwidth();
  if (!amount.ok()) return amount.error();
  value.amount = amount.value();
  const auto lendable = reader.boolean();
  if (!lendable.ok()) return lendable.error();
  value.lendable = lendable.value();
  const auto permille = reader.u32();
  if (!permille.ok()) return permille.error();
  value.max_lend_permille = permille.value();
  auto provenance = decode_provenance(reader);
  if (!provenance.ok()) return provenance.error();
  value.provenance = provenance.take();
  return value;
}

void encode(Writer& writer, const BandwidthRequest& value) {
  write_id(writer, value.id);
  write_generation(writer, value.generation);
  encode(writer, value.authority);
  encode(writer, value.target);
  writer.bandwidth(value.minimum);
  writer.bandwidth(value.desired);
  writer.bandwidth(value.maximum);
  write_enum(writer, value.guarantee_class);
  write_id(writer, value.priority);
  write_id(writer, value.tenant);
  write_id(writer, value.fairness_group);
  writer.boolean(value.borrowing_eligible);
  writer.boolean(value.preemptible);
  write_enum(writer, value.recall_tolerance);
  writer.boolean(value.reservation.has_value());
  if (value.reservation.has_value()) {
    write_id(writer, value.reservation->reservation);
    write_generation(writer, value.reservation->generation);
  }
  writer.boolean(value.window.has_value());
  if (value.window.has_value()) {
    writer.u64(value.window->start_tick);
    writer.boolean(value.window->end_tick.has_value());
    if (value.window->end_tick.has_value()) {
      writer.u64(value.window->end_tick.value());
    }
  }
  write_id(writer, value.latency_slo);
  writer.u32(static_cast<std::uint32_t>(value.labels.size()));
  for (const PolicyLabel& label : value.labels) {
    write_text(writer, label.key);
    write_text(writer, label.value);
  }
  encode(writer, value.provenance);
}

Result<BandwidthRequest> decode_request(Reader& reader) {
  BandwidthRequest value;
  auto id = read_id<BandwidthRequestTag>(reader);
  if (!id.ok()) return id.error();
  value.id = id.value();
  auto generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  auto authority = decode_authority(reader);
  if (!authority.ok()) return authority.error();
  value.authority = authority.take();
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  const auto minimum = reader.bandwidth();
  if (!minimum.ok()) return minimum.error();
  value.minimum = minimum.value();
  const auto desired = reader.bandwidth();
  if (!desired.ok()) return desired.error();
  value.desired = desired.value();
  const auto maximum = reader.bandwidth();
  if (!maximum.ok()) return maximum.error();
  value.maximum = maximum.value();
  const auto guarantee = read_enum<GuaranteeClass>(reader, 2);
  if (!guarantee.ok()) return guarantee.error();
  value.guarantee_class = guarantee.value();
  auto priority = read_id<PriorityClassTag>(reader);
  if (!priority.ok()) return priority.error();
  value.priority = priority.value();
  auto tenant = read_id<TenantTag>(reader);
  if (!tenant.ok()) return tenant.error();
  value.tenant = tenant.value();
  auto group = read_id<FairnessGroupTag>(reader);
  if (!group.ok()) return group.error();
  value.fairness_group = group.value();
  const auto borrowing = reader.boolean();
  if (!borrowing.ok()) return borrowing.error();
  value.borrowing_eligible = borrowing.value();
  const auto preemptible = reader.boolean();
  if (!preemptible.ok()) return preemptible.error();
  value.preemptible = preemptible.value();
  const auto recall = read_enum<RecallTolerance>(reader, 3);
  if (!recall.ok()) return recall.error();
  value.recall_tolerance = recall.value();
  const auto has_reservation = reader.boolean();
  if (!has_reservation.ok()) return has_reservation.error();
  if (has_reservation.value()) {
    ReservationBinding binding;
    auto reference = read_id<ReservationReferenceTag>(reader);
    if (!reference.ok()) return reference.error();
    binding.reservation = reference.value();
    auto reservation_generation = read_generation<ReservationGenerationTag>(reader);
    if (!reservation_generation.ok()) return reservation_generation.error();
    binding.generation = reservation_generation.value();
    value.reservation = binding;
  }
  const auto has_window = reader.boolean();
  if (!has_window.ok()) return has_window.error();
  if (has_window.value()) {
    RequestWindow window;
    const auto start = reader.u64();
    if (!start.ok()) return start.error();
    window.start_tick = start.value();
    const auto has_end = reader.boolean();
    if (!has_end.ok()) return has_end.error();
    if (has_end.value()) {
      const auto end = reader.u64();
      if (!end.ok()) return end.error();
      window.end_tick = end.value();
    }
    value.window = window;
  }
  auto slo = read_id<SloClassTag>(reader);
  if (!slo.ok()) return slo.error();
  value.latency_slo = slo.value();
  const auto label_count = reader.u32();
  if (!label_count.ok()) return label_count.error();
  if (label_count.value() > limits::kMaxPolicyLabels) {
    return make_error<BandwidthRequest>(ErrorCode::ProtocolPayloadTooLarge, "request carries too many policy labels");
  }
  value.labels.reserve(label_count.value());
  for (std::uint32_t i = 0; i < label_count.value(); ++i) {
    PolicyLabel label;
    auto key = read_text(reader, limits::kMaxLabelKeyBytes, "policy label key");
    if (!key.ok()) return key.error();
    label.key = key.take();
    auto label_value = read_text(reader, limits::kMaxLabelValueBytes, "policy label value");
    if (!label_value.ok()) return label_value.error();
    label.value = label_value.take();
    value.labels.push_back(std::move(label));
  }
  auto provenance = decode_provenance(reader);
  if (!provenance.ok()) return provenance.error();
  value.provenance = provenance.take();
  return value;
}

void encode(Writer& writer, const GrantAllocation& value) {
  writer.bandwidth(value.guaranteed);
  writer.bandwidth(value.discretionary);
  writer.bandwidth(value.borrowed);
  writer.bandwidth(value.contingent);
}

Result<GrantAllocation> decode_grant_allocation(Reader& reader) {
  GrantAllocation value;
  const auto guaranteed = reader.bandwidth();
  if (!guaranteed.ok()) return guaranteed.error();
  value.guaranteed = guaranteed.value();
  const auto discretionary = reader.bandwidth();
  if (!discretionary.ok()) return discretionary.error();
  value.discretionary = discretionary.value();
  const auto borrowed = reader.bandwidth();
  if (!borrowed.ok()) return borrowed.error();
  value.borrowed = borrowed.value();
  const auto contingent = reader.bandwidth();
  if (!contingent.ok()) return contingent.error();
  value.contingent = contingent.value();
  return value;
}

void encode(Writer& writer, const RecallRecord& value) {
  write_id(writer, value.id);
  write_id(writer, value.grant);
  write_generation(writer, value.grant_generation);
  write_id(writer, value.request);
  encode(writer, value.target);
  writer.bandwidth(value.recalled);
  writer.bandwidth(value.remaining);
  write_enum(writer, value.reason);
  writer.u64(value.requested_tick);
  writer.u64(value.deadline_tick);
  writer.boolean(value.acknowledgement_required);
  encode(writer, value.authority);
}

Result<RecallRecord> decode_recall_record(Reader& reader) {
  RecallRecord value;
  auto id = read_id<RecallTag>(reader);
  if (!id.ok()) return id.error();
  value.id = id.value();
  auto grant = read_id<BandwidthGrantTag>(reader);
  if (!grant.ok()) return grant.error();
  value.grant = grant.value();
  auto grant_generation = read_generation<BandwidthGrantGenerationTag>(reader);
  if (!grant_generation.ok()) return grant_generation.error();
  value.grant_generation = grant_generation.value();
  auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  const auto recalled = reader.bandwidth();
  if (!recalled.ok()) return recalled.error();
  value.recalled = recalled.value();
  const auto remaining = reader.bandwidth();
  if (!remaining.ok()) return remaining.error();
  value.remaining = remaining.value();
  const auto reason = read_enum<OutcomeReason>(reader, 46);
  if (!reason.ok()) return reason.error();
  value.reason = reason.value();
  const auto requested_tick = reader.u64();
  if (!requested_tick.ok()) return requested_tick.error();
  value.requested_tick = requested_tick.value();
  const auto deadline_tick = reader.u64();
  if (!deadline_tick.ok()) return deadline_tick.error();
  value.deadline_tick = deadline_tick.value();
  const auto acknowledgement = reader.boolean();
  if (!acknowledgement.ok()) return acknowledgement.error();
  value.acknowledgement_required = acknowledgement.value();
  auto authority = decode_authority(reader);
  if (!authority.ok()) return authority.error();
  value.authority = authority.take();
  return value;
}

void encode(Writer& writer, const Grant& value) {
  write_id(writer, value.id);
  write_generation(writer, value.generation);
  write_id(writer, value.request);
  write_generation(writer, value.request_generation);
  encode(writer, value.target);
  write_enum(writer, value.state);
  encode(writer, value.allocation);
  writer.bandwidth(value.requested_minimum);
  writer.bandwidth(value.requested_desired);
  writer.bandwidth(value.requested_maximum);
  writer.bandwidth(value.denied);
  writer.boolean(value.satisfied);
  write_enum(writer, value.reason);
  write_id(writer, value.decision);
  write_id(writer, value.recall);
  writer.boolean(value.superseded_by.has_value());
  if (value.superseded_by.has_value()) {
    write_id(writer, value.superseded_by.value());
  }
  writer.boolean(value.supersedes.has_value());
  if (value.supersedes.has_value()) {
    write_id(writer, value.supersedes.value());
  }
  writer.u64(value.issued_tick);
  writer.u64(value.last_revalidated_tick);
  writer.boolean(value.expires_at_tick.has_value());
  if (value.expires_at_tick.has_value()) {
    writer.u64(value.expires_at_tick.value());
  }
  writer.u64(value.wait_rounds);
  writer.bandwidth(value.obligation_backed);
  writer.bandwidth(value.reserve_backed);
  encode(writer, value.authority);
}

Result<Grant> decode_grant(Reader& reader) {
  Grant value;
  auto id = read_id<BandwidthGrantTag>(reader);
  if (!id.ok()) return id.error();
  value.id = id.value();
  auto generation = read_generation<BandwidthGrantGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  auto request_generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!request_generation.ok()) return request_generation.error();
  value.request_generation = request_generation.value();
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  const auto state = read_enum<GrantState>(reader, 12);
  if (!state.ok()) return state.error();
  value.state = state.value();
  auto allocation = decode_grant_allocation(reader);
  if (!allocation.ok()) return allocation.error();
  value.allocation = allocation.take();
  const auto minimum = reader.bandwidth();
  if (!minimum.ok()) return minimum.error();
  value.requested_minimum = minimum.value();
  const auto desired = reader.bandwidth();
  if (!desired.ok()) return desired.error();
  value.requested_desired = desired.value();
  const auto maximum = reader.bandwidth();
  if (!maximum.ok()) return maximum.error();
  value.requested_maximum = maximum.value();
  const auto denied = reader.bandwidth();
  if (!denied.ok()) return denied.error();
  value.denied = denied.value();
  const auto satisfied = reader.boolean();
  if (!satisfied.ok()) return satisfied.error();
  value.satisfied = satisfied.value();
  const auto reason = read_enum<OutcomeReason>(reader, 46);
  if (!reason.ok()) return reason.error();
  value.reason = reason.value();
  auto decision = read_id<DecisionTag>(reader);
  if (!decision.ok()) return decision.error();
  value.decision = decision.value();
  auto recall = read_id<RecallTag>(reader);
  if (!recall.ok()) return recall.error();
  value.recall = recall.value();
  const auto has_superseded_by = reader.boolean();
  if (!has_superseded_by.ok()) return has_superseded_by.error();
  if (has_superseded_by.value()) {
    auto superseded_by = read_id<BandwidthGrantTag>(reader);
    if (!superseded_by.ok()) return superseded_by.error();
    value.superseded_by = superseded_by.value();
  }
  const auto has_supersedes = reader.boolean();
  if (!has_supersedes.ok()) return has_supersedes.error();
  if (has_supersedes.value()) {
    auto supersedes = read_id<BandwidthGrantTag>(reader);
    if (!supersedes.ok()) return supersedes.error();
    value.supersedes = supersedes.value();
  }
  const auto issued = reader.u64();
  if (!issued.ok()) return issued.error();
  value.issued_tick = issued.value();
  const auto revalidated = reader.u64();
  if (!revalidated.ok()) return revalidated.error();
  value.last_revalidated_tick = revalidated.value();
  const auto has_expiry = reader.boolean();
  if (!has_expiry.ok()) return has_expiry.error();
  if (has_expiry.value()) {
    const auto expiry = reader.u64();
    if (!expiry.ok()) return expiry.error();
    value.expires_at_tick = expiry.value();
  }
  const auto wait = reader.u64();
  if (!wait.ok()) return wait.error();
  value.wait_rounds = wait.value();
  const auto obligation_backed = reader.bandwidth();
  if (!obligation_backed.ok()) return obligation_backed.error();
  value.obligation_backed = obligation_backed.value();
  const auto reserve_backed = reader.bandwidth();
  if (!reserve_backed.ok()) return reserve_backed.error();
  value.reserve_backed = reserve_backed.value();
  auto authority = decode_authority(reader);
  if (!authority.ok()) return authority.error();
  value.authority = authority.take();
  return value;
}

void encode(Writer& writer, const ResourceAccounting& value) {
  encode(writer, value.target);
  write_generation(writer, value.capacity_generation);
  write_enum(writer, value.evidence);
  for (const Bandwidth term : {value.physical_configured, value.administratively_unavailable, value.degraded_loss,
                               value.effective_physical, value.obligations_reserved, value.obligations_consumed,
                               value.obligations_lent, value.obligations_idle, value.capacity_deficit,
                               value.allocatable, value.emergency_reserve, value.emergency_reserve_unused,
                               value.headroom, value.arbitrable, value.guaranteed_granted,
                               value.discretionary_granted, value.borrowed_granted, value.contingent_granted,
                               value.unallocated, value.contingent_pool, value.contingent_unallocated,
                               value.authorized_consumption}) {
    writer.bandwidth(term);
  }
}

Result<ResourceAccounting> decode_accounting(Reader& reader) {
  ResourceAccounting value;
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  auto generation = read_generation<CapacitySnapshotGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.capacity_generation = generation.value();
  const auto evidence = read_enum<CapacityEvidenceState>(reader, 2);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.value();

  Bandwidth* terms[] = {&value.physical_configured, &value.administratively_unavailable, &value.degraded_loss,
                        &value.effective_physical, &value.obligations_reserved, &value.obligations_consumed,
                        &value.obligations_lent, &value.obligations_idle, &value.capacity_deficit,
                        &value.allocatable, &value.emergency_reserve, &value.emergency_reserve_unused,
                        &value.headroom, &value.arbitrable, &value.guaranteed_granted,
                        &value.discretionary_granted, &value.borrowed_granted, &value.contingent_granted,
                        &value.unallocated, &value.contingent_pool, &value.contingent_unallocated,
                        &value.authorized_consumption};
  for (Bandwidth* term : terms) {
    const auto parsed = reader.bandwidth();
    if (!parsed.ok()) return parsed.error();
    *term = parsed.value();
  }
  return value;
}

void encode(Writer& writer, const ArbitrationDecision& value) {
  write_id(writer, value.request);
  write_generation(writer, value.request_generation);
  write_enum(writer, value.state);
  write_enum(writer, value.reason);
  encode(writer, value.allocation);
  writer.bandwidth(value.requested_minimum);
  writer.bandwidth(value.requested_desired);
  writer.bandwidth(value.requested_maximum);
  writer.bandwidth(value.denied);
  writer.bandwidth(value.unmet_minimum);
  writer.boolean(value.satisfied);
  writer.boolean(value.waiting);
  writer.boolean(value.refused);
  writer.boolean(value.starvation_promoted);
  writer.u32(value.effective_rank);
  writer.u64(value.effective_weight);
  writer.u64(value.next_wait_rounds);
  write_text(writer, value.binding_reason);
  write_text(writer, value.authority_mismatch);
  writer.boolean(value.grant.has_value());
  if (value.grant.has_value()) {
    encode(writer, value.grant.value());
  }
  writer.boolean(value.previous_grant.has_value());
  if (value.previous_grant.has_value()) {
    encode(writer, value.previous_grant.value());
  }
  writer.boolean(value.recall.has_value());
  if (value.recall.has_value()) {
    encode(writer, value.recall.value());
  }
}

Result<ArbitrationDecision> decode_arbitration_decision(Reader& reader) {
  ArbitrationDecision value;
  auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  auto generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.request_generation = generation.value();
  const auto state = read_enum<GrantState>(reader, 12);
  if (!state.ok()) return state.error();
  value.state = state.value();
  const auto reason = read_enum<OutcomeReason>(reader, 46);
  if (!reason.ok()) return reason.error();
  value.reason = reason.value();
  auto allocation = decode_grant_allocation(reader);
  if (!allocation.ok()) return allocation.error();
  value.allocation = allocation.take();
  Bandwidth* terms[] = {&value.requested_minimum, &value.requested_desired, &value.requested_maximum,
                        &value.denied, &value.unmet_minimum};
  for (Bandwidth* term : terms) {
    const auto parsed = reader.bandwidth();
    if (!parsed.ok()) return parsed.error();
    *term = parsed.value();
  }
  bool* flags[] = {&value.satisfied, &value.waiting, &value.refused, &value.starvation_promoted};
  for (bool* flag : flags) {
    const auto parsed = reader.boolean();
    if (!parsed.ok()) return parsed.error();
    *flag = parsed.value();
  }
  const auto rank = reader.u32();
  if (!rank.ok()) return rank.error();
  value.effective_rank = rank.value();
  const auto weight = reader.u64();
  if (!weight.ok()) return weight.error();
  value.effective_weight = weight.value();
  const auto wait = reader.u64();
  if (!wait.ok()) return wait.error();
  value.next_wait_rounds = wait.value();
  auto binding = read_text(reader, limits::kMaxExplanationTextBytes, "binding reason");
  if (!binding.ok()) return binding.error();
  value.binding_reason = binding.take();
  auto mismatch = read_text(reader, limits::kMaxExplanationTextBytes, "authority mismatch");
  if (!mismatch.ok()) return mismatch.error();
  value.authority_mismatch = mismatch.take();
  const auto has_grant = reader.boolean();
  if (!has_grant.ok()) return has_grant.error();
  if (has_grant.value()) {
    auto grant = decode_grant(reader);
    if (!grant.ok()) return grant.error();
    value.grant = grant.take();
  }
  const auto has_previous = reader.boolean();
  if (!has_previous.ok()) return has_previous.error();
  if (has_previous.value()) {
    auto previous = decode_grant(reader);
    if (!previous.ok()) return previous.error();
    value.previous_grant = previous.take();
  }
  const auto has_recall = reader.boolean();
  if (!has_recall.ok()) return has_recall.error();
  if (has_recall.value()) {
    auto recall = decode_recall_record(reader);
    if (!recall.ok()) return recall.error();
    value.recall = recall.take();
  }
  return value;
}

void encode(Writer& writer, const RequestExplanation& value) {
  write_id(writer, value.request);
  write_generation(writer, value.request_generation);
  encode(writer, value.target);
  write_enum(writer, value.state);
  write_enum(writer, value.reason);
  writer.boolean(value.satisfied);
  writer.boolean(value.waiting);
  writer.boolean(value.refused);
  encode(writer, value.authority);
  write_id(writer, value.epoch);
  write_id(writer, value.coordinator);
  write_id(writer, value.policy);
  write_generation(writer, value.policy_generation);
  write_id(writer, value.decision);
  writer.u64(value.decision_tick);
  write_enum(writer, value.evidence);
  write_generation(writer, value.capacity_generation);
  writer.bandwidth(value.effective_physical);
  writer.bandwidth(value.obligations_applied);
  writer.bandwidth(value.allocatable);
  writer.bandwidth(value.headroom_preserved);
  writer.bandwidth(value.emergency_reserve_preserved);
  writer.bandwidth(value.arbitrable);
  writer.bandwidth(value.requested_minimum);
  writer.bandwidth(value.requested_desired);
  writer.bandwidth(value.requested_maximum);
  writer.bandwidth(value.guaranteed);
  writer.bandwidth(value.discretionary);
  writer.bandwidth(value.borrowed);
  writer.bandwidth(value.contingent);
  writer.bandwidth(value.total_granted);
  writer.bandwidth(value.denied);
  write_id(writer, value.priority);
  write_id(writer, value.fairness_group);
  write_id(writer, value.tenant);
  writer.u32(value.effective_rank);
  writer.u64(value.effective_weight);
  writer.boolean(value.starvation_promoted);
  writer.bandwidth(value.group_cap);
  writer.bandwidth(value.group_allocated);
  writer.u64(value.wait_rounds);
  writer.boolean(value.recall_pending);
  writer.bandwidth(value.recalled);
  write_id(writer, value.recall);
  write_enum(writer, value.recall_reason);
  writer.u64(value.recall_deadline_tick);
  write_text(writer, value.binding_reason);
  write_text(writer, value.authority_mismatch);
  write_generation(writer, value.resource_generation);
  write_id(writer, value.grant);
  write_generation(writer, value.grant_generation);
  encode(writer, value.provenance);
}

Result<RequestExplanation> decode_explanation(Reader& reader) {
  RequestExplanation value;
  auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  auto request_generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!request_generation.ok()) return request_generation.error();
  value.request_generation = request_generation.value();
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  const auto state = read_enum<GrantState>(reader, 12);
  if (!state.ok()) return state.error();
  value.state = state.value();
  const auto reason = read_enum<OutcomeReason>(reader, 46);
  if (!reason.ok()) return reason.error();
  value.reason = reason.value();
  bool* flags[] = {&value.satisfied, &value.waiting, &value.refused};
  for (bool* flag : flags) {
    const auto parsed = reader.boolean();
    if (!parsed.ok()) return parsed.error();
    *flag = parsed.value();
  }
  auto authority = decode_authority(reader);
  if (!authority.ok()) return authority.error();
  value.authority = authority.take();
  auto epoch = read_id<FabricEpochTag>(reader);
  if (!epoch.ok()) return epoch.error();
  value.epoch = epoch.value();
  auto coordinator = read_id<CoordinatorIncarnationTag>(reader);
  if (!coordinator.ok()) return coordinator.error();
  value.coordinator = coordinator.value();
  auto policy = read_id<PolicyTag>(reader);
  if (!policy.ok()) return policy.error();
  value.policy = policy.value();
  auto policy_generation = read_generation<PolicyGenerationTag>(reader);
  if (!policy_generation.ok()) return policy_generation.error();
  value.policy_generation = policy_generation.value();
  auto decision = read_id<DecisionTag>(reader);
  if (!decision.ok()) return decision.error();
  value.decision = decision.value();
  const auto tick = reader.u64();
  if (!tick.ok()) return tick.error();
  value.decision_tick = tick.value();
  const auto evidence = read_enum<CapacityEvidenceState>(reader, 2);
  if (!evidence.ok()) return evidence.error();
  value.evidence = evidence.value();
  auto capacity_generation = read_generation<CapacitySnapshotGenerationTag>(reader);
  if (!capacity_generation.ok()) return capacity_generation.error();
  value.capacity_generation = capacity_generation.value();
  Bandwidth* terms[] = {&value.effective_physical, &value.obligations_applied, &value.allocatable,
                        &value.headroom_preserved, &value.emergency_reserve_preserved, &value.arbitrable,
                        &value.requested_minimum, &value.requested_desired, &value.requested_maximum,
                        &value.guaranteed, &value.discretionary, &value.borrowed, &value.contingent,
                        &value.total_granted, &value.denied};
  for (Bandwidth* term : terms) {
    const auto parsed = reader.bandwidth();
    if (!parsed.ok()) return parsed.error();
    *term = parsed.value();
  }
  auto priority = read_id<PriorityClassTag>(reader);
  if (!priority.ok()) return priority.error();
  value.priority = priority.value();
  auto group = read_id<FairnessGroupTag>(reader);
  if (!group.ok()) return group.error();
  value.fairness_group = group.value();
  auto tenant = read_id<TenantTag>(reader);
  if (!tenant.ok()) return tenant.error();
  value.tenant = tenant.value();
  const auto rank = reader.u32();
  if (!rank.ok()) return rank.error();
  value.effective_rank = rank.value();
  const auto weight = reader.u64();
  if (!weight.ok()) return weight.error();
  value.effective_weight = weight.value();
  const auto promoted = reader.boolean();
  if (!promoted.ok()) return promoted.error();
  value.starvation_promoted = promoted.value();
  const auto group_cap = reader.bandwidth();
  if (!group_cap.ok()) return group_cap.error();
  value.group_cap = group_cap.value();
  const auto group_allocated = reader.bandwidth();
  if (!group_allocated.ok()) return group_allocated.error();
  value.group_allocated = group_allocated.value();
  const auto wait = reader.u64();
  if (!wait.ok()) return wait.error();
  value.wait_rounds = wait.value();
  const auto recall_pending = reader.boolean();
  if (!recall_pending.ok()) return recall_pending.error();
  value.recall_pending = recall_pending.value();
  const auto recalled = reader.bandwidth();
  if (!recalled.ok()) return recalled.error();
  value.recalled = recalled.value();
  auto recall = read_id<RecallTag>(reader);
  if (!recall.ok()) return recall.error();
  value.recall = recall.value();
  const auto recall_reason = read_enum<OutcomeReason>(reader, 46);
  if (!recall_reason.ok()) return recall_reason.error();
  value.recall_reason = recall_reason.value();
  const auto deadline = reader.u64();
  if (!deadline.ok()) return deadline.error();
  value.recall_deadline_tick = deadline.value();
  auto binding = read_text(reader, limits::kMaxExplanationTextBytes, "binding reason");
  if (!binding.ok()) return binding.error();
  value.binding_reason = binding.take();
  auto mismatch = read_text(reader, limits::kMaxExplanationTextBytes, "authority mismatch");
  if (!mismatch.ok()) return mismatch.error();
  value.authority_mismatch = mismatch.take();
  auto resource_generation = read_generation<BandwidthResourceGenerationTag>(reader);
  if (!resource_generation.ok()) return resource_generation.error();
  value.resource_generation = resource_generation.value();
  auto grant = read_id<BandwidthGrantTag>(reader);
  if (!grant.ok()) return grant.error();
  value.grant = grant.value();
  auto grant_generation = read_generation<BandwidthGrantGenerationTag>(reader);
  if (!grant_generation.ok()) return grant_generation.error();
  value.grant_generation = grant_generation.value();
  auto provenance = decode_provenance(reader);
  if (!provenance.ok()) return provenance.error();
  value.provenance = provenance.take();
  return value;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void encode(Writer& writer, const WireStatus& value) {
  writer.u16(static_cast<std::uint16_t>(value.code));
  write_text(writer, value.message);
}

Result<WireStatus> decode_wire_status(Reader& reader) {
  WireStatus value;
  const auto code = reader.u16();
  if (!code.ok()) return code.error();
  if (code.value() > static_cast<std::uint16_t>(ErrorCode::CapacityWithdrawn)) {
    return make_error<WireStatus>(ErrorCode::ProtocolMalformed, "error code is not defined by this protocol revision");
  }
  value.code = static_cast<ErrorCode>(code.value());
  auto message = read_text(reader, 512, "error message");
  if (!message.ok()) return message.error();
  value.message = message.take();
  return value;
}

WireStatus to_wire_status(const Status& status) {
  WireStatus value;
  value.code = status.code();
  value.message = status.message();
  return value;
}

void encode(Writer& writer, const HelloRequest& value) {
  write_id(writer, value.publisher);
  encode(writer, value.boot);
  writer.u16(value.protocol_version);
  write_text(writer, value.session_token);
  writer.u64(value.nonce);
}

Result<HelloRequest> decode_hello_request(Reader& reader) {
  HelloRequest value;
  auto publisher = read_id<PublisherTag>(reader);
  if (!publisher.ok()) return publisher.error();
  value.publisher = publisher.value();
  auto boot = decode_boot_id(reader);
  if (!boot.ok()) return boot.error();
  value.boot = boot.value();
  const auto version = reader.u16();
  if (!version.ok()) return version.error();
  value.protocol_version = version.value();
  auto token = read_text(reader, 256, "session token");
  if (!token.ok()) return token.error();
  value.session_token = token.take();
  const auto nonce = reader.u64();
  if (!nonce.ok()) return nonce.error();
  value.nonce = nonce.value();
  return value;
}

void encode(Writer& writer, const HelloAck& value) {
  encode(writer, value.status);
  write_id(writer, value.coordinator);
  write_id(writer, value.epoch);
  write_id(writer, value.session);
  writer.u64(value.next_grant_id);
}

Result<HelloAck> decode_hello_ack(Reader& reader) {
  HelloAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  auto coordinator = read_id<CoordinatorIncarnationTag>(reader);
  if (!coordinator.ok()) return coordinator.error();
  value.coordinator = coordinator.value();
  auto epoch = read_id<FabricEpochTag>(reader);
  if (!epoch.ok()) return epoch.error();
  value.epoch = epoch.value();
  auto session = read_id<SessionTag>(reader);
  if (!session.ok()) return session.error();
  value.session = session.value();
  const auto next = reader.u64();
  if (!next.ok()) return next.error();
  value.next_grant_id = next.value();
  return value;
}

void encode(Writer& writer, const PublishCapacityRequest& value) { encode(writer, value.snapshot); }

Result<PublishCapacityRequest> decode_publish_capacity_request(Reader& reader) {
  PublishCapacityRequest value;
  auto snapshot = decode_capacity_snapshot(reader);
  if (!snapshot.ok()) return snapshot.error();
  value.snapshot = snapshot.take();
  return value;
}

void encode(Writer& writer, const PublishCapacityAck& value) {
  encode(writer, value.status);
  write_generation(writer, value.generation);
}

Result<PublishCapacityAck> decode_publish_capacity_ack(Reader& reader) {
  PublishCapacityAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  auto generation = read_generation<CapacitySnapshotGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  return value;
}

void encode(Writer& writer, const SetPolicyRequest& value) { encode(writer, value.policy); }

Result<SetPolicyRequest> decode_set_policy_request(Reader& reader) {
  SetPolicyRequest value;
  auto policy = decode_policy(reader);
  if (!policy.ok()) return policy.error();
  value.policy = policy.take();
  return value;
}

void encode(Writer& writer, const SetPolicyAck& value) {
  encode(writer, value.status);
  write_generation(writer, value.generation);
}

Result<SetPolicyAck> decode_set_policy_ack(Reader& reader) {
  SetPolicyAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  auto generation = read_generation<PolicyGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  return value;
}

void encode(Writer& writer, const SubmitRequestsRequest& value) {
  writer.u32(static_cast<std::uint32_t>(value.requests.size()));
  for (const BandwidthRequest& request : value.requests) {
    encode(writer, request);
  }
}

Result<SubmitRequestsRequest> decode_submit_requests_request(Reader& reader) {
  SubmitRequestsRequest value;
  const auto count = reader.u32();
  if (!count.ok()) return count.error();
  if (count.value() > limits::kMaxRequestsPerRound) {
    return make_error<SubmitRequestsRequest>(ErrorCode::ProtocolPayloadTooLarge, "batch carries too many requests");
  }
  value.requests.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto request = decode_request(reader);
    if (!request.ok()) return request.error();
    value.requests.push_back(request.take());
  }
  return value;
}

void encode(Writer& writer, const SubmitRequestsAck& value) {
  encode(writer, value.status);
  writer.u32(value.accepted);
  writer.u32(static_cast<std::uint32_t>(value.refused.size()));
  for (const BandwidthRequestId id : value.refused) {
    write_id(writer, id);
  }
}

Result<SubmitRequestsAck> decode_submit_requests_ack(Reader& reader) {
  SubmitRequestsAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  const auto accepted = reader.u32();
  if (!accepted.ok()) return accepted.error();
  value.accepted = accepted.value();
  const auto refused = reader.u32();
  if (!refused.ok()) return refused.error();
  if (refused.value() > limits::kMaxRequestsPerRound) {
    return make_error<SubmitRequestsAck>(ErrorCode::ProtocolPayloadTooLarge, "ack lists too many refused requests");
  }
  value.refused.reserve(refused.value());
  for (std::uint32_t i = 0; i < refused.value(); ++i) {
    auto id = read_id<BandwidthRequestTag>(reader);
    if (!id.ok()) return id.error();
    value.refused.push_back(id.value());
  }
  return value;
}

void encode(Writer& writer, const ArbitrateRequest& value) { encode(writer, value.target); }

Result<ArbitrateRequest> decode_arbitrate_request(Reader& reader) {
  ArbitrateRequest value;
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  return value;
}

void encode(Writer& writer, const ArbitrateAck& value) {
  encode(writer, value.status);
  write_id(writer, value.decision);
  encode(writer, value.accounting);
  writer.u32(static_cast<std::uint32_t>(value.decisions.size()));
  for (const ArbitrationDecision& decision : value.decisions) {
    encode(writer, decision);
  }
}

Result<ArbitrateAck> decode_arbitrate_ack(Reader& reader) {
  ArbitrateAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  auto decision = read_id<DecisionTag>(reader);
  if (!decision.ok()) return decision.error();
  value.decision = decision.value();
  auto accounting = decode_accounting(reader);
  if (!accounting.ok()) return accounting.error();
  value.accounting = accounting.take();
  const auto count = reader.u32();
  if (!count.ok()) return count.error();
  if (count.value() > limits::kMaxVectorElementsInFrame) {
    return make_error<ArbitrateAck>(ErrorCode::ProtocolPayloadTooLarge, "round reports too many decisions");
  }
  value.decisions.reserve(count.value());
  for (std::uint32_t i = 0; i < count.value(); ++i) {
    auto entry = decode_arbitration_decision(reader);
    if (!entry.ok()) return entry.error();
    value.decisions.push_back(entry.take());
  }
  return value;
}

void encode(Writer& writer, const QueryGrantRequest& value) {
  write_id(writer, value.grant);
  write_generation(writer, value.generation);
}

Result<QueryGrantRequest> decode_query_grant_request(Reader& reader) {
  QueryGrantRequest value;
  auto grant = read_id<BandwidthGrantTag>(reader);
  if (!grant.ok()) return grant.error();
  value.grant = grant.value();
  auto generation = read_generation<BandwidthGrantGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  return value;
}

void encode(Writer& writer, const QueryGrantAck& value) {
  encode(writer, value.status);
  writer.boolean(value.found);
  encode(writer, value.grant);
}

Result<QueryGrantAck> decode_query_grant_ack(Reader& reader) {
  QueryGrantAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  const auto found = reader.boolean();
  if (!found.ok()) return found.error();
  value.found = found.value();
  auto grant = decode_grant(reader);
  if (!grant.ok()) return grant.error();
  value.grant = grant.take();
  return value;
}

void encode(Writer& writer, const QueryAccountingRequest& value) { encode(writer, value.target); }

Result<QueryAccountingRequest> decode_query_accounting_request(Reader& reader) {
  QueryAccountingRequest value;
  auto target = decode_capacity_target(reader);
  if (!target.ok()) return target.error();
  value.target = target.take();
  return value;
}

void encode(Writer& writer, const QueryAccountingAck& value) {
  encode(writer, value.status);
  encode(writer, value.accounting);
}

Result<QueryAccountingAck> decode_query_accounting_ack(Reader& reader) {
  QueryAccountingAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  auto accounting = decode_accounting(reader);
  if (!accounting.ok()) return accounting.error();
  value.accounting = accounting.take();
  return value;
}

void encode(Writer& writer, const ReleaseRequest& value) {
  write_id(writer, value.grant);
  write_generation(writer, value.generation);
}

Result<ReleaseRequest> decode_release_request(Reader& reader) {
  ReleaseRequest value;
  auto grant = read_id<BandwidthGrantTag>(reader);
  if (!grant.ok()) return grant.error();
  value.grant = grant.value();
  auto generation = read_generation<BandwidthGrantGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  return value;
}

void encode(Writer& writer, const ReleaseAck& value) {
  encode(writer, value.status);
  write_enum(writer, value.state);
  writer.boolean(value.already_released);
  writer.bandwidth(value.released);
}

Result<ReleaseAck> decode_release_ack(Reader& reader) {
  ReleaseAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  const auto state = read_enum<GrantState>(reader, 12);
  if (!state.ok()) return state.error();
  value.state = state.value();
  const auto already = reader.boolean();
  if (!already.ok()) return already.error();
  value.already_released = already.value();
  const auto released = reader.bandwidth();
  if (!released.ok()) return released.error();
  value.released = released.value();
  return value;
}

void encode(Writer& writer, const RevokeRequest& value) {
  write_id(writer, value.grant);
  write_generation(writer, value.generation);
  write_text(writer, value.reason);
}

Result<RevokeRequest> decode_revoke_request(Reader& reader) {
  RevokeRequest value;
  auto grant = read_id<BandwidthGrantTag>(reader);
  if (!grant.ok()) return grant.error();
  value.grant = grant.value();
  auto generation = read_generation<BandwidthGrantGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  auto reason = read_text(reader, 128, "revocation reason");
  if (!reason.ok()) return reason.error();
  value.reason = reason.take();
  return value;
}

void encode(Writer& writer, const RevokeAck& value) {
  encode(writer, value.status);
  write_enum(writer, value.state);
}

Result<RevokeAck> decode_revoke_ack(Reader& reader) {
  RevokeAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  const auto state = read_enum<GrantState>(reader, 12);
  if (!state.ok()) return state.error();
  value.state = state.value();
  return value;
}

void encode(Writer& writer, const FencePublisherRequest& value) {
  write_id(writer, value.publisher);
  encode(writer, value.boot);
  write_text(writer, value.reason);
}

Result<FencePublisherRequest> decode_fence_publisher_request(Reader& reader) {
  FencePublisherRequest value;
  auto publisher = read_id<PublisherTag>(reader);
  if (!publisher.ok()) return publisher.error();
  value.publisher = publisher.value();
  auto boot = decode_boot_id(reader);
  if (!boot.ok()) return boot.error();
  value.boot = boot.value();
  auto reason = read_text(reader, 128, "fence reason");
  if (!reason.ok()) return reason.error();
  value.reason = reason.take();
  return value;
}

void encode(Writer& writer, const FencePublisherAck& value) {
  encode(writer, value.status);
  writer.boolean(value.already_fenced);
}

Result<FencePublisherAck> decode_fence_publisher_ack(Reader& reader) {
  FencePublisherAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  const auto already = reader.boolean();
  if (!already.ok()) return already.error();
  value.already_fenced = already.value();
  return value;
}

void encode(Writer& writer, const ExplainRequest& value) {
  write_id(writer, value.request);
  write_generation(writer, value.generation);
}

Result<ExplainRequest> decode_explain_request(Reader& reader) {
  ExplainRequest value;
  auto request = read_id<BandwidthRequestTag>(reader);
  if (!request.ok()) return request.error();
  value.request = request.value();
  auto generation = read_generation<BandwidthRequestGenerationTag>(reader);
  if (!generation.ok()) return generation.error();
  value.generation = generation.value();
  return value;
}

void encode(Writer& writer, const ExplainAck& value) {
  encode(writer, value.status);
  encode(writer, value.explanation);
}

Result<ExplainAck> decode_explain_ack(Reader& reader) {
  ExplainAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  auto explanation = decode_explanation(reader);
  if (!explanation.ok()) return explanation.error();
  value.explanation = explanation.take();
  return value;
}

void encode(Writer& writer, const ShutdownRequest& value) { write_text(writer, value.reason); }

Result<ShutdownRequest> decode_shutdown_request(Reader& reader) {
  ShutdownRequest value;
  auto reason = read_text(reader, 128, "shutdown reason");
  if (!reason.ok()) return reason.error();
  value.reason = reason.take();
  return value;
}

void encode(Writer& writer, const ShutdownAck& value) { encode(writer, value.status); }

Result<ShutdownAck> decode_shutdown_ack(Reader& reader) {
  ShutdownAck value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  return value;
}

void encode(Writer& writer, const ErrorResponse& value) { encode(writer, value.status); }

Result<ErrorResponse> decode_error_response(Reader& reader) {
  ErrorResponse value;
  auto status = decode_wire_status(reader);
  if (!status.ok()) return status.error();
  value.status = status.take();
  return value;
}

}  // namespace bandwidth_broker
