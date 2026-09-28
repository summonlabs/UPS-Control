#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ups_control/limits.hpp"
#include "ups_control/status.hpp"

namespace ups_control {

/// The refusal vocabulary of transition evaluation.
///
/// The numeric value *is* the validation precedence: when a request violates
/// several rules at once, the finding with the lowest numeric value is the
/// primary refusal, so the same invalid request always produces the same primary
/// error regardless of the order in which its violations happen to be
/// discovered. Values are part of the public contract and are never reused.
enum class RefusalCode : std::uint8_t {
  /// No refusal. Present only for a successful evaluation.
  None = 0,
  /// The named UPS does not exist in the authoritative state.
  UnknownUps = 1,
  /// The request exceeds a configured resource bound.
  ResourceLimitExceeded = 2,
  /// The request was planned against a superseded hardware generation.
  StaleHardwareGeneration = 3,
  /// The request was planned against a superseded control epoch.
  StaleEpoch = 4,
  /// The request was planned against a superseded controller incarnation.
  StaleIncarnation = 5,
  /// The request was planned against a superseded state revision.
  StaleRevision = 6,
  /// The operation is not a documented command, or the adapter does not implement
  /// it. It is never mapped onto a nearby operation.
  UnsupportedCommand = 7,
  /// No adapter is bound, so nothing can be issued.
  AdapterUnavailable = 8,
  /// The adapter is bound but reports the operation as unimplemented.
  CapabilityUnsupported = 9,
  /// Recovered state has not been revalidated at the requested instant.
  StateNotRevalidated = 10,
  /// No validated observation establishes the current operating state.
  OperatingStateUnknown = 11,
  /// The requested edge is not in the documented operating transition graph.
  IllegalOperatingTransition = 12,
  /// The lifecycle state does not permit load-affecting control.
  LifecycleForbidsControl = 13,
  /// The requested edge is not in the documented lifecycle transition graph.
  LifecycleTransitionIllegal = 14,
  /// The current operating state is not trustworthy because a prior command was
  /// acknowledged but its effect was never verified.
  StateBasisUnverified = 15,
  /// A protected-load obligation that must remain protected would be dropped.
  ObligationUnreleased = 16,
  /// A protected-load obligation's assertion window has lapsed and it has not
  /// been explicitly released or re-asserted.
  ObligationExpired = 17,
  /// No bypass path is available.
  BypassNotAvailable = 18,
  /// A bypass path exists but is not qualified to carry the protected load.
  BypassNotQualified = 19,
  /// The output is not synchronized to the bypass source.
  OutputNotSynchronized = 20,
  /// The device does not assert that a transfer may proceed.
  TransferNotReady = 21,
  /// The battery is not reported ready to accept the load.
  BatteryNotReady = 22,
  /// The operation needs a reserve floor and neither policy nor the request
  /// declares one.
  ReserveRequirementMissing = 23,
  /// No reserve evidence exists.
  ReserveEvidenceMissing = 24,
  /// Reserve evidence is outside its freshness window.
  ReserveEvidenceStale = 25,
  /// Reserve evidence exists but cannot decide the precondition.
  ReserveIndeterminate = 26,
  /// Reserve evidence is known and below the floor.
  ReserveInsufficient = 27,
  /// No authority covers the operation.
  AuthorityMissing = 28,
  /// An authority explicitly denies the operation.
  AuthorityDenied = 29,
  /// The authority's validity window has lapsed.
  AuthorityExpired = 30,
  /// The authority was revoked.
  AuthorityRevoked = 31,
  /// The authority was issued under a superseded epoch or incarnation.
  AuthorityFenced = 32,
  /// Another unresolved control attempt holds the single-device ordering gate.
  TransitionInProgress = 33,
  /// The idempotency key is already bound to a different request.
  IdempotencyConflict = 34,
  /// The telemetry contradicts itself or the authoritative state.
  ContradictoryObservation = 35,
  /// An internal consistency guarantee was violated.
  InternalInvariant = 36,
};

/// Stable textual token in lower_snake_case.
const char* to_string(RefusalCode code) noexcept;

/// Maps a refusal onto the general status code vocabulary.
StatusCode status_code_of(RefusalCode code) noexcept;

/// One ordered evaluation finding.
struct EvaluationFinding {
  RefusalCode code = RefusalCode::None;
  std::string detail;

  friend bool operator==(const EvaluationFinding&, const EvaluationFinding&) = default;
};

/// A recorded refusal: the primary code plus its deterministic explanation.
struct RefusalRecord {
  RefusalCode code = RefusalCode::None;
  std::string detail;

  friend bool operator==(const RefusalRecord&, const RefusalRecord&) = default;
};

/// Sorts findings by precedence and drops duplicates, producing a deterministic
/// order for any set of findings. Bounded by \c limits.max_findings.
std::vector<EvaluationFinding> normalize_findings(std::vector<EvaluationFinding> findings,
                                                  const ResourceLimits& limits);

/// The primary refusal of a finding set: the lowest-valued code, or \c None when
/// the set is empty. Independent of the input order.
RefusalCode primary_refusal(const std::vector<EvaluationFinding>& findings) noexcept;

}  // namespace ups_control
