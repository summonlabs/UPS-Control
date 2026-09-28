#include "ups_control/command.hpp"

#include <string>

#include "ups_control/arith.hpp"

namespace ups_control {

const char* to_string(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::EnterStaticBypass: return "enter_static_bypass";
    case CommandKind::LeaveStaticBypass: return "leave_static_bypass";
    case CommandKind::EnterMaintenanceBypass: return "enter_maintenance_bypass";
    case CommandKind::LeaveMaintenanceBypass: return "leave_maintenance_bypass";
    case CommandKind::StartSelfTest: return "start_self_test";
    case CommandKind::StartBatteryTest: return "start_battery_test";
    case CommandKind::AbortTest: return "abort_test";
    case CommandKind::EnableRecharge: return "enable_recharge";
    case CommandKind::DisableRecharge: return "disable_recharge";
    case CommandKind::EnableDischarge: return "enable_discharge";
    case CommandKind::DisableDischarge: return "disable_discharge";
    case CommandKind::IsolateOutput: return "isolate_output";
    case CommandKind::ReturnFromIsolation: return "return_from_isolation";
    case CommandKind::ClearFaults: return "clear_faults";
  }
  return "unknown_command";
}

Result<CommandKind> parse_command_kind(std::string_view text) {
  for (int index = 1; index <= static_cast<int>(CommandKind::ClearFaults); ++index) {
    const auto candidate = static_cast<CommandKind>(index);
    if (text == to_string(candidate)) {
      return candidate;
    }
  }
  return Status::error(StatusCode::Unsupported,
                       "unsupported command '" + std::string(text) +
                           "'; this runtime never maps an unknown command onto a nearby "
                           "operation");
}

namespace {

bool is_documented(CommandKind kind) noexcept {
  return static_cast<int>(kind) >= static_cast<int>(CommandKind::EnterStaticBypass) &&
         static_cast<int>(kind) <= static_cast<int>(CommandKind::ClearFaults);
}

}  // namespace

CommandFamily command_family(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::EnableRecharge:
    case CommandKind::DisableRecharge:
    case CommandKind::EnableDischarge:
    case CommandKind::DisableDischarge:
    case CommandKind::ClearFaults:
      return CommandFamily::Capability;
    default:
      return CommandFamily::Transition;
  }
}

Result<std::optional<OperatingState>> canonical_target(CommandKind kind) {
  if (!is_documented(kind)) {
    return Status::error(StatusCode::Unsupported,
                         "command value " + std::to_string(static_cast<int>(kind)) +
                             " is not a documented command kind");
  }
  switch (kind) {
    case CommandKind::EnterStaticBypass: return std::optional<OperatingState>(OperatingState::StaticBypass);
    case CommandKind::LeaveStaticBypass: return std::optional<OperatingState>(OperatingState::OnlineNormal);
    case CommandKind::EnterMaintenanceBypass:
      return std::optional<OperatingState>(OperatingState::MaintenanceBypass);
    case CommandKind::LeaveMaintenanceBypass:
      return std::optional<OperatingState>(OperatingState::OnlineNormal);
    case CommandKind::StartSelfTest: return std::optional<OperatingState>(OperatingState::SelfTest);
    case CommandKind::StartBatteryTest: return std::optional<OperatingState>(OperatingState::BatteryTest);
    case CommandKind::AbortTest: return std::optional<OperatingState>(OperatingState::OnlineNormal);
    case CommandKind::IsolateOutput: return std::optional<OperatingState>(OperatingState::Isolated);
    case CommandKind::ReturnFromIsolation: return std::optional<OperatingState>(OperatingState::Standby);
    default: return std::optional<OperatingState>{};
  }
}

std::vector<OperatingState> accepted_effect_states(CommandKind kind) {
  switch (kind) {
    case CommandKind::EnterStaticBypass:
      return {OperatingState::StaticBypass};
    case CommandKind::LeaveStaticBypass:
      return {OperatingState::OnlineNormal, OperatingState::Recharge};
    case CommandKind::EnterMaintenanceBypass:
      return {OperatingState::MaintenanceBypass};
    case CommandKind::LeaveMaintenanceBypass:
      return {OperatingState::OnlineNormal, OperatingState::StaticBypass, OperatingState::Recharge};
    case CommandKind::StartSelfTest:
      return {OperatingState::SelfTest};
    case CommandKind::StartBatteryTest:
      return {OperatingState::BatteryTest};
    case CommandKind::AbortTest:
      return {OperatingState::OnlineNormal, OperatingState::Recharge, OperatingState::OnlineBattery};
    case CommandKind::IsolateOutput:
      return {OperatingState::Isolated};
    case CommandKind::ReturnFromIsolation:
      return {OperatingState::Standby, OperatingState::Offline};
    default:
      return {};
  }
}

