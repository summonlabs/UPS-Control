#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ups_control/authority.hpp"
#include "ups_control/battery.hpp"
#include "ups_control/command.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/obligation.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/refusal.hpp"
#include "ups_control/telemetry.hpp"

namespace ups_control {

/// The complete, explainable result of evaluating one control request.
///
/// Evaluation is pure: it reads the authoritative state and produces a report. It
/// performs no I/O, mutates nothing, and never issues anything to an adapter.
struct EvaluationReport {
  UpsId ups;
  CommandKind command = CommandKind::EnterStaticBypass;
  CommandFamily family = CommandFamily::Transition;
  StateRevision planned_revision;
  StateRevision current_revision;
  HardwareGeneration current_hardware;
  OperatingState current = OperatingState::Unknown;
  OperatingState target = OperatingState::Unknown;
  StateBasis basis = StateBasis::Recovered;

  bool allowed = false;
  RefusalCode primary = RefusalCode::None;
  std::string primary_detail;
  /// Blocking findings, ordered by precedence.
  std::vector<EvaluationFinding> findings;
  /// Non-blocking observations, ordered by precedence.
  std::vector<EvaluationFinding> warnings;

  std::optional<ReserveAssessment> reserve;
  ProtectedImpactReport protection;
  GrantAssessment grant;
  TransferStatus transfer;
  bool revalidated = false;

  friend bool operator==(const EvaluationReport&, const EvaluationReport&) = default;
};

/// Renders a report as a stable, line-oriented audit text. Deterministic for
/// equal reports.
std::string describe_evaluation(const EvaluationReport& report);

/// A snapshot of what one UPS can currently do, and why not.
struct ReadinessReport {
  UpsId ups;
  StateRevision revision;
  HardwareGeneration hardware;
  LifecycleState lifecycle = LifecycleState::Commissioning;
  OperatingState operating = OperatingState::Unknown;
  StateBasis basis = StateBasis::Recovered;
  bool revalidated = false;

  FreshnessVerdict reserve_freshness = FreshnessVerdict::NeverObserved;
  ReserveOutcome reserve_outcome = ReserveOutcome::Indeterminate;
  std::optional<ReserveQuantity> reserve;
  Tick reserve_observed_at;

  bool static_bypass_ready = false;
  bool maintenance_bypass_ready = false;
  bool transfer_ready = false;
  bool test_ready = false;
  bool recharge_authorized = false;
  bool discharge_authorized = false;

  std::size_t obligation_count = 0;
  std::size_t protective_obligation_count = 0;
  std::size_t unreleased_protective_count = 0;
  std::size_t lapsed_obligation_count = 0;

  bool command_in_flight = false;
  std::optional<AttemptId> in_flight;

  /// Non-blocking reasons that a capability is not ready, in precedence order.
  std::vector<EvaluationFinding> blockers;
};

/// Renders the readiness of one UPS as a stable, line-oriented audit text.
std::string describe_readiness(const ReadinessReport& report);

/// One evaluated request, with the request retained for context.
///
/// The report is what the evaluation decided; the command is what was asked.
/// Evaluation is pure and issues nothing: a caller that wants an attempt recorded
/// must submit the same command through the engine.
struct TransitionEvaluation {
  EvaluationReport report;
  ControlCommand command;
};

}  // namespace ups_control
