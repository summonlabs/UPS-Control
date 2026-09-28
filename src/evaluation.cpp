#include "ups_control/evaluation.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "detail/evaluate.hpp"
#include "ups_control/refusal.hpp"
#include "ups_control/ups.hpp"

namespace ups_control::detail {
namespace {

void add(std::vector<EvaluationFinding>& findings, RefusalCode code, std::string detail) {
  findings.push_back(EvaluationFinding{code, std::move(detail)});
}

std::string join_refs(const std::vector<ObligationRef>& refs, std::size_t maximum) {
  std::string text;
  const std::size_t count = std::min(refs.size(), maximum);
  for (std::size_t index = 0; index < count; ++index) {
    if (index > 0) {
      text += ", ";
    }
    text += refs[index].value();
  }
  if (refs.size() > count) {
    text += ", and " + std::to_string(refs.size() - count) + " more";
  }
  return text;
}

bool contains(const std::vector<ObligationRef>& refs, const ObligationRef& ref) {
  return std::find(refs.begin(), refs.end(), ref) != refs.end();
}

RefusalCode reserve_refusal(const ReserveAssessment& assessment) {
  switch (assessment.outcome) {
    case ReserveOutcome::Insufficient:
      return RefusalCode::ReserveInsufficient;
    case ReserveOutcome::Sufficient:
    case ReserveOutcome::NotRequired:
      return RefusalCode::None;
    case ReserveOutcome::Indeterminate:
      break;
  }
  switch (assessment.reason) {
    case ReserveIndeterminacy::EvidenceMissing:
    case ReserveIndeterminacy::ClockUnavailable:
      return RefusalCode::ReserveEvidenceMissing;
    case ReserveIndeterminacy::EvidenceStale:
      return RefusalCode::ReserveEvidenceStale;
    case ReserveIndeterminacy::EvidenceUnknown:
    case ReserveIndeterminacy::EvidenceFuture:
    case ReserveIndeterminacy::UnitMismatch:
    case ReserveIndeterminacy::QualityBelowMinimum:
    case ReserveIndeterminacy::Contradictory:
    case ReserveIndeterminacy::NotRevalidated:
    case ReserveIndeterminacy::NoFloorDeclared:
    case ReserveIndeterminacy::None:
      return RefusalCode::ReserveIndeterminate;
  }
  return RefusalCode::ReserveIndeterminate;
}

/// The transfer preconditions each command depends on, resolved into one refusal.
void add_transfer_findings(const ControlCommand& command, const TransferStatus& transfer,
                           std::vector<EvaluationFinding>& findings) {
  const auto require = [&findings](Bool3 value, RefusalCode code, const char* what) {
    if (value != Bool3::True) {
      add(findings, code,
          std::string(what) + " is " + to_string(value) +
              "; a capability that is not explicitly established never satisfies a precondition");
    }
  };
  switch (command.kind) {
    case CommandKind::EnterStaticBypass:
      require(transfer.bypass_available, RefusalCode::BypassNotAvailable, "bypass availability");
      require(transfer.bypass_qualified, RefusalCode::BypassNotQualified, "bypass qualification");
      require(transfer.output_synchronized, RefusalCode::OutputNotSynchronized, "synchronization");
      require(transfer.transfer_ready, RefusalCode::TransferNotReady, "transfer readiness");
      require(transfer.battery_ready, RefusalCode::BatteryNotReady, "battery readiness");
      break;
    case CommandKind::EnterMaintenanceBypass:
      require(transfer.bypass_available, RefusalCode::BypassNotAvailable, "bypass availability");
      require(transfer.bypass_qualified, RefusalCode::BypassNotQualified, "bypass qualification");
      break;
    case CommandKind::LeaveStaticBypass:
    case CommandKind::LeaveMaintenanceBypass:
      require(transfer.transfer_ready, RefusalCode::TransferNotReady, "transfer readiness");
      break;
    default:
      break;
  }
}

const ObservationRecord* usable_observation(const UpsRecord& record, Tick now,
                                            const ReservePolicy& policy) {
  if (!record.observation.has_value()) {
    return nullptr;
  }
  const ObservationRecord& observation = record.observation.value();
  if (observation.contradictory) {
    return nullptr;
  }
  if (evaluate_freshness(observation.observed_at, now, unit_freshness_policy(policy)) !=
      FreshnessVerdict::Fresh) {
    return nullptr;
  }
  return &observation;
}

GrantVerdict scope_verdict(const UpsRecord& record, GrantScope scope, const ControlContext& context,
                           Tick now) {
  GrantAssessment best;
  best.verdict = GrantVerdict::Missing;
  for (const AuthorityGrant& grant : record.grants) {
    if (grant.scope != scope) {
      continue;
    }
    // A grant that is not revoked and matches the epoch is considered even when the
    // caller did not cite it, so that readiness can be reported without a request.
    if (grant.revoked_at.has_value()) {
      continue;
    }
    if (grant.epoch != context.epoch || grant.incarnation != context.incarnation) {
      continue;
    }
    if (grant.expires_at.has_value() && now.value() >= grant.expires_at->value()) {
      continue;
    }
    return grant.allowed ? GrantVerdict::Allowed : GrantVerdict::Denied;
  }
  return best.verdict;
}

}  // namespace

FreshnessPolicy unit_freshness_policy(const ReservePolicy& policy) {
  FreshnessPolicy freshness;
  freshness.max_age = policy.max_evidence_age;
  freshness.max_future_skew = policy.max_future_skew;
  return freshness;
}

Status validate_command_shape(const ControlCommand& command, const ResourceLimits& limits) {
  if (command.authority.epoch.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a control request must cite a positive control epoch");
  }
  if (command.authority.incarnation.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a control request must cite a positive controller incarnation");
  }
  Status status = validate_identifier(command.ref.ups.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "control request ups id: " + status.message());
  }
  if (command.ref.revision.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a control request must cite the positive state revision it was planned "
                         "against");
  }
  if (!is_valid_instant(command.now)) {
    return Status::error(StatusCode::InvalidArgument,
                         "a control request must carry a positive requested instant");
  }
  status = canonical_target(command.kind).status();
  if (!status.ok()) {
    return status;
  }
  const Result<std::optional<OperatingState>> target = canonical_target(command.kind);
  if (command.parameters.asserted_target.has_value() && target.value().has_value() &&
      command.parameters.asserted_target.value() != target.value().value()) {
    return Status::error(StatusCode::Conflict,
                         "the request asserts the target " +
                             std::string(to_string(command.parameters.asserted_target.value())) +
                             " but " + std::string(to_string(command.kind)) +
                             " canonically produces " +
                             std::string(to_string(target.value().value())) +
                             "; an operation is never relabelled by its caller");
  }
  if (command.parameters.asserted_target.has_value() && !target.value().has_value()) {
    return Status::error(StatusCode::Conflict,
                         "a capability command has no operating-state target, so none may be "
                         "asserted");
  }
  status = validate_command_parameters(command.parameters, limits);
  if (!status.ok()) {
    return status;
  }
  if (command.authority_ref.has_value()) {
    status = validate_identifier(command.authority_ref->value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "cited authority: " + status.message());
    }
  }
  return Status::success();
}

