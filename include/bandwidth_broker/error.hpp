// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef BANDWIDTH_BROKER_ERROR_HPP
#define BANDWIDTH_BROKER_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "bandwidth_broker/export.hpp"

namespace bandwidth_broker {

// Stable numeric error codes. The numbering is part of the durable and wire
// contract: existing values must never be renumbered.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  InvalidIdentity = 2,
  IdentityConflict = 3,
  GenerationMismatch = 4,
  EpochMismatch = 5,
  BootFenced = 6,
  PolicyGenerationMismatch = 7,
  ResourceGenerationMismatch = 8,
  RequesterGenerationMismatch = 9,
  ReservationGenerationMismatch = 10,
  TenantConfigGenerationMismatch = 11,
  CapacityUnknown = 12,
  CapacityInsufficient = 13,
  ContradictoryRequest = 14,
  NumericOverflow = 15,
  OutOfRange = 16,
  BoundsExceeded = 17,
  NotFound = 18,
  AlreadyExists = 19,
  InvalidStateTransition = 20,
  StaleGrant = 21,
  GrantRevoked = 22,
  GrantFenced = 23,
  DuplicateRelease = 24,
  PolicyInvalid = 25,
  PersistenceCorrupt = 26,
  PersistenceIo = 27,
  PersistenceVersionUnsupported = 28,
  ProtocolMalformed = 29,
  ProtocolVersionUnsupported = 30,
  ProtocolPayloadTooLarge = 31,
  ProtocolTruncated = 32,
  ProtocolChecksum = 33,
  HandshakeRejected = 34,
  UnknownSession = 35,
  ConnectionClosed = 36,
  IoError = 37,
  ResourceExhausted = 38,
  Cancelled = 39,
  ShuttingDown = 40,
  InternalError = 41,
  AccountingInvariantViolation = 42,
  NotAuthoritative = 43,
  RevalidationRequired = 44,
  Unsupported = 45,
  NoCapacityAuthority = 46,
  CapacityWithdrawn = 47
};

[[nodiscard]] BB_API const char* to_string(ErrorCode code) noexcept;

// An error value. Messages are bounded by kMaxErrorMessageBytes and are always
// produced from a fixed template plus bounded identifiers, never from unbounded
// external input.
struct BB_API Error final {
  ErrorCode code{ErrorCode::Ok};
  std::string message;

  Error() = default;
  explicit Error(ErrorCode c);
  Error(ErrorCode c, std::string msg);

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
};

// Result of an operation that yields no value.
class BB_API Status final {
 public:
  Status() noexcept = default;
  Status(Error e) noexcept : error_(std::move(e)) {}  // NOLINT(google-explicit-constructor)
  [[nodiscard]] static Status success() noexcept { return Status(); }

  [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code; }
  [[nodiscard]] const std::string& message() const noexcept { return error_.message; }

 private:
  Error error_{};
};

// Result of an operation that yields a value of type T on success.
template <typename T>
class Result final {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code; }

  [[nodiscard]] T& value() noexcept { return *value_; }
  [[nodiscard]] const T& value() const noexcept { return *value_; }
  [[nodiscard]] T&& take() noexcept { return std::move(*value_); }

 private:
  std::optional<T> value_;
  Error error_{};
};

// Propagate a failing Status/Result from a function returning Status or Result<T>.
#define BB_RETURN_IF_ERROR(expr)                    \
  do {                                              \
    const auto bb_status_ = (expr);                 \
    if (!bb_status_.ok()) {                         \
      return bb_status_.error();                    \
    }                                               \
  } while (false)

// Internal invariant failure. Used for conditions that cannot be caused by
// external input; a violation indicates a defect in this runtime.
#define BB_INTERNAL_IF(cond, code) \
  do {                             \
    if (cond) {                    \
      return Error(code);          \
    }                              \
  } while (false)

[[nodiscard]] BB_API Status make_error_status(ErrorCode code, std::string message);
template <typename T>
[[nodiscard]] Result<T> make_error(ErrorCode code, std::string message) {
  return Result<T>(Error(code, std::move(message)));
}

}  // namespace bandwidth_broker

#endif  // BANDWIDTH_BROKER_ERROR_HPP
