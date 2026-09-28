#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ups_control/authority.hpp"
#include "ups_control/battery.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/refusal.hpp"
#include "ups_control/telemetry.hpp"

namespace ups_control {

/// The vendor-neutral control vocabulary.
///
/// Every member names one operation that this runtime models and can represent
/// safely. A value outside this enumeration is not a command: it is refused with
/// \c StatusCode::Unsupported at the boundary and never mapped onto a nearby
/// member.
enum class CommandKind : std::uint8_t {
  EnterStaticBypass = 1,
  LeaveStaticBypass = 2,
  EnterMaintenanceBypass = 3,
  LeaveMaintenanceBypass = 4,
  StartSelfTest = 5,
  StartBatteryTest = 6,
  AbortTest = 7,
  EnableRecharge = 8,
  DisableRecharge = 9,
  EnableDischarge = 10,
  DisableDischarge = 11,
  IsolateOutput = 12,
  ReturnFromIsolation = 13,
  ClearFaults = 14,
};

const char* to_string(CommandKind kind) noexcept;
Result<CommandKind> parse_command_kind(std::string_view text);

/// Whether a command moves the operating state or a capability.
enum class CommandFamily : std::uint8_t {
  /// The observed effect is an operating state.
  Transition = 1,
  /// The observed effect is a device capability.
  Capability = 2,
};

CommandFamily command_family(CommandKind kind) noexcept;

/// The single canonical operating-state target of a transition command.
///
/// Returns \c nullopt for a capability command, which has no operating-state
/// target, and \c StatusCode::Unsupported for a value that is not a documented
/// command kind. A caller-supplied target that differs from this is refused
/// rather than honoured, so a caller can never relabel an operation.
Result<std::optional<OperatingState>> canonical_target(CommandKind kind);

/// The operating states that count as a verified effect of this command, in
/// canonical order with the canonical target first. Empty for a capability
/// command.
std::vector<OperatingState> accepted_effect_states(CommandKind kind);

/// The grant scope this command requires.
Result<GrantScope> required_scope(CommandKind kind);

/// True when the command's safety depends on an established battery reserve.
bool requires_reserve(CommandKind kind) noexcept;

/// True when a successful effect means the protected load is no longer protected
/// by this UPS.
bool drops_protection(CommandKind kind) noexcept;

/// A bounded, human-readable name for the canonical target, for diagnostics.
std::string describe_command(CommandKind kind);

/// Parameters accompanying a command.
struct CommandParameters {
  /// Optional caller assertion of the resulting operating state. When present it
  /// must equal the canonical target, so a caller can never relabel an
  /// operation.
  std::optional<OperatingState> asserted_target;
  /// Optional per-request reserve floor for a reserve-dependent command.
  std::optional<ReserveRequirement> reserve_requirement;
  /// Logical duration the effect must have held before it may be verified.
  TickSpan verification_dwell{0};

