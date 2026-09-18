// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "bandwidth_broker/checksum.hpp"
#include "bandwidth_broker/numeric.hpp"
#include "bandwidth_broker/version.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bandwidth_broker {
namespace {

namespace fs = std::filesystem;

constexpr char kManifestMagic[8] = {'B', 'B', 'M', 'A', 'N', 'F', '0', '1'};
constexpr char kSnapshotMagic[8] = {'B', 'B', 'S', 'N', 'A', 'P', '0', '1'};
constexpr char kJournalMagic[8] = {'B', 'B', 'J', 'R', 'N', 'L', '0', '1'};
constexpr std::uint32_t kRecordMagic = 0x42425252u;

constexpr std::size_t kManifestBytes = 52;
constexpr std::size_t kSnapshotHeaderBytes = 60;
constexpr std::size_t kJournalHeaderBytes = 28;
constexpr std::size_t kRecordHeaderBytes = 48;
constexpr std::size_t kRecordPayloadLimit = limits::kMaxPayloadBytes;

constexpr const char* kManifestName = "MANIFEST";
constexpr std::string_view kSnapshotPrefix = "snapshot-";
constexpr std::string_view kJournalPrefix = "journal-";
constexpr std::string_view kSnapshotSuffix = ".bbs";
constexpr std::string_view kJournalSuffix = ".bbj";
constexpr char kPathSeparator = static_cast<char>(0x2F);
constexpr char kBackslash = static_cast<char>(0x5C);

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

[[nodiscard]] std::string join_path(const std::string& directory, std::string_view name) {
  std::string out = directory;
  const char last = out.empty() ? static_cast<char>(0) : out.back();
  if (last != kPathSeparator && last != kBackslash) {
    out.push_back(kPathSeparator);
  }
  out.append(name);
  return out;
}

[[nodiscard]] Status io_error(const std::string& what) {
  return make_error_status(ErrorCode::PersistenceIo, "store io failure: " + what);
}

[[nodiscard]] Status sync_file(std::FILE* file) {
#if defined(_WIN32)
  if (_commit(_fileno(file)) != 0) {
    return io_error("sync failed");
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return io_error("sync failed");
  }
#endif
  return Status::success();
}

[[nodiscard]] Status write_all(std::FILE* file, const void* data, std::size_t length) {
  if (length == 0) {
    return Status::success();
  }
  if (std::fwrite(data, 1, length, file) != length) {
    return io_error("short write");
  }
  return Status::success();
}

[[nodiscard]] Status close_file(std::FILE* file) {
  if (file == nullptr) {
    return Status::success();
  }
  if (std::fclose(file) != 0) {
    return io_error("close failed");
  }
  return Status::success();
}

[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t limit) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error) {
    return make_error<std::vector<std::uint8_t>>(ErrorCode::PersistenceIo, "cannot size store file");
  }
  if (size > limit) {
    return make_error<std::vector<std::uint8_t>>(ErrorCode::PersistenceCorrupt,
                                                 "store file exceeds the supported maximum size");
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return make_error<std::vector<std::uint8_t>>(ErrorCode::PersistenceIo, "cannot open store file");
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
  const std::size_t read = buffer.empty() ? 0 : std::fread(buffer.data(), 1, buffer.size(), file);
  const bool short_read = read != buffer.size();
  const auto closed = close_file(file);
  if (short_read) {
    return make_error<std::vector<std::uint8_t>>(ErrorCode::PersistenceIo, "short read from store file");
  }
  if (!closed.ok()) {
    return closed.error();
  }
  return buffer;
}

[[nodiscard]] Result<std::uint64_t> parse_sequence_suffix(std::string_view name,
                                                          std::string_view prefix,
                                                          std::string_view suffix) {
  if (name.size() <= prefix.size() + suffix.size()) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "unrecognised store file name");
  }
  if (name.substr(0, prefix.size()) != prefix) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "unrecognised store file prefix");
  }
  if (name.substr(name.size() - suffix.size()) != suffix) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "unrecognised store file suffix");
  }
  const std::string_view digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (digits.empty() || digits.size() > 20) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "unrecognised store file sequence");
  }
  std::uint64_t value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') {
      return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "non-numeric store file sequence");
    }
    if (value > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10ULL) {
      return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "store file sequence overflows");
    }
    value = value * 10ULL + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

