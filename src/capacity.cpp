// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/capacity.hpp"

#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {

const char* to_string(CapacityEvidenceState state) noexcept {
  switch (state) {
    case CapacityEvidenceState::Unknown: return "unknown";
    case CapacityEvidenceState::Known: return "known";
    case CapacityEvidenceState::Stale: return "stale";
  }
  return "unknown";
}

bool CapacityTarget::valid() const noexcept {
  if (!resource.valid() || !resource_generation.valid()) {
    return false;
  }
  if (pool.valid() != pool_generation.valid()) {
    return false;
  }
  return true;
}

Status CapacityTarget::validate() const {
  if (!resource.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity target resource id is not set");
  }
  if (!resource_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity target resource generation is not set");
  }
  if (pool.valid() && !pool_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity target pool generation is not set");
  }
  if (!pool.valid() && pool_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity target pool generation is set without a pool id");
  }
  return Status::success();
}

std::string CapacityTarget::describe() const {
  std::string out;
  out.reserve(64);
  out += "resource=";
  out += resource.to_string();
  out += "/gen=";
  out += std::to_string(resource_generation.value());
  if (has_pool()) {
    out += ",pool=";
    out += pool.to_string();
    out += "/gen=";
    out += std::to_string(pool_generation.value());
  }
  return out;
}

Status CapacitySnapshot::validate() const {
  BB_RETURN_IF_ERROR(target.validate());
  if (!snapshot.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity snapshot id is not set");
  }
  if (!generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity snapshot generation is not set");
  }
  if (!authority.fabric_epoch.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity snapshot fabric epoch is not set");
  }
  if (!authority.coordinator.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity snapshot coordinator incarnation is not set");
  }
  if (!authority.publisher.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity snapshot publisher id is not set");
  }
  if (!authority.publisher_boot.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "capacity snapshot publisher boot identity is not set");
  }
  if (authority.resource != target.resource) {
    return make_error_status(ErrorCode::InvalidArgument, "capacity snapshot authority resource does not match its target");
  }
  if (authority.resource_generation != target.resource_generation) {
    return make_error_status(ErrorCode::InvalidArgument,
                             "capacity snapshot authority resource generation does not match its target");
  }
  if (authority.capacity_generation != generation) {
    return make_error_status(ErrorCode::InvalidArgument,
                             "capacity snapshot authority capacity generation does not match its generation");
  }
  if (authority.publisher_sequence != publisher_sequence) {
    return make_error_status(ErrorCode::InvalidArgument,
                             "capacity snapshot authority publisher sequence does not match its sequence");
  }
  if (evidence == CapacityEvidenceState::Known && !physical_configured.has_value()) {
    return make_error_status(ErrorCode::InvalidArgument, "known capacity evidence must carry physical capacity");
  }
  if (!physical_configured.has_value()) {
    if (!administratively_unavailable.is_zero() || !degraded_loss.is_zero() || !headroom_target.is_zero() ||
        !reserved_committed.is_zero()) {
      return make_error_status(ErrorCode::InvalidArgument,
                               "unknown capacity evidence must not carry capacity amounts");
    }
  } else {
    if (administratively_unavailable > *physical_configured) {
      return make_error_status(ErrorCode::InvalidArgument,
                               "administratively unavailable capacity exceeds physical capacity");
    }
    const auto after_admin = physical_configured->checked_sub(administratively_unavailable);
    if (!after_admin.ok()) {
      return after_admin.error();
    }
    if (degraded_loss > after_admin.value()) {
      return make_error_status(ErrorCode::InvalidArgument, "degraded loss exceeds the remaining physical capacity");
    }
  }
  BB_RETURN_IF_ERROR(validate_text(provenance.detail, limits::kMaxProvenanceDetailBytes, "provenance detail"));
  return Status::success();
}

Result<Bandwidth> CapacitySnapshot::effective_physical() const {
  if (!is_usable()) {
    return make_error<Bandwidth>(ErrorCode::CapacityUnknown,
                                 "capacity evidence is not current; unknown capacity is not spare capacity");
  }
  const auto after_admin = physical_configured->checked_sub(administratively_unavailable);
  if (!after_admin.ok()) {
    return after_admin.error();
  }
  return after_admin.value().checked_sub(degraded_loss);
}

Status validate_capacity_snapshot(const CapacitySnapshot& snapshot) { return snapshot.validate(); }

Result<Bandwidth> combine_headroom(Bandwidth target, Bandwidth permille_share) noexcept {
  return Bandwidth::max_of(target, permille_share);
}

}  // namespace bandwidth_broker