  friend bool operator==(const CommandParameters&, const CommandParameters&) = default;
};

Status validate_command_parameters(const CommandParameters& parameters,
                                   const ResourceLimits& limits);

/// One control request, carrying the authority and the exact device generation
/// and state revision it was planned against.
struct ControlCommand {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  IdempotencyKey key;
  CommandKind kind = CommandKind::EnterStaticBypass;
  CommandParameters parameters;
  /// The grant cited for this operation. Absent means no authority is cited,
  /// which is refused for every command that requires one.
  std::optional<AuthorityRef> authority_ref;
};

// ---------------------------------------------------------------------------
// Attempt stages
// ---------------------------------------------------------------------------

/// The stages a control attempt passes through. Each stage is durable before the
/// next begins.
enum class AttemptPhase : std::uint8_t {
  /// Recorded durably, not yet handed to an adapter.
  Planned = 1,
  /// Evaluation refused the request. Terminal.
  Refused = 2,
  /// Handed to the adapter.
  Issued = 3,
  /// The adapter acknowledged the request. An acknowledgement is not an effect.
  Acknowledged = 4,
  /// Telemetry newer than the acknowledgement has been recorded.
  Observed = 5,
  /// The observed effect matched the command. Terminal.
  Verified = 6,
  /// The attempt ended without a verified effect. Terminal.
  Failed = 7,
  /// The adapter does not implement the command. Terminal.
  Unsupported = 8,
};

const char* to_string(AttemptPhase phase) noexcept;
bool is_terminal(AttemptPhase phase) noexcept;
bool is_unresolved(AttemptPhase phase) noexcept;

/// What the adapter reported about the issued command.
enum class AckOutcome : std::uint8_t {
  None = 0,
  Accepted = 1,
  Rejected = 2,
  Unsupported = 3,
  NoResponse = 4,
  Malformed = 5,
};

const char* to_string(AckOutcome outcome) noexcept;

/// What the post-command observation showed.
enum class ObservedOutcome : std::uint8_t {
  None = 0,
  /// The observed state is one of the accepted effect states.
  MatchesTarget = 1,
  /// The observed state is a concrete but different state.
  DifferentState = 2,
  /// The observation contradicts itself.
  Contradictory = 3,
  /// The observation does not establish an operating state.
  Unavailable = 4,
};

const char* to_string(ObservedOutcome outcome) noexcept;

/// The verification verdict. Distinct from acknowledgement and from observation.
enum class VerificationVerdict : std::uint8_t {
  NotEvaluated = 0,
  Verified = 1,
  /// The observation does not yet show the effect.
  Unverified = 2,
  /// The observation shows a concrete state that the command cannot produce.
  Contradicted = 3,
  /// The observation cannot decide the effect.
  Indeterminate = 4,
};

const char* to_string(VerificationVerdict verdict) noexcept;

/// The evidence class of an attempt. This runtime never claims hardware proof: an
/// attempt carried out through the deterministic adapter is labelled
/// \c Synthetic everywhere it is reported.
enum class EvidenceClass : std::uint8_t {
  /// No adapter produced the attempt.
  Unsupported = 0,
  Synthetic = 1,
  Real = 2,
};

const char* to_string(EvidenceClass evidence) noexcept;

/// The durable record of one control attempt.
struct AttemptRecord {
  AttemptId id;
  IdempotencyKey key;
  UpsId ups;
  HardwareGeneration hardware;
  ControlEpoch epoch;
  Incarnation incarnation;
  StateRevision planned_revision;
  CommandKind command = CommandKind::EnterStaticBypass;
  CommandParameters parameters;
  /// Canonical target state, or \c Unknown for a capability command.
  OperatingState target = OperatingState::Unknown;
  Tick submitted_at;
  AttemptPhase phase = AttemptPhase::Planned;
  /// Deterministic fingerprint of the planned request. Two requests share a
  /// fingerprint only when every planned field is equal.
  std::uint64_t plan_digest = 0;
  std::optional<RefusalRecord> refusal;
  AckOutcome ack = AckOutcome::None;
  std::string ack_detail;
  Tick acknowledged_at;
  ObservedOutcome observed = ObservedOutcome::None;
  Tick observed_at;
  OperatingState observed_state = OperatingState::Unknown;
  CapabilityObservation observed_capability;
  VerificationVerdict verification = VerificationVerdict::NotEvaluated;
  std::string verification_detail;
  Tick verified_at;
  /// The identity of the observation that established a verified effect.
  std::optional<EvidenceId> verification_evidence;
  std::uint32_t verification_rounds = 0;
  EvidenceClass evidence = EvidenceClass::Unsupported;
  AdapterId adapter;
  std::string terminal_detail;

  friend bool operator==(const AttemptRecord&, const AttemptRecord&) = default;
};

/// True when the attempt's observed capability matches what the command enables.
bool capability_effect_matches(CommandKind kind, Bool3 observed) noexcept;

/// Evaluates the verification verdict for one attempt against a recorded
/// observation. Pure: it performs no I/O and mutates nothing.
VerificationVerdict verify_attempt(const AttemptRecord& attempt,
                                   const ObservationRecord& observation, Tick now,
                                   const FreshnessPolicy& freshness, std::string& detail);

/// The freshness window applied to the telemetry that verifies an effect.
FreshnessPolicy default_verification_freshness();

}  // namespace ups_control