Result<EvaluationReport> evaluate_transition(const EvaluationInputs& inputs) {
  if (inputs.state == nullptr || inputs.command == nullptr || inputs.limits == nullptr) {
    return Status::error(StatusCode::InvalidArgument,
                         "transition evaluation needs a state, a command, and resource limits");
  }
  const ControlCommand& command = *inputs.command;
  const ResourceLimits& limits = *inputs.limits;

  const Status shape = validate_command_shape(command, limits);
  if (!shape.ok()) {
    return shape;
  }

  EvaluationReport report;
  report.ups = command.ref.ups;
  report.command = command.kind;
  report.family = command_family(command.kind);
  report.planned_revision = command.ref.revision;
  report.revalidated = inputs.revalidated;
  // A command that does not remove UPS protection leaves every binding obligation
  // protected. The full impact report is computed only for the two commands that
  // do remove it.
  report.protection.impact = ProtectionImpact::Preserves;

  std::vector<EvaluationFinding> findings;
  std::vector<EvaluationFinding> warnings;

  const UpsRecord* record = inputs.record;
  if (record == nullptr) {
    add(findings, RefusalCode::UnknownUps,
        "no unit '" + command.ref.ups.value() + "' exists in the authoritative state");
    report.findings = normalize_findings(std::move(findings), limits);
    report.warnings = normalize_findings(std::move(warnings), limits);
    report.primary = primary_refusal(report.findings);
    if (report.primary != RefusalCode::None) {
      for (const EvaluationFinding& finding : report.findings) {
        if (finding.code == report.primary) {
          report.primary_detail = finding.detail;
          break;
        }
      }
    }
    return report;
  }

  report.current_revision = record->revision;
  report.current_hardware = record->hardware;
  report.current = record->operating;
  report.basis = reported_basis(*record, inputs.basis_confirmed);
  report.transfer = record->observation.has_value() ? record->observation->transfer : TransferStatus{};

  const bool known_kind = canonical_target(command.kind).ok();
  if (known_kind) {
    const Result<std::optional<OperatingState>> target = canonical_target(command.kind);
    if (target.value().has_value()) {
      report.target = target.value().value();
    }
  }

  if (const Status valid = validate_ups_record(*record, limits); !valid.ok()) {
    add(findings, RefusalCode::InternalInvariant,
        "the authoritative record fails its own structural validation: " + valid.message());
  }

  if (!(command.ref.hardware == record->hardware)) {
    add(findings, RefusalCode::StaleHardwareGeneration,
        "the request was planned against hardware generation " +
            std::to_string(command.ref.hardware.value()) + " but unit '" + record->id.value() +
            "' is at hardware generation " + std::to_string(record->hardware.value()) +
            "; the request is fenced");
  }
  if (!(command.authority.epoch == inputs.state->epoch)) {
    add(findings, RefusalCode::StaleEpoch,
        "the request cites control epoch " + std::to_string(command.authority.epoch.value()) +
            " but the store is at epoch " + std::to_string(inputs.state->epoch.value()));
  }
  if (!(command.authority.incarnation == inputs.state->incarnation)) {
    add(findings, RefusalCode::StaleIncarnation,
        "the request cites controller incarnation " +
            std::to_string(command.authority.incarnation.value()) + " but the store is at incarnation " +
            std::to_string(inputs.state->incarnation.value()));
  }
  if (!(command.ref.revision == record->revision)) {
    add(findings, RefusalCode::StaleRevision,
        "the request was planned against state revision " +
            std::to_string(command.ref.revision.value()) + " but unit '" + record->id.value() +
            "' is at revision " + std::to_string(record->revision.value()));
  }
  if (!known_kind) {
    add(findings, RefusalCode::UnsupportedCommand,
        "command value " + std::to_string(static_cast<int>(command.kind)) +
            " is not a documented operation and is never mapped onto a neighbouring one");
  }
  if (!inputs.adapter_bound) {
    add(findings, RefusalCode::AdapterUnavailable,
        "no adapter is bound, so no command can be issued for unit '" + record->id.value() + "'");
  } else if (known_kind && (inputs.adapter_supported_commands & command_kind_bit(command.kind)) == 0) {
    add(findings, RefusalCode::CapabilityUnsupported,
        "adapter '" + inputs.adapter.value() + "' does not implement " +
            std::string(to_string(command.kind)) +
            "; the operation is reported unsupported rather than substituted");
  }
  if (!inputs.revalidated) {
    add(findings, RefusalCode::StateNotRevalidated,
        "the store has been recovered and not revalidated at instant " +
            std::to_string(command.now.value()) + ", so no control decision may be taken");
  }
  if (record->operating == OperatingState::Unknown) {
    add(findings, RefusalCode::OperatingStateUnknown,
        "no validated observation establishes the operating state of unit '" + record->id.value() +
            "', and a transition out of an unknown state is never planned");
  }
  if (known_kind && report.family == CommandFamily::Transition && report.target != OperatingState::Unknown &&
      record->operating != OperatingState::Unknown &&
      !is_legal_operating_transition(record->operating, report.target)) {
    add(findings, RefusalCode::IllegalOperatingTransition,
        std::string("the transition from ") + to_string(record->operating) + " to " +
            to_string(report.target) + " is not an edge of the documented transition graph");
  }
  if (!lifecycle_permits_control(record->lifecycle)) {
    add(findings, RefusalCode::LifecycleForbidsControl,
        std::string("lifecycle ") + to_string(record->lifecycle) +
            " does not permit load-affecting control");
  }
  if (record->basis == StateBasis::CommandedUnverified) {
    add(findings, RefusalCode::StateBasisUnverified,
        "the operating state of unit '" + record->id.value() +
            "' rests on an acknowledged command whose effect was never verified; record telemetry "
            "or abandon the attempt before commanding again");
  }

  // --- protected-load obligations ---
  const ObservationRecord* effective_observation = usable_observation(*record, command.now, record->reserve_policy);
  if (record->observation.has_value() && record->observation->contradictory) {
    add(findings, RefusalCode::ContradictoryObservation,
        "the recorded observation contradicts itself: " +
            record->observation->contradiction_detail);
  } else if (record->observation.has_value() &&
             effective_observation == nullptr &&
             drops_protection(command.kind)) {
    add(findings, RefusalCode::ContradictoryObservation,
        "the recorded observation is not fresh at the requested instant, so the protected-load "
        "impact of a protection-dropping transition cannot be established");
  }

  if (known_kind && drops_protection(command.kind)) {
    report.protection = assess_protected_impact(record->obligations, command.authority_ref,
                                                command.now, limits);
    if (record->obligations.empty()) {
      report.protection.impact = ProtectionImpact::Preserves;
    }
    if (report.protection.impact == ProtectionImpact::DropsUndeclared) {
      std::vector<ObligationRef> lapsed_unreleased;
      std::vector<ObligationRef> active_unreleased;
      for (const ObligationRef& ref : report.protection.unreleased) {
        if (contains(report.protection.lapsed, ref)) {
          lapsed_unreleased.push_back(ref);
        } else {
          active_unreleased.push_back(ref);
        }
      }
      if (!active_unreleased.empty()) {
        add(findings, RefusalCode::ObligationUnreleased,
            std::string(to_string(command.kind)) +
                " removes UPS protection from protected loads whose obligations are still binding: " +
                join_refs(active_unreleased, 8));
      }
      if (!lapsed_unreleased.empty()) {
        add(findings, RefusalCode::ObligationExpired,
            std::string(to_string(command.kind)) +
                " would drop protection asserted by obligations whose window has lapsed and which "
                "were never released or re-asserted: " + join_refs(lapsed_unreleased, 8));
      }
    }
  }

  // --- transfer preconditions ---
  TransferStatus effective_transfer;
  if (effective_observation != nullptr) {
    effective_transfer = effective_observation->transfer;
    report.transfer = effective_transfer;
  } else {
    effective_transfer = TransferStatus{};
    report.transfer = effective_transfer;
  }
  if (known_kind && report.family == CommandFamily::Transition) {
    add_transfer_findings(command, effective_transfer, findings);
  }

  // --- reserve ---
  GrantScope scope = GrantScope::BypassTransfer;
  if (known_kind) {
    const Result<GrantScope> resolved = required_scope(command.kind);
    if (resolved.ok()) {
      scope = resolved.value();
    }
  }
  const GrantAssessment cited_grant =
      assess_grant(record->grants, command.authority_ref, scope, command.authority, command.now);
  report.grant = cited_grant;

  const bool reserve_relevant = known_kind && (requires_reserve(command.kind) ||
                                               command.parameters.reserve_requirement.has_value());
  if (reserve_relevant) {
    std::optional<ReserveQuantity> authority_floor;
    if (cited_grant.verdict == GrantVerdict::Allowed && cited_grant.grant.has_value()) {
      const AuthorityGrant* grant = find_grant(*record, cited_grant.grant.value());
      if (grant != nullptr && grant->reserve_floor.has_value()) {
        authority_floor = grant->reserve_floor;
      }
    }
    const ReserveAssessment assessment =
        evaluate_reserve(command.parameters.reserve_requirement, record->reserve_policy,
                         record->observation.has_value() ? &record->observation.value() : nullptr,
                         authority_floor, command.now, inputs.revalidated);
    report.reserve = assessment;
    if (assessment.outcome == ReserveOutcome::NotRequired) {
      add(findings, RefusalCode::ReserveRequirementMissing,
          std::string(to_string(command.kind)) +
              " depends on battery reserve but neither the request, an authority grant, nor policy "
              "declares a floor, so the precondition cannot be established");
    } else if (const RefusalCode code = reserve_refusal(assessment); code != RefusalCode::None) {
      add(findings, code, assessment.detail);
    }
  } else if (record->reserve_policy.discharge_floor.has_value()) {
    report.reserve = evaluate_reserve(std::nullopt, record->reserve_policy,
                                      record->observation.has_value()
                                          ? &record->observation.value()
                                          : nullptr,
                                      std::nullopt, command.now, inputs.revalidated);
  }

  // --- authority ---
  if (known_kind) {
    switch (cited_grant.verdict) {
      case GrantVerdict::Allowed:
        break;
      case GrantVerdict::Missing:
      case GrantVerdict::OutOfScope:
        add(findings, RefusalCode::AuthorityMissing, cited_grant.detail);
        break;
      case GrantVerdict::Denied:
        add(findings, RefusalCode::AuthorityDenied, cited_grant.detail);
        break;
      case GrantVerdict::Expired:
        add(findings, RefusalCode::AuthorityExpired, cited_grant.detail);
        break;
      case GrantVerdict::Revoked:
        add(findings, RefusalCode::AuthorityRevoked, cited_grant.detail);
        break;
      case GrantVerdict::Fenced:
        add(findings, RefusalCode::AuthorityFenced, cited_grant.detail);
        break;
    }
  }

  // --- single-device ordering ---
  if (record->in_flight.has_value()) {
    add(findings, RefusalCode::TransitionInProgress,
        "unit '" + record->id.value() + "' already has unresolved attempt " +
            std::to_string(record->in_flight->value()) +
            "; a second command is refused so that device transitions stay ordered");
  }

  // --- warnings ---
  if (!inputs.basis_confirmed) {
    warnings.push_back(EvaluationFinding{
        RefusalCode::StateNotRevalidated,
        "the operating state of unit '" + record->id.value() +
            "' was recovered from the store and is not confirmed by post-recovery telemetry in "
            "this session"});
  }
  if (record->observation.has_value()) {
    const FreshnessVerdict freshness = evaluate_freshness(
        record->observation->observed_at, command.now, unit_freshness_policy(record->reserve_policy));
    if (freshness != FreshnessVerdict::Fresh) {
      warnings.push_back(EvaluationFinding{
          RefusalCode::ReserveEvidenceStale,
          std::string("the last observation of unit '") + record->id.value() + "' is " +
              to_string(freshness) + " (observed at " +
              std::to_string(record->observation->observed_at.value()) + ")"});
    }
    if (is_synthetic(record->observation->provenance)) {
      warnings.push_back(EvaluationFinding{
          RefusalCode::ContradictoryObservation,
          "the recorded observation of unit '" + record->id.value() +
              "' carries SYNTHETIC provenance and is not hardware evidence"});
    }
  }
  if (cited_grant.verdict == GrantVerdict::Allowed && cited_grant.grant.has_value()) {
    const AuthorityGrant* grant = find_grant(*record, cited_grant.grant.value());
    if (grant != nullptr && !grant->expires_at.has_value()) {
      warnings.push_back(EvaluationFinding{
          RefusalCode::AuthorityExpired,
          "grant '" + grant->ref.value() + "' declares no expiry, so it stays in force until it is "
          "revoked"});
    }
  }

  report.findings = normalize_findings(std::move(findings), limits);
  report.warnings = normalize_findings(std::move(warnings), limits);
  report.primary = primary_refusal(report.findings);
  report.allowed = report.primary == RefusalCode::None;
  if (report.allowed) {
    report.primary_detail = "every precondition is established";
  } else {
    for (const EvaluationFinding& finding : report.findings) {
      if (finding.code == report.primary) {
        report.primary_detail = finding.detail;
        break;
      }
    }
  }
  return report;
}

