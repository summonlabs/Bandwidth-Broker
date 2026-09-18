// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/error.hpp"

namespace bandwidth_broker {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::InvalidIdentity: return "invalid_identity";
    case ErrorCode::IdentityConflict: return "identity_conflict";
    case ErrorCode::GenerationMismatch: return "generation_mismatch";
    case ErrorCode::EpochMismatch: return "epoch_mismatch";
    case ErrorCode::BootFenced: return "boot_fenced";
    case ErrorCode::PolicyGenerationMismatch: return "policy_generation_mismatch";
    case ErrorCode::ResourceGenerationMismatch: return "resource_generation_mismatch";
    case ErrorCode::RequesterGenerationMismatch: return "requester_generation_mismatch";
    case ErrorCode::ReservationGenerationMismatch: return "reservation_generation_mismatch";
    case ErrorCode::TenantConfigGenerationMismatch: return "tenant_config_generation_mismatch";
    case ErrorCode::CapacityUnknown: return "capacity_unknown";
    case ErrorCode::CapacityInsufficient: return "capacity_insufficient";
    case ErrorCode::ContradictoryRequest: return "contradictory_request";
    case ErrorCode::NumericOverflow: return "numeric_overflow";
    case ErrorCode::OutOfRange: return "out_of_range";
    case ErrorCode::BoundsExceeded: return "bounds_exceeded";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::AlreadyExists: return "already_exists";
    case ErrorCode::InvalidStateTransition: return "invalid_state_transition";
    case ErrorCode::StaleGrant: return "stale_grant";
    case ErrorCode::GrantRevoked: return "grant_revoked";
    case ErrorCode::GrantFenced: return "grant_fenced";
    case ErrorCode::DuplicateRelease: return "duplicate_release";
    case ErrorCode::PolicyInvalid: return "policy_invalid";
    case ErrorCode::PersistenceCorrupt: return "persistence_corrupt";
    case ErrorCode::PersistenceIo: return "persistence_io";
    case ErrorCode::PersistenceVersionUnsupported: return "persistence_version_unsupported";
    case ErrorCode::ProtocolMalformed: return "protocol_malformed";
    case ErrorCode::ProtocolVersionUnsupported: return "protocol_version_unsupported";
    case ErrorCode::ProtocolPayloadTooLarge: return "protocol_payload_too_large";
    case ErrorCode::ProtocolTruncated: return "protocol_truncated";
    case ErrorCode::ProtocolChecksum: return "protocol_checksum";
    case ErrorCode::HandshakeRejected: return "handshake_rejected";
    case ErrorCode::UnknownSession: return "unknown_session";
    case ErrorCode::ConnectionClosed: return "connection_closed";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::ResourceExhausted: return "resource_exhausted";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::ShuttingDown: return "shutting_down";
    case ErrorCode::InternalError: return "internal_error";
    case ErrorCode::AccountingInvariantViolation: return "accounting_invariant_violation";
    case ErrorCode::NotAuthoritative: return "not_authoritative";
    case ErrorCode::RevalidationRequired: return "revalidation_required";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::NoCapacityAuthority: return "no_capacity_authority";
    case ErrorCode::CapacityWithdrawn: return "capacity_withdrawn";
  }
  return "unknown_error";
}

Error::Error(ErrorCode c) : code(c), message(to_string(c)) {}

Error::Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {
  if (message.size() > 512) {
    message.resize(512);
  }
}

Status make_error_status(ErrorCode code, std::string message) { return Status(Error(code, std::move(message))); }

}  // namespace bandwidth_broker