Result<GrantScope> required_scope(CommandKind kind) {
  if (!is_documented(kind)) {
    return Status::error(StatusCode::Unsupported,
                         "command value " + std::to_string(static_cast<int>(kind)) +
                             " is not a documented command kind");
  }
  switch (kind) {
    case CommandKind::EnterStaticBypass:
    case CommandKind::LeaveStaticBypass:
      return GrantScope::BypassTransfer;
    case CommandKind::EnterMaintenanceBypass:
    case CommandKind::LeaveMaintenanceBypass:
      return GrantScope::MaintenanceEntry;
    case CommandKind::StartSelfTest:
    case CommandKind::StartBatteryTest:
    case CommandKind::AbortTest:
      return GrantScope::TestExecution;
    case CommandKind::EnableRecharge:
    case CommandKind::DisableRecharge:
      return GrantScope::RechargeEnable;
    case CommandKind::EnableDischarge:
    case CommandKind::DisableDischarge:
      return GrantScope::DischargeEnable;
    case CommandKind::IsolateOutput:
    case CommandKind::ReturnFromIsolation:
      return GrantScope::IsolationControl;
    case CommandKind::ClearFaults:
      return GrantScope::FaultClear;
  }
  return Status::error(StatusCode::Unsupported, "command has no documented scope");
}

bool requires_reserve(CommandKind kind) noexcept {
  return kind == CommandKind::StartBatteryTest || kind == CommandKind::EnableDischarge;
}

bool drops_protection(CommandKind kind) noexcept {
  return kind == CommandKind::EnterMaintenanceBypass || kind == CommandKind::IsolateOutput;
}

std::string describe_command(CommandKind kind) {
  const Result<std::optional<OperatingState>> target = canonical_target(kind);
  if (!target.ok()) {
    return std::string(to_string(kind));
  }
  if (!target.value().has_value()) {
    return std::string(to_string(kind)) + " (capability)";
  }
  return std::string(to_string(kind)) + " -> " + to_string(target.value().value());
}

Status validate_command_parameters(const CommandParameters& parameters,
                                   const ResourceLimits& limits) {
  if (parameters.verification_dwell.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "the verification dwell must not be negative");
  }
  if (parameters.reserve_requirement.has_value()) {
    const Status status = validate_reserve_quantity(parameters.reserve_requirement->minimum, limits);
    if (!status.ok()) {
      return Status::error(status.code(), "command reserve requirement: " + status.message());
    }
  }
  return Status::success();
}

const char* to_string(AttemptPhase phase) noexcept {
  switch (phase) {
    case AttemptPhase::Planned: return "planned";
    case AttemptPhase::Refused: return "refused";
    case AttemptPhase::Issued: return "issued";
    case AttemptPhase::Acknowledged: return "acknowledged";
    case AttemptPhase::Observed: return "observed";
    case AttemptPhase::Verified: return "verified";
    case AttemptPhase::Failed: return "failed";
    case AttemptPhase::Unsupported: return "unsupported";
  }
  return "unknown";
}

bool is_terminal(AttemptPhase phase) noexcept {
  switch (phase) {
    case AttemptPhase::Refused:
    case AttemptPhase::Verified:
    case AttemptPhase::Failed:
    case AttemptPhase::Unsupported:
      return true;
    default:
      return false;
  }
}

bool is_unresolved(AttemptPhase phase) noexcept {
  return !is_terminal(phase);
}

const char* to_string(AckOutcome outcome) noexcept {
  switch (outcome) {
    case AckOutcome::None: return "none";
    case AckOutcome::Accepted: return "accepted";
    case AckOutcome::Rejected: return "rejected";
    case AckOutcome::Unsupported: return "unsupported";
    case AckOutcome::NoResponse: return "no_response";
    case AckOutcome::Malformed: return "malformed";
  }
  return "unknown";
}

const char* to_string(ObservedOutcome outcome) noexcept {
  switch (outcome) {
    case ObservedOutcome::None: return "none";
    case ObservedOutcome::MatchesTarget: return "matches_target";
    case ObservedOutcome::DifferentState: return "different_state";
    case ObservedOutcome::Contradictory: return "contradictory";
    case ObservedOutcome::Unavailable: return "unavailable";
  }
  return "unknown";
}

