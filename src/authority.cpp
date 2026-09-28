#include "ups_control/authority.hpp"

#include <algorithm>
#include <string>

namespace ups_control {

const char* to_string(GrantScope scope) noexcept {
  switch (scope) {
    case GrantScope::BypassTransfer: return "bypass_transfer";
    case GrantScope::MaintenanceEntry: return "maintenance_entry";
    case GrantScope::TestExecution: return "test_execution";
    case GrantScope::RechargeEnable: return "recharge_enable";
    case GrantScope::DischargeEnable: return "discharge_enable";
    case GrantScope::IsolationControl: return "isolation_control";
    case GrantScope::FaultClear: return "fault_clear";
    case GrantScope::PolicyChange: return "policy_change";
  }
  return "unknown";
}

Result<GrantScope> parse_grant_scope(std::string_view text) {
  if (text == "bypass_transfer") {
    return GrantScope::BypassTransfer;
  }
  if (text == "maintenance_entry") {
    return GrantScope::MaintenanceEntry;
  }
  if (text == "test_execution") {
    return GrantScope::TestExecution;
  }
  if (text == "recharge_enable") {
    return GrantScope::RechargeEnable;
  }
  if (text == "discharge_enable") {
    return GrantScope::DischargeEnable;
  }
  if (text == "isolation_control") {
    return GrantScope::IsolationControl;
  }
  if (text == "fault_clear") {
    return GrantScope::FaultClear;
  }
  if (text == "policy_change") {
    return GrantScope::PolicyChange;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown grant scope '" + std::string(text) + "'");
}

Status validate_grant(const AuthorityGrant& grant, const ResourceLimits& limits) {
  Status status = validate_identifier(grant.ref.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "grant reference: " + status.message());
  }
  status = validate_identifier(grant.granted_by.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "granting authority: " + status.message());
  }
  if (!is_valid_instant(grant.issued_at)) {
    return Status::error(StatusCode::InvalidArgument, "grant must carry a positive issue instant");
  }
  if (grant.expires_at.has_value()) {
    if (!is_valid_instant(grant.expires_at.value())) {
      return Status::error(StatusCode::InvalidArgument, "grant expiry must be a positive instant");
    }
    if (grant.expires_at.value().value() <= grant.issued_at.value()) {
      return Status::error(StatusCode::InvalidArgument,
                           "grant expiry must be strictly after its issue instant");
    }
  }
  if (grant.reserve_floor.has_value()) {
    status = validate_reserve_quantity(grant.reserve_floor.value(), limits);
    if (!status.ok()) {
      return Status::error(status.code(), "grant reserve floor: " + status.message());
    }
  }
  // A revocation is recorded as a whole: a revoking authority without a revocation
  // instant is a half-written record that would report as Allowed while looking
  // revoked, so it is refused in either direction.
  if (grant.revoked_by.has_value() && !grant.revoked_at.has_value()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a grant that records a revoking authority must also record the "
                         "revocation instant");
  }
  if (grant.revoked_at.has_value()) {
    if (!grant.revoked_by.has_value()) {
      return Status::error(StatusCode::InvalidArgument,
                           "a revoked grant must record the revoking authority");
    }
    if (!is_valid_instant(grant.revoked_at.value())) {
      return Status::error(StatusCode::InvalidArgument,
                           "a revoked grant must record a positive revocation instant");
    }
    status = validate_identifier(grant.revoked_by->value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "revoking authority: " + status.message());
    }
  }
  return validate_label(grant.note, limits.max_detail_bytes);
}

const char* to_string(GrantVerdict verdict) noexcept {
  switch (verdict) {
    case GrantVerdict::Allowed: return "allowed";
    case GrantVerdict::Denied: return "denied";
    case GrantVerdict::Missing: return "missing";
    case GrantVerdict::Expired: return "expired";
    case GrantVerdict::Revoked: return "revoked";
    case GrantVerdict::Fenced: return "fenced";
    case GrantVerdict::OutOfScope: return "out_of_scope";
  }
  return "unknown";
}

GrantAssessment assess_grant(const std::vector<AuthorityGrant>& grants,
                             const std::optional<AuthorityRef>& cited, GrantScope scope,
                             const ControlContext& context, Tick now) {
  GrantAssessment assessment;
  if (!cited.has_value()) {
    assessment.verdict = GrantVerdict::Missing;
    assessment.detail = std::string("no authority was cited for the ") + to_string(scope) +
                        " scope, so permission cannot be established";
    return assessment;
  }
  assessment.grant = cited;

  const auto found = std::find_if(grants.begin(), grants.end(),
                                  [&cited](const AuthorityGrant& grant) {
                                    return grant.ref == cited.value();
                                  });
  if (found == grants.end()) {
    assessment.verdict = GrantVerdict::Missing;
    assessment.detail = "no grant '" + cited->value() + "' is bound to this unit";
    return assessment;
  }

  if (found->scope != scope) {
    assessment.verdict = GrantVerdict::OutOfScope;
    assessment.detail = "grant '" + cited->value() + "' covers " + to_string(found->scope) +
                        " and does not authorize " + to_string(scope) +
                        "; a neighbouring scope is never substituted";
    return assessment;
  }

  if (found->revoked_at.has_value()) {
    assessment.verdict = GrantVerdict::Revoked;
    assessment.detail = "grant '" + cited->value() + "' was revoked at instant " +
                        std::to_string(found->revoked_at->value());
    return assessment;
  }

  if (found->epoch != context.epoch || found->incarnation != context.incarnation) {
    assessment.verdict = GrantVerdict::Fenced;
    assessment.detail = "grant '" + cited->value() + "' was issued under epoch " +
                        std::to_string(found->epoch.value()) + " incarnation " +
                        std::to_string(found->incarnation.value()) + " and is fenced by the current epoch " +
                        std::to_string(context.epoch.value()) + " incarnation " +
                        std::to_string(context.incarnation.value());
    return assessment;
  }

  if (found->expires_at.has_value() && now.value() >= found->expires_at->value()) {
    assessment.verdict = GrantVerdict::Expired;
    assessment.detail = "grant '" + cited->value() + "' lapsed at instant " +
                        std::to_string(found->expires_at->value()) +
                        " and no longer authorizes anything";
    return assessment;
  }

  if (!found->allowed) {
    assessment.verdict = GrantVerdict::Denied;
    assessment.detail = "grant '" + cited->value() + "' explicitly denies " + to_string(scope);
    return assessment;
  }

  assessment.verdict = GrantVerdict::Allowed;
  assessment.detail = "grant '" + cited->value() + "' authorizes " + to_string(scope) +
                      " under epoch " + std::to_string(context.epoch.value()) + " incarnation " +
                      std::to_string(context.incarnation.value());
  return assessment;
}

}  // namespace ups_control