void encode_record_header(std::uint8_t* out, RecordType type, const RecordHeader& header, std::uint32_t payload_length) {
  put_u32(out, kRecordMagic);
  out[4] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(type) >> 8);
  out[5] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(type) & 0xFFu);
  out[6] = 0;
  out[7] = 0;
  put_u64(out + 8, header.sequence.value());
  put_u64(out + 16, header.tick);
  put_u64(out + 24, header.epoch.value());
  put_u64(out + 32, header.coordinator.value());
  put_u32(out + 40, payload_length);
}

}  // namespace
const char* to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::PolicySet: return "policy_set";
    case RecordType::CapacityPublished: return "capacity_published";
    case RecordType::ObligationUpserted: return "obligation_upserted";
    case RecordType::ObligationRetired: return "obligation_retired";
    case RecordType::RequestAccepted: return "request_accepted";
    case RecordType::RequestRetired: return "request_retired";
    case RecordType::RoundCommitted: return "round_committed";
    case RecordType::GrantIssued: return "grant_issued";
    case RecordType::GrantUpdated: return "grant_updated";
    case RecordType::GrantReleased: return "grant_released";
    case RecordType::GrantRevoked: return "grant_revoked";
    case RecordType::GrantRevalidated: return "grant_revalidated";
    case RecordType::PublisherFenced: return "publisher_fenced";
    case RecordType::EpochAdvanced: return "epoch_advanced";
    case RecordType::IdempotencyRecord: return "idempotency_record";
    case RecordType::CoordinatorBoot: return "coordinator_boot";
  }
  return "unknown";
}

bool is_known_record_type(std::uint16_t raw) noexcept {
  return raw >= 1 && raw <= 16;
}

struct Store::Impl final {
  std::string directory;
  StoreOptions options;
  RecoveryReport report;
  mutable std::mutex mutex;
  std::FILE* journal{nullptr};
  std::uint64_t journal_first_sequence{0};
  std::uint64_t last_sequence{0};
  std::size_t journal_records{0};
  bool at_capacity{false};
};

Store::Store() : impl_(std::make_unique<Impl>()) {}
Store::~Store() {
  if (impl_ && impl_->journal != nullptr) {
    std::fclose(impl_->journal);
    impl_->journal = nullptr;
  }
}

namespace {

[[nodiscard]] Result<std::uint64_t> read_manifest(const std::string& directory, bool& present) {
  const std::string path = join_path(directory, kManifestName);
  std::error_code error;
  present = fs::exists(path, error);
  if (!present) {
    return std::uint64_t{0};
  }
  const auto bytes = read_file(path, kManifestBytes);
  if (!bytes.ok()) {
    return bytes.error();
  }
  if (bytes.value().size() != kManifestBytes) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "manifest has an unexpected size");
  }
  const std::uint8_t* raw = bytes.value().data();
  if (std::memcmp(raw, kManifestMagic, sizeof(kManifestMagic)) != 0) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "manifest magic mismatch");
  }
  if (get_u32(raw + 8) != BB_STORE_FORMAT_VERSION) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceVersionUnsupported,
                                     "manifest store format version is not supported by this build");
  }
  const std::uint32_t crc = get_u32(raw + kManifestBytes - 4);
  if (Crc32c::compute(raw, kManifestBytes - 4) != crc) {
    return make_error<std::uint64_t>(ErrorCode::PersistenceCorrupt, "manifest checksum mismatch");
  }
  return get_u64(raw + 16);
}

