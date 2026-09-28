#include "ups_control/battery.hpp"

#include <string>
#include <vector>

namespace ups_control {

const char* to_string(ReserveOutcome outcome) noexcept {
  switch (outcome) {
    case ReserveOutcome::NotRequired: return "not_required";
    case ReserveOutcome::Sufficient: return "sufficient";
    case ReserveOutcome::Insufficient: return "insufficient";
    case ReserveOutcome::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

const char* to_string(ReserveIndeterminacy reason) noexcept {
  switch (reason) {
    case ReserveIndeterminacy::None: return "none";
    case ReserveIndeterminacy::NoFloorDeclared: return "no_floor_declared";
    case ReserveIndeterminacy::EvidenceMissing: return "evidence_missing";
    case ReserveIndeterminacy::EvidenceUnknown: return "evidence_unknown";
    case ReserveIndeterminacy::EvidenceStale: return "evidence_stale";
    case ReserveIndeterminacy::EvidenceFuture: return "evidence_future";
    case ReserveIndeterminacy::UnitMismatch: return "unit_mismatch";
    case ReserveIndeterminacy::QualityBelowMinimum: return "quality_below_minimum";
    case ReserveIndeterminacy::NotRevalidated: return "not_revalidated";
    case ReserveIndeterminacy::Contradictory: return "contradictory";
    case ReserveIndeterminacy::ClockUnavailable: return "clock_unavailable";
  }
  return "unknown";
}

Status validate_reserve_policy(const ReservePolicy& policy, const ResourceLimits& limits) {
  if (policy.max_evidence_age.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "reserve policy max_evidence_age must not be negative");
  }
  if (policy.max_future_skew.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "reserve policy max_future_skew must not be negative");
  }
  if (policy.discharge_floor.has_value()) {
    const Status status = validate_reserve_quantity(policy.discharge_floor.value(), limits);
    if (!status.ok()) {
      return Status::error(status.code(), "reserve policy discharge floor: " + status.message());
    }
    if (policy.discharge_floor->unit == ReserveUnit::BasisPoints) {
      return Status::error(
          StatusCode::InvalidArgument,
          "a discharge floor stated in basis points is a fraction of nameplate, not a reserve "
          "floor; state the floor in the same unit the evidence is reported in");
    }
  }
  return Status::success();
}

namespace {

void set_indeterminate(ReserveAssessment& assessment, ReserveIndeterminacy reason,
                       std::string detail) {
  assessment.outcome = ReserveOutcome::Indeterminate;
  assessment.reason = reason;
  assessment.detail = std::move(detail);
}

}  // namespace

ReserveAssessment evaluate_reserve(const std::optional<ReserveRequirement>& requirement,
                                   const ReservePolicy& policy,
                                   const ObservationRecord* observation,
                                   const std::optional<ReserveQuantity>& authority_floor, Tick now,
                                   bool revalidated) {
  ReserveAssessment assessment;

  std::vector<ReserveQuantity> floors;
  if (requirement.has_value()) {
    floors.push_back(requirement->minimum);
  }
  if (authority_floor.has_value()) {
    floors.push_back(authority_floor.value());
  }
  if (policy.discharge_floor.has_value()) {
    floors.push_back(policy.discharge_floor.value());
  }

  if (floors.empty()) {
    assessment.outcome = ReserveOutcome::NotRequired;
    assessment.reason = ReserveIndeterminacy::NoFloorDeclared;
    assessment.detail = "no reserve floor is declared by the request, by an authority grant, or by policy";
    return assessment;
  }

  if (!revalidated) {
    set_indeterminate(assessment, ReserveIndeterminacy::NotRevalidated,
                      "the store has been recovered and not revalidated at this instant, so no "
                      "reserve evidence may be treated as current");
    return assessment;
  }

  if (!is_valid_instant(now)) {
    set_indeterminate(assessment, ReserveIndeterminacy::ClockUnavailable,
                      "no valid requested instant was supplied");
    return assessment;
  }

  if (observation == nullptr) {
    set_indeterminate(assessment, ReserveIndeterminacy::EvidenceMissing,
                      "no telemetry has ever been recorded for this unit");
    return assessment;
  }

  if (observation->contradictory) {
    set_indeterminate(assessment, ReserveIndeterminacy::Contradictory,
                      "the recorded observation contradicts itself: " +
                          observation->contradiction_detail);
    return assessment;
  }

  assessment.evidence = observation->evidence;
  assessment.observed_at = observation->observed_at;

  const ReserveObservation& reserve = observation->battery.reserve;
  if (reserve.state == ReserveState::Unknown) {
    set_indeterminate(assessment, ReserveIndeterminacy::EvidenceUnknown,
                      std::string("the device reports the reserve as unknown (") +
                          to_string(reserve.unknown_reason) +
                          "); unknown is not zero and cannot satisfy a reserve floor");
    return assessment;
  }

  if (!is_valid_instant(observation->observed_at)) {
    set_indeterminate(assessment, ReserveIndeterminacy::EvidenceMissing,
                      "the recorded observation carries no valid observation instant");
    return assessment;
  }

  const FreshnessPolicy freshness{policy.max_evidence_age, policy.max_future_skew};
  const FreshnessVerdict verdict =
      evaluate_freshness(observation->observed_at, now, freshness);
  assessment.freshness = verdict;
  switch (verdict) {
    case FreshnessVerdict::Stale: {
      const Result<TickSpan> age = observation_age(observation->observed_at, now);
      if (age.ok()) {
        assessment.age = age.value();
      }
      set_indeterminate(assessment, ReserveIndeterminacy::EvidenceStale,
                        "the reserve evidence was observed at " +
                            std::to_string(observation->observed_at.value()) +
                            " and is older than the accepted window of " +
                            std::to_string(policy.max_evidence_age.value()) + " ticks");
      return assessment;
    }
    case FreshnessVerdict::FutureObservation:
      set_indeterminate(assessment, ReserveIndeterminacy::EvidenceFuture,
                        "the reserve evidence claims an instant ahead of the requested instant "
                        "beyond the tolerated skew");
      return assessment;
    case FreshnessVerdict::NeverObserved:
      set_indeterminate(assessment, ReserveIndeterminacy::EvidenceMissing,
                        "the reserve evidence carries no usable observation instant");
      return assessment;
    case FreshnessVerdict::NotRevalidated:
      set_indeterminate(assessment, ReserveIndeterminacy::NotRevalidated,
                        "the reserve evidence has not been revalidated");
      return assessment;
    case FreshnessVerdict::Fresh:
      break;
  }
  const Result<TickSpan> age = observation_age(observation->observed_at, now);
  if (age.ok()) {
    assessment.age = age.value();
  }

  for (const ReserveQuantity& floor : floors) {
    if (!reserve_units_comparable(reserve.quantity.unit, floor.unit)) {
      set_indeterminate(assessment, ReserveIndeterminacy::UnitMismatch,
                        "the reserve is reported in " +
                            std::string(to_string(reserve.quantity.unit)) +
                            " but a declared floor is stated in " +
                            std::string(to_string(floor.unit)) +
                            "; this runtime does not convert across reserve classes, so the floor "
                            "cannot be decided");
      return assessment;
    }
  }

  if (evidence_quality_rank(reserve.quality) < evidence_quality_rank(policy.minimum_quality)) {
    set_indeterminate(assessment, ReserveIndeterminacy::QualityBelowMinimum,
                      std::string("the reserve evidence is ") + to_string(reserve.quality) +
                          " but policy requires at least " + to_string(policy.minimum_quality));
    return assessment;
  }

  ReserveQuantity highest = floors.front();
  for (std::size_t index = 1; index < floors.size(); ++index) {
    const Result<int> comparison = compare_reserve(floors[index], highest);
    if (!comparison.ok()) {
      set_indeterminate(assessment, ReserveIndeterminacy::UnitMismatch, comparison.status().message());
      return assessment;
    }
    if (comparison.value() > 0) {
      highest = floors[index];
    }
  }
  assessment.required = highest;
  assessment.observed = reserve.quantity;

  const Result<int> comparison = compare_reserve(reserve.quantity, highest);
  if (!comparison.ok()) {
    set_indeterminate(assessment, ReserveIndeterminacy::UnitMismatch, comparison.status().message());
    return assessment;
  }
  if (comparison.value() < 0) {
    assessment.outcome = ReserveOutcome::Insufficient;
    assessment.reason = ReserveIndeterminacy::None;
    assessment.detail = "the reserve is " + format_reserve(reserve.quantity) +
                        ", below the required floor of " + format_reserve(highest);
    return assessment;
  }
  assessment.outcome = ReserveOutcome::Sufficient;
  assessment.reason = ReserveIndeterminacy::None;
  assessment.detail = "the reserve is " + format_reserve(reserve.quantity) +
                      ", at or above the required floor of " + format_reserve(highest) +
                      ", observed " + std::to_string(assessment.age.value()) + " ticks ago";
  return assessment;
}

}  // namespace ups_control
