// Bandwidth Broker - vendor-neutral fabric bandwidth arbitration runtime.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "bandwidth_broker/explanation.hpp"

#include "bandwidth_broker/limits.hpp"
#include "bandwidth_broker/text.hpp"

namespace bandwidth_broker {

Status RequestExplanation::validate() const {
  if (!request.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "explanation request id is not set");
  }
  if (!request_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "explanation request generation is not set");
  }
  BB_RETURN_IF_ERROR(target.validate());
  return Status::success();
}

Status validate_explanation(const RequestExplanation& explanation) {
  if (!explanation.request.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "explanation request id is not set");
  }
  if (!explanation.request_generation.valid()) {
    return make_error_status(ErrorCode::InvalidIdentity, "explanation request generation is not set");
  }
  BB_RETURN_IF_ERROR(explanation.target.validate());
  BB_RETURN_IF_ERROR(validate_text(explanation.binding_reason, limits::kMaxExplanationTextBytes, "binding reason"));
  BB_RETURN_IF_ERROR(
      validate_text(explanation.authority_mismatch, limits::kMaxExplanationTextBytes, "authority mismatch"));
  BB_RETURN_IF_ERROR(validate_text(explanation.provenance.detail, limits::kMaxProvenanceDetailBytes, "provenance detail"));
  return Status::success();
}

std::string describe_explanation(const RequestExplanation& explanation) {
  std::string out;
  out.reserve(512);
  out += "request=";
  out += explanation.request.to_string();
  out += "/";
  out += std::to_string(explanation.request_generation.value());
  out += " target=";
  out += explanation.target.describe();
  out += " state=";
  out += to_string(explanation.state);
  out += " reason=";
  out += to_string(explanation.reason);
  out += " effective=";
  out += explanation.effective_physical.to_string();
  out += " obligations=";
  out += explanation.obligations_applied.to_string();
  out += " arbitrable=";
  out += explanation.arbitrable.to_string();
  out += " guaranteed=";
  out += explanation.guaranteed.to_string();
  out += " discretionary=";
  out += explanation.discretionary.to_string();
  out += " borrowed=";
  out += explanation.borrowed.to_string();
  out += " contingent=";
  out += explanation.contingent.to_string();
  out += " denied=";
  out += explanation.denied.to_string();
  out += " priority=";
  out += explanation.priority.to_string();
  out += " group=";
  out += explanation.fairness_group.to_string();
  out += " policy=";
  out += explanation.policy.to_string();
  out += "/";
  out += std::to_string(explanation.policy_generation.value());
  if (!explanation.binding_reason.empty()) {
    out += " binding=";
    out += explanation.binding_reason;
  }
  return out;
}

}  // namespace bandwidth_broker