StateBasis reported_basis(const UpsRecord& record, bool basis_confirmed) noexcept {
  return basis_confirmed ? record.basis : StateBasis::Recovered;
}

ReadinessReport compute_readiness(const UpsState& state, const UpsRecord& record,
                                  const ResourceLimits& limits, Tick now, bool revalidated,
                                  bool adapter_bound, std::uint32_t adapter_supported_commands,
                                  bool basis_confirmed) {
  (void)adapter_bound;
  (void)adapter_supported_commands;
  ReadinessReport readiness;
  readiness.ups = record.id;
  readiness.revision = record.revision;
  readiness.hardware = record.hardware;
  readiness.lifecycle = record.lifecycle;
  readiness.operating = record.operating;
  readiness.basis = reported_basis(record, basis_confirmed);
  readiness.revalidated = revalidated;
  readiness.in_flight = record.in_flight;
  readiness.command_in_flight = record.in_flight.has_value();

  const ControlContext context{state.epoch, state.incarnation};

  const ObservationRecord* observation = usable_observation(record, now, record.reserve_policy);
  TransferStatus transfer;
  if (observation != nullptr) {
    transfer = observation->transfer;
  }

  const ReserveAssessment reserve = evaluate_reserve(std::nullopt, record.reserve_policy,
                                                     record.observation.has_value()
                                                         ? &record.observation.value()
                                                         : nullptr,
                                                     std::nullopt, now, revalidated);
  readiness.reserve_freshness = reserve.freshness;
  readiness.reserve_outcome = reserve.outcome;
  readiness.reserve = reserve.observed;
  readiness.reserve_observed_at = reserve.observed_at;

  for (const ProtectedLoadObligation& obligation : record.obligations) {
    if (!obligation_is_protective(obligation)) {
      continue;
    }
    ++readiness.protective_obligation_count;
    if (obligation_lapsed(obligation, now)) {
      ++readiness.lapsed_obligation_count;
      ++readiness.unreleased_protective_count;
      readiness.blockers.push_back(EvaluationFinding{
          RefusalCode::ObligationExpired,
          "obligation '" + obligation.ref.value() +
              "' has lapsed and has not been released or re-asserted"});
    } else if (obligation.state == ObligationState::Active) {
      ++readiness.unreleased_protective_count;
    }
  }
  readiness.obligation_count = record.obligations.size();

  const bool operable = lifecycle_permits_control(record.lifecycle) && revalidated &&
                        !record.in_flight.has_value() &&
                        record.basis != StateBasis::CommandedUnverified &&
                        record.operating != OperatingState::Unknown;

  readiness.transfer_ready = certainly_true(transfer.transfer_ready);
  readiness.static_bypass_ready = operable && static_bypass_ready(transfer);
  readiness.maintenance_bypass_ready = operable && maintenance_bypass_ready(transfer);
  readiness.test_ready = operable && !is_testing(record.operating) && observation != nullptr;

  readiness.recharge_authorized =
      scope_verdict(record, GrantScope::RechargeEnable, context, now) == GrantVerdict::Allowed;
  readiness.discharge_authorized =
      scope_verdict(record, GrantScope::DischargeEnable, context, now) == GrantVerdict::Allowed;

  if (!revalidated) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::StateNotRevalidated,
        "the store has not been revalidated at this instant"});
  }
  if (!lifecycle_permits_control(record.lifecycle)) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::LifecycleForbidsControl,
        std::string("lifecycle ") + to_string(record.lifecycle) + " does not permit control"});
  }
  if (record.in_flight.has_value()) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::TransitionInProgress,
        "attempt " + std::to_string(record.in_flight->value()) + " is unresolved"});
  }
  if (record.basis == StateBasis::CommandedUnverified) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::StateBasisUnverified,
        "a prior command was acknowledged but never verified"});
  }
  if (!static_bypass_ready(transfer)) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::TransferNotReady,
        "the device does not establish every static bypass precondition"});
  }
  if (!maintenance_bypass_ready(transfer)) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::BypassNotQualified,
        "the device does not establish every maintenance bypass precondition"});
  }
  if (observation == nullptr) {
    readiness.blockers.push_back(EvaluationFinding{
        RefusalCode::ReserveEvidenceMissing,
        "no fresh, non-contradictory observation is available"});
  }
  readiness.blockers = normalize_findings(std::move(readiness.blockers), limits);
  return readiness;
}

}  // namespace ups_control::detail