[[nodiscard]] Status write_manifest(const std::string& directory, std::uint64_t snapshot_sequence) {
  std::uint8_t raw[kManifestBytes] = {};
  std::memcpy(raw, kManifestMagic, sizeof(kManifestMagic));
  put_u32(raw + 8, BB_STORE_FORMAT_VERSION);
  put_u32(raw + 12, 0);
  put_u64(raw + 16, snapshot_sequence);
  put_u64(raw + 24, 0);
  put_u64(raw + 32, 0);
  put_u64(raw + 40, 0);
  put_u32(raw + kManifestBytes - 4, Crc32c::compute(raw, kManifestBytes - 4));

  const std::string target = join_path(directory, kManifestName);
  const std::string temporary = target + ".tmp";
  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) {
    return io_error("cannot create manifest");
  }
  BB_RETURN_IF_ERROR(write_all(file, raw, sizeof(raw)));
  if (std::fflush(file) != 0) {
    std::fclose(file);
    return io_error("manifest flush failed");
  }
  BB_RETURN_IF_ERROR(sync_file(file));
  BB_RETURN_IF_ERROR(close_file(file));
  std::error_code error;
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(temporary, error);
    return io_error("manifest rename failed");
  }
  return Status::success();
}

}  // namespace

namespace {

struct SnapshotReadResult final {
  bool ok{false};
  bool corrupt{false};
  SnapshotImage image{};
};

[[nodiscard]] SnapshotReadResult read_snapshot(const std::string& path) {
  SnapshotReadResult result;
  const auto bytes = read_file(path, limits::kMaxStoreFileBytes);
  if (!bytes.ok()) {
    result.corrupt = true;
    return result;
  }
  const auto& raw = bytes.value();
  if (raw.size() < kSnapshotHeaderBytes) {
    result.corrupt = true;
    return result;
  }
  if (std::memcmp(raw.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
    result.corrupt = true;
    return result;
  }
  if (get_u32(raw.data() + 8) != BB_STORE_FORMAT_VERSION) {
    result.corrupt = true;
    return result;
  }
  if (get_u32(raw.data() + 12) != 0) {
    result.corrupt = true;
    return result;
  }
  const std::uint64_t payload_length = get_u64(raw.data() + 48);
  if (get_u32(raw.data() + kSnapshotHeaderBytes - 4) != Crc32c::compute(raw.data(), kSnapshotHeaderBytes - 4)) {
    result.corrupt = true;
    return result;
  }
  if (payload_length > limits::kMaxStoreFileBytes || raw.size() != kSnapshotHeaderBytes + payload_length + 4) {
    result.corrupt = true;
    return result;
  }
  const std::uint32_t payload_crc = get_u32(raw.data() + kSnapshotHeaderBytes + payload_length);
  if (Crc32c::compute(raw.data() + kSnapshotHeaderBytes, static_cast<std::size_t>(payload_length)) != payload_crc) {
    result.corrupt = true;
    return result;
  }
  result.image.sequence = get_u64(raw.data() + 16);
  result.image.header.tick = get_u64(raw.data() + 24);
  result.image.header.epoch = FabricEpoch::from_value(get_u64(raw.data() + 32));
  result.image.header.coordinator = CoordinatorIncarnation::from_value(get_u64(raw.data() + 40));
  result.image.header.sequence = AuditSequence::from_value(result.image.sequence);
  const auto begin = raw.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderBytes);
  result.image.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(payload_length));
  result.ok = true;
  return result;
}

struct JournalScanResult final {
  std::vector<Record> records;
  std::uint64_t damaged_records{0};
  bool truncated{false};
};