const char* to_string(VerificationVerdict verdict) noexcept {
  switch (verdict) {
    case VerificationVerdict::NotEvaluated: return "not_evaluated";
    case VerificationVerdict::Verified: return "verified";
    case VerificationVerdict::Unverified: return "unverified";
    case VerificationVerdict::Contradicted: return "contradicted";
    case VerificationVerdict::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

const char* to_string(EvidenceClass evidence) noexcept {
  switch (evidence) {
    case EvidenceClass::Unsupported: return "UNSUPPORTED";
    case EvidenceClass::Synthetic: return "SYNTHETIC";
    case EvidenceClass::Real: return "REAL";
  }
  return "UNSUPPORTED";
}

bool capability_effect_matches(CommandKind kind, Bool3 observed) noexcept {
  switch (kind) {
    case CommandKind::EnableRecharge:
    case CommandKind::EnableDischarge:
      return observed == Bool3::True;
    case CommandKind::DisableRecharge:
    case CommandKind::DisableDischarge:
      return observed == Bool3::False;
    case CommandKind::ClearFaults:
      // The observed value passed for ClearFaults is the reported fault presence.
      return observed == Bool3::False;
    default:
      return false;
  }
}

namespace {

Bool3 capability_observation(CommandKind kind, const ObservationRecord& observation) noexcept {
  switch (kind) {
    case CommandKind::EnableRecharge:
    case CommandKind::DisableRecharge:
      return observation.capability.recharge_enabled;
    case CommandKind::EnableDischarge:
    case CommandKind::DisableDischarge:
      return observation.capability.discharge_enabled;
    case CommandKind::ClearFaults:
      return observation.fault_present;
    default:
      return Bool3::Unknown;
  }
}

}  // namespace

FreshnessPolicy default_verification_freshness() {
  FreshnessPolicy policy;
  policy.max_age = TickSpan{300};
  policy.max_future_skew = TickSpan{0};
  return policy;
}

VerificationVerdict verify_attempt(const AttemptRecord& attempt,
                                   const ObservationRecord& observation, Tick now,
                                   const FreshnessPolicy& freshness, std::string& detail) {
  if (attempt.phase != AttemptPhase::Acknowledged && attempt.phase != AttemptPhase::Observed) {
    detail = std::string("the attempt is ") + to_string(attempt.phase) +
             " and cannot be verified from this phase";
    return VerificationVerdict::Indeterminate;
  }
  if (attempt.phase == AttemptPhase::Acknowledged || attempt.ack == AckOutcome::NoResponse) {
    // Verification is still permitted after a lost acknowledgement: the device may
    // have acted. The observation must nonetheless be newer than the issuance.
    if (!is_valid_instant(observation.observed_at)) {
      detail = "the observation carries no usable instant";
      return VerificationVerdict::Indeterminate;
    }
    if (attempt.acknowledged_at.value() > 0 &&
        observation.observed_at.value() <= attempt.acknowledged_at.value()) {
      detail = "the observation is not newer than the acknowledgement";
      return VerificationVerdict::Indeterminate;
    }
    if (attempt.acknowledged_at.value() == 0 &&
        observation.observed_at.value() <= attempt.submitted_at.value()) {
      detail = "the observation is not newer than the issuance";
      return VerificationVerdict::Indeterminate;
    }
  } else if (observation.observed_at.value() <= attempt.acknowledged_at.value()) {
    detail = "the observation is not newer than the acknowledgement";
    return VerificationVerdict::Indeterminate;
  }

  const FreshnessVerdict fresh = evaluate_freshness(observation.observed_at, now, freshness);
  if (fresh != FreshnessVerdict::Fresh) {
    detail = std::string("the observation is not fresh: ") + to_string(fresh) +
             " (observed at " + std::to_string(observation.observed_at.value()) +
             ", requested instant " + std::to_string(now.value()) + ")";
    return VerificationVerdict::Indeterminate;
  }

  if (observation.contradictory) {
    detail = "the observation contradicts itself: " + observation.contradiction_detail;
    return VerificationVerdict::Contradicted;
  }

  if (attempt.parameters.verification_dwell.value() > 0) {
    const Result<std::int64_t> age =
        checked_sub(observation.observed_at.value(), attempt.acknowledged_at.value());
    if (!age.ok()) {
      detail = "the dwell cannot be computed from the recorded instants";
      return VerificationVerdict::Indeterminate;
    }
    if (age.value() < attempt.parameters.verification_dwell.value()) {
      detail = "the effect has been held for " + std::to_string(age.value()) +
               " ticks but the command requires " +
               std::to_string(attempt.parameters.verification_dwell.value());
      return VerificationVerdict::Unverified;
    }
  }

  if (command_family(attempt.command) == CommandFamily::Transition) {
    if (observation.operating == OperatingState::Unknown) {
      detail = "the observation does not establish an operating state";
      return VerificationVerdict::Indeterminate;
    }
    const std::vector<OperatingState> accepted = accepted_effect_states(attempt.command);
    for (const OperatingState candidate : accepted) {
      if (candidate == observation.operating) {
        detail = std::string("the observed operating state ") + to_string(observation.operating) +
                 " is an accepted effect of " + to_string(attempt.command);
        return VerificationVerdict::Verified;
      }
    }
    detail = std::string("the observed operating state ") + to_string(observation.operating) +
             " is not an accepted effect of " + to_string(attempt.command);
    return VerificationVerdict::Contradicted;
  }

  const Bool3 observed = capability_observation(attempt.command, observation);
  if (observed == Bool3::Unknown) {
    detail = "the observation does not report the capability this command affects";
    return VerificationVerdict::Indeterminate;
  }
  if (capability_effect_matches(attempt.command, observed)) {
    detail = std::string("the observed capability ") + to_string(observed) +
             " matches the effect of " + to_string(attempt.command);
    return VerificationVerdict::Verified;
  }
  detail = std::string("the observed capability ") + to_string(observed) +
           " does not match the effect of " + to_string(attempt.command);
  return VerificationVerdict::Contradicted;
}

}  // namespace ups_control
