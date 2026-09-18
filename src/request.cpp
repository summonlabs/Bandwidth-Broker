// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/request.hpp"

#include "bandwidth_broker/checksum.hpp"
#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {

const char* to_string(GuaranteeClass value) noexcept {
  switch (value) {
    case GuaranteeClass::Unspecified: return "unspecified";
    case GuaranteeClass::Guaranteed: return "guaranteed";
    case GuaranteeClass::BestEffort: return "best_effort";
  }
  return "unspecified";
}

const char* to_string(RecallTolerance value) noexcept {
  switch (value) {
    case RecallTolerance::Unspecified: return "unspecified";
    case RecallTolerance::Immediate: return "immediate";
    case RecallTolerance::GracePeriod: return "grace_period";
    case RecallTolerance::NoRecall: return "no_recall";
  }
  return "unspecified";
}

GuaranteeClass BandwidthRequest::effective_guarantee_class() const noexcept {
  if (guarantee_class != GuaranteeClass::Unspecified) {
    return guarantee_class;
  }
  return minimum.is_positive() ? GuaranteeClass::Guaranteed : GuaranteeClass::BestEffort;
}

RecallTolerance BandwidthRequest::effective_recall_tolerance() const noexcept {
  if (recall_tolerance != RecallTolerance::Unspecified) {
    return recall_tolerance;
  }
  return preemptible ? RecallTolerance::Immediate : RecallTolerance::NoRecall;
}

bool requester_authority_well_formed(const AuthorityVector& authority) noexcept {
  if (!authority.fabric_epoch.valid()) return false;
  if (!authority.resource.valid() || !authority.resource_generation.valid()) return false;
  if (!authority.policy.valid() || !authority.policy_generation.valid()) return false;
  if (!authority.request.valid() || !authority.request_generation.valid()) return false;
  if (!authority.fairness_group.valid()) return false;
  if (!authority.fairness_config_generation.valid()) return false;
  if (!authority.tenant_config_generation.valid()) return false;
  if (!authority.publisher.valid() || !authority.publisher_boot.valid()) return false;
  if (authority.reservation.valid() != authority.reservation_generation.valid()) return false;
  // Coordinator-owned bindings must be absent in a submission.
  if (authority.coordinator.valid()) return false;
  if (authority.capacity_generation.valid()) return false;
  if (authority.monotonic_tick != 0) return false;
  return true;
}

Status validate_requester_authority(const AuthorityVector& authority) noexcept {
  if (!authority.fabric_epoch.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the fabric epoch");
  }
  if (!authority.resource.valid() || !authority.resource_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the resource binding");
  }
  if (!authority.policy.valid() || !authority.policy_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the policy binding");
  }
  if (!authority.request.valid() || !authority.request_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the request identity");
  }
  if (!authority.fairness_group.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the fairness group");
  }
  if (!authority.fairness_config_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the fairness config generation");
  }
  if (!authority.tenant_config_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the tenant config generation");
  }
  if (!authority.publisher.valid() || !authority.publisher_boot.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request authority is missing the requester identity");
  }
  if (authority.reservation.valid() != authority.reservation_generation.valid()) {
    return make_error_status(ErrorCode::InvalidArgument,
                             "request authority reservation reference and generation must be set together");
  }
  if (authority.coordinator.valid()) {
    return make_error_status(ErrorCode::NotAuthoritative, "a submitted request must not claim a coordinator incarnation");
  }
  if (authority.capacity_generation.valid()) {
    return make_error_status(ErrorCode::NotAuthoritative, "a submitted request must not claim a capacity generation");
  }
  if (authority.monotonic_tick != 0) {
    return make_error_status(ErrorCode::NotAuthoritative, "a submitted request must not claim a coordinator tick");
  }
  return Status::success();
}

