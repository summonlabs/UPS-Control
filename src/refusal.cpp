#include "ups_control/refusal.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace ups_control {

const char* to_string(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::None: return "none";
    case RefusalCode::UnknownUps: return "unknown_ups";
    case RefusalCode::ResourceLimitExceeded: return "resource_limit_exceeded";
    case RefusalCode::StaleHardwareGeneration: return "stale_hardware_generation";
    case RefusalCode::StaleEpoch: return "stale_epoch";
    case RefusalCode::StaleIncarnation: return "stale_incarnation";
    case RefusalCode::StaleRevision: return "stale_revision";
    case RefusalCode::UnsupportedCommand: return "unsupported_command";
    case RefusalCode::AdapterUnavailable: return "adapter_unavailable";
    case RefusalCode::CapabilityUnsupported: return "capability_unsupported";
    case RefusalCode::StateNotRevalidated: return "state_not_revalidated";
    case RefusalCode::OperatingStateUnknown: return "operating_state_unknown";
    case RefusalCode::IllegalOperatingTransition: return "illegal_operating_transition";
    case RefusalCode::LifecycleForbidsControl: return "lifecycle_forbids_control";
    case RefusalCode::LifecycleTransitionIllegal: return "lifecycle_transition_illegal";
    case RefusalCode::StateBasisUnverified: return "state_basis_unverified";
    case RefusalCode::ObligationUnreleased: return "obligation_unreleased";
    case RefusalCode::ObligationExpired: return "obligation_expired";
    case RefusalCode::BypassNotAvailable: return "bypass_not_available";
    case RefusalCode::BypassNotQualified: return "bypass_not_qualified";
    case RefusalCode::OutputNotSynchronized: return "output_not_synchronized";
    case RefusalCode::TransferNotReady: return "transfer_not_ready";
    case RefusalCode::BatteryNotReady: return "battery_not_ready";
    case RefusalCode::ReserveRequirementMissing: return "reserve_requirement_missing";
    case RefusalCode::ReserveEvidenceMissing: return "reserve_evidence_missing";
    case RefusalCode::ReserveEvidenceStale: return "reserve_evidence_stale";
    case RefusalCode::ReserveIndeterminate: return "reserve_indeterminate";
    case RefusalCode::ReserveInsufficient: return "reserve_insufficient";
    case RefusalCode::AuthorityMissing: return "authority_missing";
    case RefusalCode::AuthorityDenied: return "authority_denied";
    case RefusalCode::AuthorityExpired: return "authority_expired";
    case RefusalCode::AuthorityRevoked: return "authority_revoked";
    case RefusalCode::AuthorityFenced: return "authority_fenced";
    case RefusalCode::TransitionInProgress: return "transition_in_progress";
    case RefusalCode::IdempotencyConflict: return "idempotency_conflict";
    case RefusalCode::ContradictoryObservation: return "contradictory_observation";
    case RefusalCode::InternalInvariant: return "internal_invariant";
  }
  return "unknown_refusal_code";
}

StatusCode status_code_of(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::None: return StatusCode::Ok;
    case RefusalCode::UnknownUps: return StatusCode::NotFound;
    case RefusalCode::ResourceLimitExceeded: return StatusCode::LimitExceeded;
    case RefusalCode::StaleHardwareGeneration:
    case RefusalCode::StaleEpoch:
    case RefusalCode::StaleIncarnation:
      return StatusCode::StaleAuthority;
    case RefusalCode::StaleRevision: return StatusCode::StaleSourceGeneration;
    case RefusalCode::UnsupportedCommand: return StatusCode::Unsupported;
    case RefusalCode::AdapterUnavailable: return StatusCode::Unavailable;
    case RefusalCode::CapabilityUnsupported: return StatusCode::CapabilityUnsupported;
    case RefusalCode::StateNotRevalidated: return StatusCode::NotRevalidated;
    case RefusalCode::OperatingStateUnknown: return StatusCode::Unknown;
    case RefusalCode::IllegalOperatingTransition: return StatusCode::IllegalTransition;
    case RefusalCode::LifecycleForbidsControl: return StatusCode::Conflict;
    case RefusalCode::LifecycleTransitionIllegal: return StatusCode::IllegalTransition;
    case RefusalCode::StateBasisUnverified: return StatusCode::Indeterminate;
    case RefusalCode::ObligationUnreleased: return StatusCode::ObligationUnreleased;
    case RefusalCode::ObligationExpired: return StatusCode::ObligationUnreleased;
    case RefusalCode::BypassNotAvailable: return StatusCode::Unavailable;
    case RefusalCode::BypassNotQualified: return StatusCode::PreconditionFailed;
    case RefusalCode::OutputNotSynchronized: return StatusCode::PreconditionFailed;
    case RefusalCode::TransferNotReady: return StatusCode::PreconditionFailed;
    case RefusalCode::BatteryNotReady: return StatusCode::PreconditionFailed;
    case RefusalCode::ReserveRequirementMissing: return StatusCode::EvidenceMissing;
    case RefusalCode::ReserveEvidenceMissing: return StatusCode::EvidenceMissing;
    case RefusalCode::ReserveEvidenceStale: return StatusCode::EvidenceStale;
    case RefusalCode::ReserveIndeterminate: return StatusCode::ReserveIndeterminate;
    case RefusalCode::ReserveInsufficient: return StatusCode::ReserveInsufficient;
    case RefusalCode::AuthorityMissing: return StatusCode::AuthorityMissing;
    case RefusalCode::AuthorityDenied: return StatusCode::AuthorityDenied;
    case RefusalCode::AuthorityExpired: return StatusCode::AuthorityExpired;
    case RefusalCode::AuthorityRevoked: return StatusCode::StaleAuthority;
    case RefusalCode::AuthorityFenced: return StatusCode::StaleAuthority;
    case RefusalCode::TransitionInProgress: return StatusCode::TransitionInProgress;
    case RefusalCode::IdempotencyConflict: return StatusCode::IdempotencyConflict;
    case RefusalCode::ContradictoryObservation: return StatusCode::Indeterminate;
    case RefusalCode::InternalInvariant: return StatusCode::InvariantViolation;
  }
  return StatusCode::InvariantViolation;
}

std::vector<EvaluationFinding> normalize_findings(std::vector<EvaluationFinding> findings,
                                                  const ResourceLimits& limits) {
  std::sort(findings.begin(), findings.end(),
            [](const EvaluationFinding& left, const EvaluationFinding& right) {
              if (left.code != right.code) {
                return static_cast<int>(left.code) < static_cast<int>(right.code);
              }
              return left.detail < right.detail;
            });
  findings.erase(std::unique(findings.begin(), findings.end(),
                             [](const EvaluationFinding& left, const EvaluationFinding& right) {
                               return left.code == right.code && left.detail == right.detail;
                             }),
                 findings.end());
  if (findings.size() > limits.max_findings) {
    findings.resize(limits.max_findings);
  }
  return findings;
}

RefusalCode primary_refusal(const std::vector<EvaluationFinding>& findings) noexcept {
  RefusalCode primary = RefusalCode::None;
  for (const EvaluationFinding& finding : findings) {
    if (finding.code == RefusalCode::None) {
      continue;
    }
    if (primary == RefusalCode::None || static_cast<int>(finding.code) < static_cast<int>(primary)) {
      primary = finding.code;
    }
  }
  return primary;
}

}  // namespace ups_control
