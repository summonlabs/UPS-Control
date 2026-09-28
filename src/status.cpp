#include "ups_control/status.hpp"

namespace ups_control {

const char* to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "ok";
    case StatusCode::InvalidArgument: return "invalid_argument";
    case StatusCode::NotFound: return "not_found";
    case StatusCode::AlreadyExists: return "already_exists";
    case StatusCode::DuplicateIdentity: return "duplicate_identity";
    case StatusCode::Conflict: return "conflict";
    case StatusCode::PreconditionFailed: return "precondition_failed";
    case StatusCode::StaleGeneration: return "stale_generation";
    case StatusCode::StaleAuthority: return "stale_authority";
    case StatusCode::StaleSourceGeneration: return "stale_source_generation";
    case StatusCode::IncompatibleVersion: return "incompatible_version";
    case StatusCode::Corruption: return "corruption";
    case StatusCode::LimitExceeded: return "limit_exceeded";
    case StatusCode::Unsupported: return "unsupported";
    case StatusCode::Unavailable: return "unavailable";
    case StatusCode::Unknown: return "unknown";
    case StatusCode::Indeterminate: return "indeterminate";
    case StatusCode::PermissionDenied: return "permission_denied";
    case StatusCode::IoFailure: return "io_failure";
    case StatusCode::LockConflict: return "lock_conflict";
    case StatusCode::InvariantViolation: return "invariant_violation";
    case StatusCode::Refused: return "refused";
    case StatusCode::IllegalTransition: return "illegal_transition";
    case StatusCode::ObligationUnreleased: return "obligation_unreleased";
    case StatusCode::ReserveInsufficient: return "reserve_insufficient";
    case StatusCode::ReserveIndeterminate: return "reserve_indeterminate";
    case StatusCode::EvidenceMissing: return "evidence_missing";
    case StatusCode::EvidenceStale: return "evidence_stale";
    case StatusCode::NotRevalidated: return "not_revalidated";
    case StatusCode::Closed: return "closed";
    case StatusCode::EndianMismatch: return "endian_mismatch";
    case StatusCode::ReadOnly: return "read_only";
    case StatusCode::AttemptNotFound: return "attempt_not_found";
    case StatusCode::AttemptStateConflict: return "attempt_state_conflict";
    case StatusCode::IdempotencyConflict: return "idempotency_conflict";
    case StatusCode::TransitionInProgress: return "transition_in_progress";
    case StatusCode::CapabilityUnsupported: return "capability_unsupported";
    case StatusCode::AdapterFailure: return "adapter_failure";
    case StatusCode::AuthorityExpired: return "authority_expired";
    case StatusCode::AuthorityMissing: return "authority_missing";
    case StatusCode::AuthorityDenied: return "authority_denied";
  }
  return "unknown_status_code";
}

Status Status::error(StatusCode code, std::string message) {
  Status status;
  status.code_ = code;
  status.message_ = std::move(message);
  return status;
}

std::string Status::to_string() const {
  if (ok()) {
    return "ok";
  }
  std::string text = ups_control::to_string(code_);
  if (!message_.empty()) {
    text += ": ";
    text += message_;
  }
  return text;
}

}  // namespace ups_control
