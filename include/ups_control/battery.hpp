#pragma once

#include <optional>
#include <string>

#include "ups_control/ids.hpp"
#include "ups_control/telemetry.hpp"
#include "ups_control/units.hpp"

namespace ups_control {

/// A declared reserve floor.
struct ReserveRequirement {
  ReserveQuantity minimum;

  friend bool operator==(const ReserveRequirement&, const ReserveRequirement&) = default;
};

/// Policy for using reserve evidence. Policy is an authority input supplied by an
/// authorized caller; it is never inferred from telemetry.
struct ReservePolicy {
  /// Minimum reserve that must be established before a reserve-dependent
  /// operation. Absent means policy declares no floor, in which case a
  /// reserve-dependent operation is refused rather than assumed safe.
  std::optional<ReserveQuantity> discharge_floor;
  /// Largest accepted age of the evidence that satisfies the floor.
  TickSpan max_evidence_age{300};
  /// Tolerated future skew of the reporting device clock.
  TickSpan max_future_skew{0};
  /// Weakest evidence quality that may satisfy the floor.
  EvidenceQuality minimum_quality = EvidenceQuality::Estimated;

  friend bool operator==(const ReservePolicy&, const ReservePolicy&) = default;
};

Status validate_reserve_policy(const ReservePolicy& policy, const ResourceLimits& limits);

/// The verdict of one reserve evaluation.
enum class ReserveOutcome : std::uint8_t {
  /// No floor was declared, so nothing was required.
  NotRequired = 1,
  /// A known, fresh, comparable quantity satisfies the floor.
  Sufficient = 2,
  /// A known, fresh, comparable quantity is below the floor.
  Insufficient = 3,
  /// The floor could not be decided from the evidence.
  Indeterminate = 4,
};

const char* to_string(ReserveOutcome outcome) noexcept;

/// Why a reserve evaluation is indeterminate. Distinct from "insufficient": an
/// unknown reserve is not a small reserve.
enum class ReserveIndeterminacy : std::uint8_t {
  None = 0,
  NoFloorDeclared = 1,
  EvidenceMissing = 2,
  EvidenceUnknown = 3,
  EvidenceStale = 4,
  EvidenceFuture = 5,
  UnitMismatch = 6,
  QualityBelowMinimum = 7,
  NotRevalidated = 8,
  Contradictory = 9,
  ClockUnavailable = 10,
};

const char* to_string(ReserveIndeterminacy reason) noexcept;

/// A complete, explainable reserve assessment.
struct ReserveAssessment {
  ReserveOutcome outcome = ReserveOutcome::Indeterminate;
  ReserveIndeterminacy reason = ReserveIndeterminacy::EvidenceMissing;
  FreshnessVerdict freshness = FreshnessVerdict::NeverObserved;
  std::optional<ReserveQuantity> observed;
  std::optional<ReserveQuantity> required;
  std::optional<EvidenceId> evidence;
  Tick observed_at;
  TickSpan age;
  /// Deterministic explanation, suitable for an audit record.
  std::string detail;

  friend bool operator==(const ReserveAssessment&, const ReserveAssessment&) = default;
};

/// Evaluates reserve evidence against a requirement.
///
/// Precedence, and therefore the resulting \c reason, is fixed:
///   1. no floor declared              -> NotRequired
///   2. store not revalidated          -> Indeterminate(NotRevalidated)
///   3. observation contradictory      -> Indeterminate(Contradictory)
///   4. no reserve observation         -> Indeterminate(EvidenceMissing)
///   5. reserve explicitly unknown     -> Indeterminate(EvidenceUnknown)
///   6. observation instant invalid    -> Indeterminate(EvidenceMissing)
///   7. observation ahead of now       -> Indeterminate(EvidenceFuture)
///   8. observation older than window  -> Indeterminate(EvidenceStale)
///   9. units not comparable           -> Indeterminate(UnitMismatch)
///  10. quality weaker than the floor  -> Indeterminate(QualityBelowMinimum)
///  11. known value below the floor    -> Insufficient
///  12. otherwise                      -> Sufficient
///
/// Step 5 is the reason an unknown reserve can never satisfy a precondition, and
/// step 11 is the reason unknown can never be read as zero.
ReserveAssessment evaluate_reserve(const std::optional<ReserveRequirement>& requirement,
                                   const ReservePolicy& policy,
                                   const ObservationRecord* observation,
                                   const std::optional<ReserveQuantity>& authority_floor,
                                   Tick now, bool revalidated);

}  // namespace ups_control
