// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/obligation.hpp"

#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {

Status Obligation::validate() const {
  if (!reservation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "obligation reservation reference is not set");
  }
  if (!generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "obligation reservation generation is not set");
  }
  BB_RETURN_IF_ERROR(target.validate());
  if (!amount.is_positive()) {
    return make_error_status(ErrorCode::InvalidArgument, "obligation amount must be positive");
  }
  if (max_lend_permille > limits::kMaxPermille) {
    return make_error_status(ErrorCode::OutOfRange, "obligation lendable permille exceeds 1000");
  }
  if (!lendable && max_lend_permille != 0) {
    return make_error_status(ErrorCode::InvalidArgument, "obligation is not lendable but declares a lendable share");
  }
  if (lendable && max_lend_permille == 0) {
    return make_error_status(ErrorCode::InvalidArgument, "lendable obligation must declare a non-zero lendable share");
  }
  BB_RETURN_IF_ERROR(validate_text(provenance.detail, limits::kMaxProvenanceDetailBytes, "provenance detail"));
  return Status::success();
}

Result<Bandwidth> Obligation::lendable_ceiling() const {
  if (!lendable) {
    return Bandwidth::zero();
  }
  return amount.scaled(max_lend_permille, limits::kMaxPermille);
}

Status validate_obligation(const Obligation& obligation) { return obligation.validate(); }

bool obligation_less(const Obligation& a, const Obligation& b) noexcept {
  if (a.reservation != b.reservation) return a.reservation < b.reservation;
  if (a.generation != b.generation) return a.generation < b.generation;
  return a.target < b.target;
}

}  // namespace bandwidth_broker