[[nodiscard]] Result<JournalScanResult> scan_journal(const std::string& path, std::uint64_t after_sequence) {
  JournalScanResult result;
  const auto bytes = read_file(path, limits::kMaxStoreFileBytes);
  if (!bytes.ok()) {
    return bytes.error();
  }
  const auto& raw = bytes.value();
  if (raw.size() < kJournalHeaderBytes) {
    return make_error<JournalScanResult>(ErrorCode::PersistenceCorrupt, "journal header is truncated");
  }
  if (std::memcmp(raw.data(), kJournalMagic, sizeof(kJournalMagic)) != 0) {
    return make_error<JournalScanResult>(ErrorCode::PersistenceCorrupt, "journal magic mismatch");
  }
  if (get_u32(raw.data() + 8) != BB_STORE_FORMAT_VERSION) {
    return make_error<JournalScanResult>(ErrorCode::PersistenceVersionUnsupported,
                                         "journal store format version is not supported by this build");
  }
  if (get_u32(raw.data() + kJournalHeaderBytes - 4) != Crc32c::compute(raw.data(), kJournalHeaderBytes - 4)) {
    return make_error<JournalScanResult>(ErrorCode::PersistenceCorrupt, "journal header checksum mismatch");
  }

  std::size_t offset = kJournalHeaderBytes;
  while (offset < raw.size()) {
    if (raw.size() - offset < kRecordHeaderBytes) {
      result.truncated = true;
      result.damaged_records += 1;
      break;
    }
    const std::uint8_t* header = raw.data() + offset;
    if (get_u32(header) != kRecordMagic) {
      result.truncated = true;
      result.damaged_records += 1;
      break;
    }
    const std::uint16_t high = static_cast<std::uint16_t>(header[4]);
    const std::uint16_t low = static_cast<std::uint16_t>(header[5]);
    const std::uint16_t type_raw = static_cast<std::uint16_t>((high << 8) | low);
    if (!is_known_record_type(type_raw)) {
      result.truncated = true;
      result.damaged_records += 1;
      break;
    }
    const std::uint32_t payload_length = get_u32(header + 40);
    if (payload_length > kRecordPayloadLimit) {
      result.truncated = true;
      result.damaged_records += 1;
      break;
    }
    const std::size_t total = kRecordHeaderBytes + static_cast<std::size_t>(payload_length) + 4;
    if (raw.size() - offset < total) {
      result.truncated = true;
      result.damaged_records += 1;
      break;
    }
    const std::uint32_t expected_crc = get_u32(raw.data() + offset + kRecordHeaderBytes + payload_length);
    Crc32c crc;
    crc.update(raw.data() + offset, kRecordHeaderBytes);
    crc.update(raw.data() + offset + kRecordHeaderBytes, payload_length);
    if (crc.value() != expected_crc) {
      result.truncated = true;
      result.damaged_records += 1;
      break;
    }
    Record record;
    record.type = static_cast<RecordType>(type_raw);
    record.header.sequence = AuditSequence::from_value(get_u64(header + 8));
    record.header.tick = get_u64(header + 16);
    record.header.epoch = FabricEpoch::from_value(get_u64(header + 24));
    record.header.coordinator = CoordinatorIncarnation::from_value(get_u64(header + 32));
    if (record.header.sequence.value() > after_sequence) {
      const auto begin = raw.begin() + static_cast<std::ptrdiff_t>(offset + kRecordHeaderBytes);
      record.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(payload_length));
      result.records.push_back(std::move(record));
    }
    offset += total;
  }
  return result;
}

}  // namespace

namespace {

[[nodiscard]] Status create_journal(Store::Impl& impl, std::uint64_t first_sequence) {
  const std::string name =
      std::string(kJournalPrefix) + std::to_string(first_sequence) + std::string(kJournalSuffix);
  const std::string path = join_path(impl.directory, name);
  std::FILE* file = std::fopen(path.c_str(), "wb+");
  if (file == nullptr) {
    return io_error("cannot create journal file");
  }
  std::uint8_t header[kJournalHeaderBytes] = {};
  std::memcpy(header, kJournalMagic, sizeof(kJournalMagic));
  put_u32(header + 8, BB_STORE_FORMAT_VERSION);
  put_u32(header + 12, 0);
  put_u64(header + 16, first_sequence);
  put_u32(header + kJournalHeaderBytes - 4, Crc32c::compute(header, kJournalHeaderBytes - 4));
  const auto written = write_all(file, header, sizeof(header));
  if (!written.ok()) {
    std::fclose(file);
    return written.error();
  }
  if (std::fflush(file) != 0) {
    std::fclose(file);
    return io_error("journal flush failed");
  }
  const auto synced = sync_file(file);
  if (!synced.ok()) {
    std::fclose(file);
    return synced.error();
  }
  impl.journal = file;
  impl.journal_first_sequence = first_sequence;
  impl.journal_records = 0;
  return Status::success();
}

}  // namespace