Status BandwidthRequest::validate() const {
  if (!id.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request id is not set");
  }
  if (!generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request generation is not set");
  }
  BB_RETURN_IF_ERROR(target.validate());
  BB_RETURN_IF_ERROR(validate_requester_authority(authority));

  if (authority.request != id) {
    return make_error_status(ErrorCode::InvalidArgument, "request authority request id does not match the request id");
  }
  if (authority.request_generation != generation) {
    return make_error_status(ErrorCode::InvalidArgument,
                             "request authority request generation does not match the request generation");
  }
  if (authority.resource != target.resource || authority.resource_generation != target.resource_generation) {
    return make_error_status(ErrorCode::InvalidArgument, "request authority resource does not match the request target");
  }

  if (maximum.is_zero()) {
    return make_error_status(ErrorCode::ContradictoryRequest, "request maximum must be positive");
  }
  if (minimum > maximum) {
    return make_error_status(ErrorCode::ContradictoryRequest, "request minimum exceeds request maximum");
  }
  if (desired > maximum) {
    return make_error_status(ErrorCode::ContradictoryRequest, "request desired exceeds request maximum");
  }
  if (desired < minimum) {
    return make_error_status(ErrorCode::ContradictoryRequest, "request desired is below request minimum");
  }

  if (guarantee_class == GuaranteeClass::Guaranteed && minimum.is_zero()) {
    return make_error_status(ErrorCode::ContradictoryRequest,
                             "a guaranteed request must declare a positive minimum");
  }
  if (guarantee_class == GuaranteeClass::BestEffort && minimum.is_positive()) {
    return make_error_status(ErrorCode::ContradictoryRequest,
                             "a best-effort request must not declare a positive minimum");
  }

  if (borrowing_eligible && !preemptible) {
    return make_error_status(ErrorCode::ContradictoryRequest,
                             "a borrowing request must be preemptible: borrowed bandwidth is always revocable");
  }
  if (borrowing_eligible && recall_tolerance == RecallTolerance::NoRecall) {
    return make_error_status(ErrorCode::ContradictoryRequest,
                             "a borrowing request must not declare NoRecall: borrowed bandwidth is always revocable");
  }
  if (preemptible && recall_tolerance == RecallTolerance::NoRecall) {
    return make_error_status(ErrorCode::ContradictoryRequest,
                             "a preemptible request must not declare NoRecall");
  }
  if (!preemptible && recall_tolerance != RecallTolerance::Unspecified &&
      recall_tolerance != RecallTolerance::NoRecall) {
    return make_error_status(ErrorCode::ContradictoryRequest,
                             "a non-preemptible request must not declare a recall tolerance other than NoRecall");
  }

  if (!priority.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request priority class id is not set");
  }
  if (!tenant.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request tenant id is not set");
  }
  if (!fairness_group.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "request fairness group id is not set");
  }
  if (authority.fairness_group != fairness_group) {
    return make_error_status(ErrorCode::InvalidArgument, "request authority fairness group does not match the request");
  }

  if (reservation.has_value()) {
    if (!reservation->valid()) {
      return make_error_status(ErrorCode::InvalidIdentity, "request reservation binding is incomplete");
    }
    if (effective_guarantee_class() != GuaranteeClass::Guaranteed) {
      return make_error_status(ErrorCode::ContradictoryRequest,
                               "a reservation-bound request must be a guaranteed request");
    }
    if (authority.reservation != reservation->reservation ||
        authority.reservation_generation != reservation->generation) {
      return make_error_status(ErrorCode::InvalidArgument,
                               "request authority reservation does not match the request binding");
    }
  } else if (authority.reservation.valid()) {
    return make_error_status(ErrorCode::InvalidArgument,
                             "request authority declares a reservation but the request has no binding");
  }

  if (window.has_value() && window->end_tick.has_value() && *window->end_tick <= window->start_tick) {
    return make_error_status(ErrorCode::ContradictoryRequest, "request window end tick is not after its start tick");
  }

  if (labels.size() > limits::kMaxPolicyLabels) {
    return make_error_status(ErrorCode::BoundsExceeded, "request carries too many policy labels");
  }
  std::string previous_key;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const PolicyLabel& label = labels[i];
    BB_RETURN_IF_ERROR(validate_text(label.key, limits::kMaxLabelKeyBytes, "policy label key"));
    BB_RETURN_IF_ERROR(validate_text(label.value, limits::kMaxLabelValueBytes, "policy label value"));
    if (label.key.empty()) {
      return make_error_status(ErrorCode::InvalidArgument, "policy label key must not be empty");
    }
    if (i > 0 && !(previous_key < label.key)) {
      return make_error_status(ErrorCode::InvalidArgument,
                               "policy labels must be sorted by key with unique keys");
    }
    previous_key = label.key;
  }

  BB_RETURN_IF_ERROR(validate_text(provenance.detail, limits::kMaxProvenanceDetailBytes, "provenance detail"));
  return Status::success();
}