namespace ups_control {

std::string describe_evaluation(const EvaluationReport& report) {
  std::string text;
  text += "unit=" + report.ups.value() + "\n";
  text += std::string("command=") + to_string(report.command) + "\n";
  text += "planned_revision=" + std::to_string(report.planned_revision.value()) + "\n";
  text += "current_revision=" + std::to_string(report.current_revision.value()) + "\n";
  text += std::string("current_state=") + to_string(report.current) + "\n";
  text += std::string("target_state=") + to_string(report.target) + "\n";
  text += std::string("verdict=") + (report.allowed ? "allowed" : "refused") + "\n";
  text += std::string("primary=") + to_string(report.primary) + "\n";
  text += "primary_detail=" + report.primary_detail + "\n";
  text += "protection=" + std::string(to_string(report.protection.impact)) + "\n";
  text += std::string("authority=") + to_string(report.grant.verdict) + "\n";
  if (report.reserve.has_value()) {
    text += std::string("reserve=") + to_string(report.reserve->outcome) + "\n";
    text += "reserve_detail=" + report.reserve->detail + "\n";
  }
  for (const EvaluationFinding& finding : report.findings) {
    text += std::string("finding=") + to_string(finding.code) + " " + finding.detail + "\n";
  }
  for (const EvaluationFinding& warning : report.warnings) {
    text += std::string("warning=") + to_string(warning.code) + " " + warning.detail + "\n";
  }
  return text;
}

std::string describe_readiness(const ReadinessReport& report) {
  std::string text;
  text += "unit=" + report.ups.value() + "\n";
  text += std::string("lifecycle=") + to_string(report.lifecycle) + "\n";
  text += std::string("operating=") + to_string(report.operating) + "\n";
  text += std::string("basis=") + to_string(report.basis) + "\n";
  text += std::string("revalidated=") + (report.revalidated ? "true" : "false") + "\n";
  text += std::string("reserve=") + to_string(report.reserve_outcome) + "\n";
  text += std::string("reserve_freshness=") + to_string(report.reserve_freshness) + "\n";
  text += std::string("static_bypass_ready=") + (report.static_bypass_ready ? "true" : "false") + "\n";
  text += std::string("maintenance_bypass_ready=") +
          (report.maintenance_bypass_ready ? "true" : "false") + "\n";
  text += std::string("transfer_ready=") + (report.transfer_ready ? "true" : "false") + "\n";
  text += std::string("test_ready=") + (report.test_ready ? "true" : "false") + "\n";
  text += std::string("recharge_authorized=") + (report.recharge_authorized ? "true" : "false") + "\n";
  text += std::string("discharge_authorized=") + (report.discharge_authorized ? "true" : "false") + "\n";
  text += "obligations=" + std::to_string(report.obligation_count) + "\n";
  text += "protective_obligations=" + std::to_string(report.protective_obligation_count) + "\n";
  for (const EvaluationFinding& blocker : report.blockers) {
    text += std::string("blocker=") + to_string(blocker.code) + " " + blocker.detail + "\n";
  }
  return text;
}

}  // namespace ups_control