Result<std::unique_ptr<Store>> Store::open(const std::string& directory, const StoreOptions& options) {
  if (directory.empty()) {
    return make_error<std::unique_ptr<Store>>(ErrorCode::InvalidArgument, "store directory must not be empty");
  }
  if (directory.size() > 1024) {
    return make_error<std::unique_ptr<Store>>(ErrorCode::BoundsExceeded, "store directory path is too long");
  }
  std::error_code error;
  const bool existed = fs::exists(directory, error);
  if (!existed) {
    if (!fs::create_directories(directory, error) || error) {
      return make_error<std::unique_ptr<Store>>(ErrorCode::PersistenceIo, "cannot create the store directory");
    }
  } else if (!fs::is_directory(directory, error)) {
    return make_error<std::unique_ptr<Store>>(ErrorCode::PersistenceIo, "store path exists and is not a directory");
  }

  auto store = std::unique_ptr<Store>(new Store());
  store->impl_->directory = directory;
  store->impl_->options = options;
  store->impl_->report.durable = true;
  store->impl_->report.fresh = !existed;
  store->impl_->report.detail = existed ? "store opened" : "store created";
  return store;
}

Result<RecoveryResult> Store::recover() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RecoveryResult result;
  result.report = impl_->report;
  result.report.durable = true;

  bool manifest_present = false;
  const auto manifest = read_manifest(impl_->directory, manifest_present);
  result.report.manifest_present = manifest_present;

  std::vector<std::pair<std::uint64_t, std::string>> snapshots;
  std::vector<std::pair<std::uint64_t, std::string>> journals;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(impl_->directory, error)) {
    if (error) {
      return make_error<RecoveryResult>(ErrorCode::PersistenceIo, "cannot enumerate the store directory");
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const auto snapshot_sequence = parse_sequence_suffix(name, kSnapshotPrefix, kSnapshotSuffix);
    if (snapshot_sequence.ok()) {
      snapshots.emplace_back(snapshot_sequence.value(), entry.path().string());
      continue;
    }
    const auto journal_sequence = parse_sequence_suffix(name, kJournalPrefix, kJournalSuffix);
    if (journal_sequence.ok()) {
      journals.emplace_back(journal_sequence.value(), entry.path().string());
    }
  }
  std::sort(snapshots.begin(), snapshots.end());
  std::sort(journals.begin(), journals.end());

  if (snapshots.empty() && journals.empty()) {
    result.report.fresh = true;
    result.report.detail = "store is empty";
    if (!manifest_present) {
      BB_RETURN_IF_ERROR(write_manifest(impl_->directory, 0));
    }
  } else if (!manifest_present) {
    return make_error<RecoveryResult>(ErrorCode::PersistenceCorrupt,
                                      "store files exist but the manifest is missing");
  }
  if (!manifest.ok()) {
    return make_error<RecoveryResult>(manifest.code(), manifest.error().message);
  }

  for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it) {
    const auto loaded = read_snapshot(it->second);
    if (loaded.ok) {
      result.has_snapshot = true;
      result.snapshot = loaded.image;
      result.report.snapshot_loaded = true;
      result.report.snapshot_sequence = loaded.image.sequence;
      break;
    }
    result.report.snapshot_corrupt = true;
    result.report.detail = "a snapshot failed its integrity check and was skipped";
  }

  const std::uint64_t after = result.has_snapshot ? result.snapshot.sequence : 0;
  for (const auto& entry : journals) {
    if (entry.first == 0) {
      continue;
    }
    const auto scanned = scan_journal(entry.second, after);
    if (!scanned.ok()) {
      return make_error<RecoveryResult>(scanned.code(), scanned.error().message);
    }
    for (auto& record : scanned.value().records) {
      result.records.push_back(std::move(record));
    }
    result.report.records_discarded += scanned.value().damaged_records;
    if (scanned.value().truncated) {
      result.report.journal_truncated = true;
      result.report.truncations += 1;
      break;
    }
  }
  result.report.records_replayed = result.records.size();
  result.report.journal_records = result.records.size();
  if (!result.records.empty()) {
    result.report.last_sequence = result.records.back().header.sequence;
  } else if (result.has_snapshot) {
    result.report.last_sequence = AuditSequence::from_value(result.snapshot.sequence);
  }
  if (result.report.detail.empty()) {
    result.report.detail = "recovery completed";
  }

  impl_->last_sequence = result.report.last_sequence.value();
  impl_->report = result.report;
  impl_->at_capacity = false;

  const std::uint64_t next = impl_->last_sequence + 1;
  const auto created = create_journal(*impl_, next);
  if (!created.ok()) {
    return created.error();
  }
  return result;
}

