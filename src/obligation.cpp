#include "ups_control/obligation.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace ups_control {

const char* to_string(ObligationTier tier) noexcept {
  switch (tier) {
    case ObligationTier::LifeSafety: return "life_safety";
    case ObligationTier::Critical: return "critical";
    case ObligationTier::Essential: return "essential";
    case ObligationTier::NonEssential: return "non_essential";
  }
  return "unknown";
}

Result<ObligationTier> parse_obligation_tier(std::string_view text) {
  if (text == "life_safety") {
    return ObligationTier::LifeSafety;
  }
  if (text == "critical") {
    return ObligationTier::Critical;
  }
  if (text == "essential") {
    return ObligationTier::Essential;
  }
  if (text == "non_essential") {
    return ObligationTier::NonEssential;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown obligation tier '" + std::string(text) + "'");
}

const char* to_string(ProtectionRequirement requirement) noexcept {
  switch (requirement) {
    case ProtectionRequirement::MustRemainProtected: return "must_remain_protected";
    case ProtectionRequirement::MayBeInterruptedWithAuthority: return "may_be_interrupted_with_authority";
    case ProtectionRequirement::Informational: return "informational";
  }
  return "unknown";
}

Result<ProtectionRequirement> parse_protection_requirement(std::string_view text) {
  if (text == "must_remain_protected") {
    return ProtectionRequirement::MustRemainProtected;
  }
  if (text == "may_be_interrupted_with_authority") {
    return ProtectionRequirement::MayBeInterruptedWithAuthority;
  }
  if (text == "informational") {
    return ProtectionRequirement::Informational;
  }
  return Status::error(StatusCode::InvalidArgument,
                       "unknown protection requirement '" + std::string(text) + "'");
}

const char* to_string(ObligationState state) noexcept {
  switch (state) {
    case ObligationState::Active: return "active";
    case ObligationState::Suspended: return "suspended";
    case ObligationState::Released: return "released";
  }
  return "unknown";
}

Status validate_obligation(const ProtectedLoadObligation& obligation,
                           const ResourceLimits& limits) {
  Status status = validate_identifier(obligation.ref.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "obligation reference: " + status.message());
  }
  status = validate_identifier(obligation.load.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "obligation load: " + status.message());
  }
  status = validate_identifier(obligation.asserted_by.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "obligation asserting authority: " + status.message());
  }
  if (!is_valid_instant(obligation.asserted_at)) {
    return Status::error(StatusCode::InvalidArgument,
                         "obligation must carry a positive assertion instant");
  }
  if (obligation.expires_at.has_value()) {
    if (!is_valid_instant(obligation.expires_at.value())) {
      return Status::error(StatusCode::InvalidArgument,
                           "obligation expiry must be a positive instant");
    }
    if (obligation.expires_at.value().value() <= obligation.asserted_at.value()) {
      return Status::error(StatusCode::InvalidArgument,
                           "obligation expiry must be strictly after its assertion instant");
    }
  }
  if (obligation.state == ObligationState::Released) {
    if (!obligation.released_by.has_value()) {
      return Status::error(StatusCode::InvalidArgument,
                           "a released obligation must record the releasing authority");
    }
    if (!obligation.released_at.has_value() || !is_valid_instant(obligation.released_at.value())) {
      return Status::error(StatusCode::InvalidArgument,
                           "a released obligation must record a positive release instant");
    }
    status = validate_identifier(obligation.released_by->value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "obligation releasing authority: " + status.message());
    }
  }
  status = validate_label(obligation.release_reason, limits.max_detail_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "obligation release reason: " + status.message());
  }
  return Status::success();
}

bool obligation_binds(const ProtectedLoadObligation& obligation, Tick now) noexcept {
  if (obligation.state != ObligationState::Active) {
    return false;
  }
  if (!obligation.expires_at.has_value()) {
    return true;
  }
  return now.value() < obligation.expires_at->value();
}

bool obligation_lapsed(const ProtectedLoadObligation& obligation, Tick now) noexcept {
  if (obligation.state != ObligationState::Active || !obligation.expires_at.has_value()) {
    return false;
  }
  return now.value() >= obligation.expires_at->value();
}

bool obligation_is_protective(const ProtectedLoadObligation& obligation) noexcept {
  return obligation.protection != ProtectionRequirement::Informational;
}

const char* to_string(ProtectionImpact impact) noexcept {
  switch (impact) {
    case ProtectionImpact::Preserves: return "preserves";
    case ProtectionImpact::ReducedWithAuthority: return "reduced_with_authority";
    case ProtectionImpact::DropsUndeclared: return "drops_undeclared";
    case ProtectionImpact::Unknown: return "unknown";
  }
  return "unknown";
}

ProtectedImpactReport assess_protected_impact(
    const std::vector<ProtectedLoadObligation>& obligations,
    const std::optional<AuthorityRef>& release_authority, Tick now, const ResourceLimits& limits) {
  ProtectedImpactReport report;

  // The function is pure and takes the obligations in whatever order the caller
  // holds them, so the lists it produces are put into reference order before any
  // bound is applied. That makes the report deterministic for equal inputs
  // regardless of input order, which is what the header promises.
  std::vector<const ProtectedLoadObligation*> ordered;
  ordered.reserve(obligations.size());
  for (const ProtectedLoadObligation& obligation : obligations) {
    ordered.push_back(&obligation);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const ProtectedLoadObligation* left, const ProtectedLoadObligation* right) {
              return left->ref < right->ref;
            });

  for (const ProtectedLoadObligation* entry : ordered) {
    const ProtectedLoadObligation& obligation = *entry;
    if (!obligation_is_protective(obligation)) {
      continue;
    }
    const bool lapsed = obligation_lapsed(obligation, now);
    if (lapsed) {
      report.lapsed.push_back(obligation.ref);
    }
    if (obligation_binds(obligation, now)) {
      report.binding.push_back(obligation.ref);
    }

    // A released or suspended obligation no longer asserts protection.
    if (obligation.state != ObligationState::Active) {
      continue;
    }

    // The obligation is still asserting protection, so a protection-dropping
    // transition puts it at risk.
    report.at_risk.push_back(obligation.ref);

    bool covered = false;
    if (!lapsed && obligation.protection == ProtectionRequirement::MayBeInterruptedWithAuthority &&
        release_authority.has_value() && release_authority.value() == obligation.asserted_by) {
      // The asserting authority itself permits the interruption.
      covered = true;
    }
    if (!covered) {
      report.unreleased.push_back(obligation.ref);
    }
  }

  if (report.at_risk.size() > limits.max_obligations_at_risk) {
    report.at_risk.resize(limits.max_obligations_at_risk);
  }
  if (report.unreleased.size() > limits.max_obligations_at_risk) {
    report.unreleased.resize(limits.max_obligations_at_risk);
  }
  if (report.binding.size() > limits.max_obligations_at_risk) {
    report.binding.resize(limits.max_obligations_at_risk);
  }
  if (report.lapsed.size() > limits.max_obligations_at_risk) {
    report.lapsed.resize(limits.max_obligations_at_risk);
  }

  if (!report.unreleased.empty()) {
    report.impact = ProtectionImpact::DropsUndeclared;
  } else if (!report.at_risk.empty()) {
    report.impact = ProtectionImpact::ReducedWithAuthority;
  } else {
    report.impact = ProtectionImpact::Preserves;
  }
  return report;
}

}  // namespace ups_control