std::uint64_t BandwidthRequest::content_hash() const {
  ContentHasher hasher;
  hasher.add_u64(id.value());
  hasher.add_u64(generation.value());
  hasher.add_u64(target.resource.value());
  hasher.add_u64(target.resource_generation.value());
  hasher.add_u64(target.pool.value());
  hasher.add_u64(target.pool_generation.value());
  hasher.add_u64(static_cast<std::uint64_t>(minimum.bits_per_second()));
  hasher.add_u64(static_cast<std::uint64_t>(desired.bits_per_second()));
  hasher.add_u64(static_cast<std::uint64_t>(maximum.bits_per_second()));
  hasher.add_u8(static_cast<std::uint8_t>(guarantee_class));
  hasher.add_u64(priority.value());
  hasher.add_u64(tenant.value());
  hasher.add_u64(fairness_group.value());
  hasher.add_bool(borrowing_eligible);
  hasher.add_bool(preemptible);
  hasher.add_u8(static_cast<std::uint8_t>(recall_tolerance));
  hasher.add_bool(reservation.has_value());
  if (reservation.has_value()) {
    hasher.add_u64(reservation->reservation.value());
    hasher.add_u64(reservation->generation.value());
  }
  hasher.add_bool(window.has_value());
  if (window.has_value()) {
    hasher.add_u64(window->start_tick);
    hasher.add_bool(window->end_tick.has_value());
    if (window->end_tick.has_value()) {
      hasher.add_u64(*window->end_tick);
    }
  }
  hasher.add_u64(latency_slo.value());
  hasher.add_u64(labels.size());
  for (const PolicyLabel& label : labels) {
    hasher.add_text(label.key);
    hasher.add_text(label.value);
  }
  hasher.add_u64(authority.fabric_epoch.value());
  hasher.add_u64(authority.policy.value());
  hasher.add_u64(authority.policy_generation.value());
  hasher.add_u64(authority.fairness_config_generation.value());
  hasher.add_u64(authority.tenant_config_generation.value());
  hasher.add_u64(authority.publisher.value());
  hasher.add_u64(authority.publisher_sequence);
  hasher.add_raw(authority.publisher_boot.bytes().data(), authority.publisher_boot.bytes().size());
  return hasher.value();
}

bool BandwidthRequest::operator==(const BandwidthRequest& other) const noexcept {
  return id == other.id && generation == other.generation && target == other.target && minimum == other.minimum &&
         desired == other.desired && maximum == other.maximum && guarantee_class == other.guarantee_class &&
         priority == other.priority && tenant == other.tenant && fairness_group == other.fairness_group &&
         borrowing_eligible == other.borrowing_eligible && preemptible == other.preemptible &&
         recall_tolerance == other.recall_tolerance && reservation == other.reservation && window == other.window &&
         latency_slo == other.latency_slo && labels == other.labels && authority == other.authority;
}

Status validate_request(const BandwidthRequest& request) { return request.validate(); }

}  // namespace bandwidth_broker