Status Store::append(RecordType type, const RecordHeader& header, const std::vector<std::uint8_t>& payload) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->journal == nullptr) {
    return make_error_status(ErrorCode::PersistenceIo, "store was not recovered before use");
  }
  if (!header.sequence.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "durable record sequence is not set");
  }
  if (payload.size() > kRecordPayloadLimit) {
    return make_error_status(ErrorCode::BoundsExceeded, "durable record payload exceeds the supported maximum");
  }
  if (impl_->journal_records >= impl_->options.max_journal_records) {
    impl_->at_capacity = true;
    return make_error_status(ErrorCode::ResourceExhausted,
                             "journal is full; compact the store before writing more state");
  }

  std::uint8_t raw[kRecordHeaderBytes] = {};
  encode_record_header(raw, type, header, static_cast<std::uint32_t>(payload.size()));
  Crc32c crc;
  crc.update(raw, sizeof(raw));
  crc.update(payload.data(), payload.size());
  std::uint8_t trailer[4] = {};
  put_u32(trailer, crc.value());

  std::FILE* file = impl_->journal;
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return io_error("cannot seek the journal");
  }
  BB_RETURN_IF_ERROR(write_all(file, raw, sizeof(raw)));
  BB_RETURN_IF_ERROR(write_all(file, payload.data(), payload.size()));
  BB_RETURN_IF_ERROR(write_all(file, trailer, sizeof(trailer)));
  if (std::fflush(file) != 0) {
    return io_error("journal flush failed");
  }
  if (impl_->options.fsync_on_commit) {
    BB_RETURN_IF_ERROR(sync_file(file));
  }
  impl_->journal_records += 1;
  impl_->last_sequence = header.sequence.value();
  return Status::success();
}

Status Store::flush() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->journal == nullptr) {
    return Status::success();
  }
  if (std::fflush(impl_->journal) != 0) {
    return io_error("journal flush failed");
  }
  return sync_file(impl_->journal);
}

bool Store::needs_compaction() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->at_capacity || impl_->journal_records >= impl_->options.compaction_records;
}

std::size_t Store::journal_record_count() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->journal_records;
}

Status Store::compact(const SnapshotImage& image) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (image.payload.size() > limits::kMaxStoreFileBytes) {
    return make_error_status(ErrorCode::BoundsExceeded, "snapshot payload exceeds the supported maximum");
  }
  const std::string name =
      std::string(kSnapshotPrefix) + std::to_string(image.sequence) + std::string(kSnapshotSuffix);
  const std::string target = join_path(impl_->directory, name);
  const std::string temporary = target + ".tmp";

  std::uint8_t header[kSnapshotHeaderBytes] = {};
  std::memcpy(header, kSnapshotMagic, sizeof(kSnapshotMagic));
  put_u32(header + 8, BB_STORE_FORMAT_VERSION);
  put_u32(header + 12, 0);
  put_u64(header + 16, image.sequence);
  put_u64(header + 24, image.header.tick);
  put_u64(header + 32, image.header.epoch.value());
  put_u64(header + 40, image.header.coordinator.value());
  put_u64(header + 48, static_cast<std::uint64_t>(image.payload.size()));
  put_u32(header + kSnapshotHeaderBytes - 4, Crc32c::compute(header, kSnapshotHeaderBytes - 4));

  std::FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) {
    return io_error("cannot create the snapshot file");
  }
  BB_RETURN_IF_ERROR(write_all(file, header, sizeof(header)));
  BB_RETURN_IF_ERROR(write_all(file, image.payload.data(), image.payload.size()));
  std::uint8_t trailer[4] = {};
  put_u32(trailer, Crc32c::compute(image.payload.data(), image.payload.size()));
  BB_RETURN_IF_ERROR(write_all(file, trailer, sizeof(trailer)));
  if (std::fflush(file) != 0) {
    std::fclose(file);
    return io_error("snapshot flush failed");
  }
  BB_RETURN_IF_ERROR(sync_file(file));
  BB_RETURN_IF_ERROR(close_file(file));
  std::error_code error;
  fs::rename(temporary, target, error);
  if (error) {
    fs::remove(temporary, error);
    return io_error("snapshot rename failed");
  }

  BB_RETURN_IF_ERROR(write_manifest(impl_->directory, image.sequence));

  if (impl_->journal != nullptr) {
    std::fclose(impl_->journal);
    impl_->journal = nullptr;
  }
  std::vector<std::string> stale;
  for (const auto& entry : fs::directory_iterator(impl_->directory, error)) {
    if (error) {
      return io_error("cannot enumerate the store directory");
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string entry_name = entry.path().filename().string();
    const auto sequence = parse_sequence_suffix(entry_name, kJournalPrefix, kJournalSuffix);
    if (sequence.ok() && sequence.value() <= image.sequence) {
      stale.push_back(entry.path().string());
    }
  }
  for (const std::string& stale_path : stale) {
    fs::remove(stale_path, error);
  }

  std::vector<std::pair<std::uint64_t, std::string>> snapshots;
  for (const auto& entry : fs::directory_iterator(impl_->directory, error)) {
    if (error) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string entry_name = entry.path().filename().string();
    const auto sequence = parse_sequence_suffix(entry_name, kSnapshotPrefix, kSnapshotSuffix);
    if (sequence.ok()) {
      snapshots.emplace_back(sequence.value(), entry.path().string());
    }
  }
  std::sort(snapshots.begin(), snapshots.end());
  while (snapshots.size() > limits::kMaxSnapshotCount) {
    fs::remove(snapshots.front().second, error);
    snapshots.erase(snapshots.begin());
  }

  impl_->journal_records = 0;
  impl_->at_capacity = false;
  impl_->report.snapshots_written += 1;
  impl_->report.snapshot_sequence = image.sequence;
  impl_->last_sequence = image.sequence;
  return create_journal(*impl_, image.sequence + 1);
}

RecoveryReport Store::report() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RecoveryReport snapshot = impl_->report;
  snapshot.journal_records = impl_->journal_records;
  return snapshot;
}

std::string Store::directory() const { return impl_->directory; }

Result<RecoveryReport> Store::inspect() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RecoveryReport report = impl_->report;
  report.durable = true;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(impl_->directory, error)) {
    if (error) {
      return make_error<RecoveryReport>(ErrorCode::PersistenceIo, "cannot enumerate the store directory");
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const auto snapshot_sequence = parse_sequence_suffix(name, kSnapshotPrefix, kSnapshotSuffix);
    if (snapshot_sequence.ok()) {
      const auto loaded = read_snapshot(entry.path().string());
      if (!loaded.ok) {
        report.snapshot_corrupt = true;
      }
      continue;
    }
    const auto journal_sequence = parse_sequence_suffix(name, kJournalPrefix, kJournalSuffix);
    if (journal_sequence.ok()) {
      const auto scanned = scan_journal(entry.path().string(), 0);
      if (!scanned.ok()) {
        return make_error<RecoveryReport>(scanned.code(), scanned.error().message);
      }
      report.journal_records += scanned.value().records.size();
      if (scanned.value().truncated) {
        report.journal_truncated = true;
      }
    }
  }
  return report;
}

}  // namespace bandwidth_broker
