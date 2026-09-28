// Freshness, provenance and unknown-semantics proofs for the UPS Control
// library.
//
// The contracts stated here come from include/ups_control/telemetry.hpp,
// include/ups_control/battery.hpp and docs/authority-and-transitions.md:
//
//  * Tick{0} is "never observed"; every real instant is strictly positive.
//  * The freshness window is inclusive at max_age and one tick older is stale.
//  * An observation ahead of the requested instant is fresh only within the
//    tolerated future skew.
//  * A reserve observation is either a known value with a stated quality or an
//    explicit unknown with a stated reason, and unknown is never zero.
//  * Contradictory telemetry is detected, deterministically and independently of
//    the order in which its fields were assigned, and is never used as evidence.

#include "test_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "ups_control/battery.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/status.hpp"
#include "ups_control/telemetry.hpp"
#include "ups_control/units.hpp"

using namespace ups_control;

namespace {

constexpr std::int64_t kInt64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kInt64Min = (std::numeric_limits<std::int64_t>::min)();

template <typename Id>
Id make_id(const char* text) {
  return UC_REQUIRE_OK(Id::parse(text));
}

FreshnessPolicy freshness_policy(std::int64_t max_age, std::int64_t max_future_skew) {
  return FreshnessPolicy{TickSpan{max_age}, TickSpan{max_future_skew}};
}

/// A telemetry report with no contradiction and no missing field: every case
/// below starts from this report so that exactly one documented rule is
/// exercised at a time.
TelemetryReport consistent_report() {
  TelemetryReport report;
  report.evidence = make_id<EvidenceId>("ev-0001");
  report.ups = make_id<UpsId>("ups-under-test");
  report.hardware = HardwareGeneration{1};
  report.source = make_id<SourceId>("source-0001");
  report.provenance = Provenance::AdapterReported;
  report.revision = SourceRevision{7};
  report.observed_at = Tick{1000};
  report.received_at = Tick{1001};
  report.operating = OperatingState::OnlineNormal;
  report.transfer.bypass_kind = BypassKind::Static;
  report.transfer.bypass_available = Bool3::True;
  report.transfer.bypass_qualified = Bool3::True;
  report.transfer.output_synchronized = Bool3::True;
  report.transfer.transfer_ready = Bool3::True;
  report.transfer.battery_ready = Bool3::True;
  report.capability.recharge_enabled = Bool3::True;
  report.capability.discharge_enabled = Bool3::True;
  report.battery.reserve = known_reserve(ReserveQuantity{ReserveUnit::Seconds, 600},
                                         EvidenceQuality::Measured);
  report.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 9000}, EvidenceQuality::Measured);
  report.battery.activity = BatteryActivity::Idle;
  report.battery.charge_inhibited = Bool3::False;
  report.battery.discharge_inhibited = Bool3::False;
  report.fault_present = Bool3::False;
  return report;
}

ObservationRecord record_with(const ReserveObservation& reserve, Tick observed_at) {
  ObservationRecord record;
  record.evidence = make_id<EvidenceId>("ev-0001");
  record.source = make_id<SourceId>("source-0001");
  record.provenance = Provenance::AdapterReported;
  record.revision = SourceRevision{7};
  record.observed_at = observed_at;
  record.received_at = observed_at;
  record.operating = OperatingState::OnlineNormal;
  record.battery.reserve = reserve;
  return record;
}

std::size_t count_messages(const std::vector<std::string>& messages, const std::string& needle) {
  std::size_t count = 0;
  for (const std::string& message : messages) {
    if (message.find(needle) != std::string::npos) {
      ++count;
    }
  }
  return count;
}

/// Asserts that exactly one contradiction is reported and that it names the
/// expected conflict.
void check_single_contradiction(const TelemetryReport& report, const std::string& needle,
                                const char* what) {
  const std::vector<std::string> found = find_contradictions(report);
  UC_CHECK_MSG(found.size() == 1, std::string(what) + ": expected exactly one contradiction, got " +
                                      std::to_string(found.size()));
  if (found.size() == 1) {
    UC_CHECK_MSG(found.front().find(needle) != std::string::npos,
                 std::string(what) + ": the reported contradiction '" + found.front() +
                     "' does not name the expected conflict");
  }
}

ReserveAssessment assess(const ReserveObservation& reserve, Tick observed_at, Tick now,
                         EvidenceQuality minimum_quality, std::int64_t floor_seconds) {
  ReservePolicy policy;
  policy.max_evidence_age = TickSpan{300};
  policy.max_future_skew = TickSpan{0};
  policy.minimum_quality = minimum_quality;
  const std::optional<ReserveRequirement> requirement =
      ReserveRequirement{ReserveQuantity{ReserveUnit::Seconds, floor_seconds}};
  const ObservationRecord record = record_with(reserve, observed_at);
  return evaluate_reserve(requirement, policy, &record, std::nullopt, now, true);
}

}  // namespace

UC_TEST(evidence, freshness_is_never_observed_for_the_reserved_origin_and_invalid_instants) {
  const FreshnessPolicy policy = freshness_policy(100, 0);
  UC_CHECK_EQ(evaluate_freshness(Tick{0}, Tick{1000}, policy), FreshnessVerdict::NeverObserved);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{0}, policy), FreshnessVerdict::NeverObserved);
  UC_CHECK_EQ(evaluate_freshness(Tick{0}, Tick{0}, policy), FreshnessVerdict::NeverObserved);
  UC_CHECK_EQ(evaluate_freshness(Tick{-1}, Tick{1000}, policy), FreshnessVerdict::NeverObserved);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{-1}, policy), FreshnessVerdict::NeverObserved);
  UC_CHECK_EQ(evaluate_freshness(Tick{kInt64Min}, Tick{kInt64Max}, policy),
              FreshnessVerdict::NeverObserved);
  UC_CHECK_EQ(evaluate_freshness(Tick{kInt64Max}, Tick{kInt64Min}, policy),
              FreshnessVerdict::NeverObserved);
}

UC_TEST(evidence, freshness_window_is_inclusive_at_max_age) {
  const FreshnessPolicy policy = freshness_policy(100, 0);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{1000}, policy), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{999}, Tick{1000}, policy), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{901}, Tick{1000}, policy), FreshnessVerdict::Fresh);
  // Exactly max_age old is still fresh; one tick older is stale.
  UC_CHECK_EQ(evaluate_freshness(Tick{900}, Tick{1000}, policy), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{899}, Tick{1000}, policy), FreshnessVerdict::Stale);
  UC_CHECK_EQ(evaluate_freshness(Tick{1}, Tick{1000}, policy), FreshnessVerdict::Stale);

  // A zero-length window accepts only the requested instant itself.
  const FreshnessPolicy immediate = freshness_policy(0, 0);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{1000}, immediate), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{999}, Tick{1000}, immediate), FreshnessVerdict::Stale);

  // The largest representable window still accepts the oldest valid observation.
  const FreshnessPolicy huge = freshness_policy(kInt64Max, 0);
  UC_CHECK_EQ(evaluate_freshness(Tick{1}, Tick{kInt64Max}, huge), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{kInt64Max}, Tick{kInt64Max}, huge), FreshnessVerdict::Fresh);
}

UC_TEST(evidence, a_future_observation_is_fresh_only_within_the_tolerated_skew) {
  const FreshnessPolicy no_skew = freshness_policy(100, 0);
  UC_CHECK_EQ(evaluate_freshness(Tick{1001}, Tick{1000}, no_skew),
              FreshnessVerdict::FutureObservation);
  UC_CHECK_EQ(evaluate_freshness(Tick{kInt64Max}, Tick{1000}, no_skew),
              FreshnessVerdict::FutureObservation);

  const FreshnessPolicy skew = freshness_policy(100, 5);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{1000}, skew), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{1001}, Tick{1000}, skew), FreshnessVerdict::Fresh);
  // The skew bound is inclusive, exactly like the age window.
  UC_CHECK_EQ(evaluate_freshness(Tick{1005}, Tick{1000}, skew), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{1006}, Tick{1000}, skew),
              FreshnessVerdict::FutureObservation);
  UC_CHECK_EQ(evaluate_freshness(Tick{2000}, Tick{1000}, skew),
              FreshnessVerdict::FutureObservation);

  // The future check precedes the age window: a negative window cannot turn a
  // future observation into a stale one.
  const FreshnessPolicy negative = freshness_policy(-1, 5);
  UC_CHECK_EQ(evaluate_freshness(Tick{1003}, Tick{1000}, negative), FreshnessVerdict::Fresh);
  UC_CHECK_EQ(evaluate_freshness(Tick{1006}, Tick{1000}, negative),
              FreshnessVerdict::FutureObservation);
}

UC_TEST(evidence, a_negative_max_age_yields_stale_and_never_fresh) {
  const FreshnessPolicy policy = freshness_policy(-1, 10);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{1000}, policy), FreshnessVerdict::Stale);
  UC_CHECK_EQ(evaluate_freshness(Tick{999}, Tick{1000}, policy), FreshnessVerdict::Stale);
  UC_CHECK_EQ(evaluate_freshness(Tick{1}, Tick{1000}, policy), FreshnessVerdict::Stale);
  const FreshnessPolicy very_negative = freshness_policy(kInt64Min, 10);
  UC_CHECK_EQ(evaluate_freshness(Tick{1000}, Tick{1000}, very_negative),
              FreshnessVerdict::Stale);

  // A negative window is invalid policy input, refused before it is used.
  ReservePolicy reserve_policy;
  reserve_policy.max_evidence_age = TickSpan{-1};
  UC_REQUIRE_STATUS(validate_reserve_policy(reserve_policy, ResourceLimits{}),
                    StatusCode::InvalidArgument);
  reserve_policy.max_evidence_age = TickSpan{0};
  reserve_policy.max_future_skew = TickSpan{-1};
  UC_REQUIRE_STATUS(validate_reserve_policy(reserve_policy, ResourceLimits{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(evidence, observation_age_is_exact_and_refuses_a_future_observation) {
  UC_CHECK_EQ(UC_REQUIRE_OK(observation_age(Tick{1000}, Tick{1000})), TickSpan{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(observation_age(Tick{900}, Tick{1000})), TickSpan{100});
  UC_CHECK_EQ(UC_REQUIRE_OK(observation_age(Tick{1}, Tick{1})), TickSpan{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(observation_age(Tick{1}, Tick{kInt64Max})),
              TickSpan{kInt64Max - 1});
  UC_CHECK_EQ(UC_REQUIRE_OK(observation_age(Tick{kInt64Max - 1}, Tick{kInt64Max})), TickSpan{1});

  UC_REQUIRE_STATUS(observation_age(Tick{1001}, Tick{1000}), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(observation_age(Tick{kInt64Max}, Tick{1}), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(observation_age(Tick{0}, Tick{1000}), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(observation_age(Tick{1000}, Tick{0}), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(observation_age(Tick{-1}, Tick{-1}), StatusCode::InvalidArgument);
  const Result<TickSpan> future = observation_age(Tick{1001}, Tick{1000});
  UC_CHECK(!future.ok());
  UC_CHECK(future.status().message().find("ahead") != std::string::npos);
}

UC_TEST(evidence, known_and_unknown_reserve_observations_are_built_consistently) {
  const ReserveQuantity quantity{ReserveUnit::Seconds, 600};
  const ReserveObservation known = known_reserve(quantity, EvidenceQuality::Measured);
  UC_CHECK_EQ(known.state, ReserveState::Known);
  UC_CHECK_EQ(known.quantity, quantity);
  UC_CHECK_EQ(known.unknown_reason, UnknownReason::None);
  UC_CHECK_EQ(known.quality, EvidenceQuality::Measured);
  UC_CHECK(validate_reserve_observation(known, ResourceLimits{}).ok());

  const ReserveObservation unknown = unknown_reserve(UnknownReason::SensorFault);
  UC_CHECK_EQ(unknown.state, ReserveState::Unknown);
  UC_CHECK_EQ(unknown.unknown_reason, UnknownReason::SensorFault);
  UC_CHECK_EQ(unknown.quality, EvidenceQuality::Unknown);
  // The unknown carries no value: its quantity is the struct default, never a
  // measured zero.
  UC_CHECK_EQ(unknown.quantity, (ReserveQuantity{}));
  UC_CHECK(validate_reserve_observation(unknown, ResourceLimits{}).ok());

  // Every documented unknown reason is representable and valid.
  const std::vector<UnknownReason> reasons = {
      UnknownReason::NotReported,        UnknownReason::SensorFault,
      UnknownReason::CommunicationLost,  UnknownReason::Unsupported,
      UnknownReason::Suppressed,         UnknownReason::NotApplicable,
      UnknownReason::Indeterminate};
  for (const UnknownReason reason : reasons) {
    const ReserveObservation built = unknown_reserve(reason);
    UC_CHECK_EQ(built.unknown_reason, reason);
    UC_CHECK_EQ(built.state, ReserveState::Unknown);
    UC_CHECK(validate_reserve_observation(built, ResourceLimits{}).ok());
  }

  // A default-constructed observation is an explicit "not reported" unknown.
  const ReserveObservation defaulted;
  UC_CHECK_EQ(defaulted.state, ReserveState::Unknown);
  UC_CHECK_EQ(defaulted.unknown_reason, UnknownReason::NotReported);
  UC_CHECK_EQ(defaulted.quality, EvidenceQuality::Unknown);
  UC_CHECK(validate_reserve_observation(defaulted, ResourceLimits{}).ok());

  // A known value must state its quality, so known_reserve with Unknown quality
  // produces an observation that validation refuses.
  const ReserveObservation unqualified = known_reserve(quantity, EvidenceQuality::Unknown);
  UC_REQUIRE_STATUS(validate_reserve_observation(unqualified, ResourceLimits{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(evidence, validate_reserve_observation_refuses_conflated_known_and_unknown_states) {
  const ReserveQuantity quantity{ReserveUnit::Seconds, 600};
  const ResourceLimits limits;

  ReserveObservation known_with_reason = known_reserve(quantity, EvidenceQuality::Measured);
  known_with_reason.unknown_reason = UnknownReason::SensorFault;
  UC_REQUIRE_STATUS(validate_reserve_observation(known_with_reason, limits),
                    StatusCode::InvalidArgument);

  ReserveObservation unknown_with_quality = unknown_reserve(UnknownReason::NotReported);
  unknown_with_quality.quality = EvidenceQuality::Measured;
  UC_REQUIRE_STATUS(validate_reserve_observation(unknown_with_quality, limits),
                    StatusCode::InvalidArgument);

  ReserveObservation unknown_without_reason = unknown_reserve(UnknownReason::NotReported);
  unknown_without_reason.unknown_reason = UnknownReason::None;
  UC_REQUIRE_STATUS(validate_reserve_observation(unknown_without_reason, limits),
                    StatusCode::InvalidArgument);

  ReserveObservation both_wrong = unknown_reserve(UnknownReason::NotReported);
  both_wrong.unknown_reason = UnknownReason::None;
  both_wrong.quality = EvidenceQuality::Derived;
  UC_REQUIRE_STATUS(validate_reserve_observation(both_wrong, limits),
                    StatusCode::InvalidArgument);

  // A known value is still bound by the quantity's own range and the limits.
  const ReserveObservation negative =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, -1}, EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_reserve_observation(negative, limits), StatusCode::InvalidArgument);
  const ReserveObservation out_of_range =
      known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 10'001},
                    EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_reserve_observation(out_of_range, limits),
                    StatusCode::InvalidArgument);
  const ReserveObservation over_limit =
      known_reserve(ReserveQuantity{ReserveUnit::MilliwattHours, 1'000'000'000'000'001},
                    EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_reserve_observation(over_limit, limits),
                    StatusCode::LimitExceeded);
}

UC_TEST(evidence, an_unknown_reserve_is_never_read_as_zero) {
  // A floor of zero would be satisfied by a zero quantity. The unknown must not
  // satisfy it, which is what "unknown is not zero" means operationally.
  const ReserveAssessment from_default = assess(ReserveObservation{}, Tick{1000}, Tick{1000},
                                                EvidenceQuality::Estimated, 0);
  UC_CHECK_EQ(from_default.outcome, ReserveOutcome::Indeterminate);
  UC_CHECK_EQ(from_default.reason, ReserveIndeterminacy::EvidenceUnknown);
  UC_CHECK(!from_default.observed.has_value());
  UC_CHECK(from_default.detail.find("unknown is not zero") != std::string::npos);

  const ReserveAssessment from_sensor_fault =
      assess(unknown_reserve(UnknownReason::SensorFault), Tick{1000}, Tick{1000},
             EvidenceQuality::Estimated, 0);
  UC_CHECK_EQ(from_sensor_fault.outcome, ReserveOutcome::Indeterminate);
  UC_CHECK_EQ(from_sensor_fault.reason, ReserveIndeterminacy::EvidenceUnknown);

  // The contrast that gives the previous results meaning: a known zero does
  // satisfy a zero floor, and is insufficient against a floor of one second.
  const ReserveObservation known_zero =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 0}, EvidenceQuality::Measured);
  const ReserveAssessment zero_floor =
      assess(known_zero, Tick{1000}, Tick{1000}, EvidenceQuality::Estimated, 0);
  UC_CHECK_EQ(zero_floor.outcome, ReserveOutcome::Sufficient);
  UC_CHECK(zero_floor.observed.has_value());
  UC_CHECK_EQ(zero_floor.observed.value(), (ReserveQuantity{ReserveUnit::Seconds, 0}));

  const ReserveAssessment one_second_floor =
      assess(known_zero, Tick{1000}, Tick{1000}, EvidenceQuality::Estimated, 1);
  UC_CHECK_EQ(one_second_floor.outcome, ReserveOutcome::Insufficient);
  UC_CHECK_EQ(one_second_floor.reason, ReserveIndeterminacy::None);

  // An unknown is also never stale-or-insufficient: the indeterminacy is stated
  // as unknown evidence, whichever instant is asked about.
  const ReserveAssessment old_unknown = assess(unknown_reserve(UnknownReason::CommunicationLost),
                                               Tick{1}, Tick{1000}, EvidenceQuality::Estimated, 0);
  UC_CHECK_EQ(old_unknown.outcome, ReserveOutcome::Indeterminate);
  UC_CHECK_EQ(old_unknown.reason, ReserveIndeterminacy::EvidenceUnknown);
}

UC_TEST(evidence, evidence_quality_rank_orders_measured_above_estimated_above_derived_above_unknown) {
  const int measured = evidence_quality_rank(EvidenceQuality::Measured);
  const int estimated = evidence_quality_rank(EvidenceQuality::Estimated);
  const int derived = evidence_quality_rank(EvidenceQuality::Derived);
  const int unknown = evidence_quality_rank(EvidenceQuality::Unknown);

  UC_CHECK_MSG(measured > estimated, "measured must rank above estimated");
  UC_CHECK_MSG(estimated > derived, "estimated must rank above derived");
  UC_CHECK_MSG(derived > unknown, "derived must rank above unknown");

  // The four ranks are distinct, so the order is a strict total order rather
  // than a partial one with ties.
  const std::vector<EvidenceQuality> qualities = {EvidenceQuality::Unknown,
                                                  EvidenceQuality::Measured,
                                                  EvidenceQuality::Estimated,
                                                  EvidenceQuality::Derived};
  for (std::size_t left = 0; left < qualities.size(); ++left) {
    for (std::size_t right = left + 1; right < qualities.size(); ++right) {
      UC_CHECK_MSG(evidence_quality_rank(qualities[left]) != evidence_quality_rank(qualities[right]),
                   "two distinct evidence qualities share a rank");
    }
  }
  // Unknown is the weakest: no value ranks below it, and every other value is
  // strictly above it.
  for (const EvidenceQuality quality : qualities) {
    UC_CHECK_MSG(evidence_quality_rank(quality) >= evidence_quality_rank(EvidenceQuality::Unknown),
                 "unknown is not the weakest evidence quality");
  }
  UC_CHECK(!qualities.empty());
  UC_CHECK(evidence_quality_rank(EvidenceQuality::Measured) >
            evidence_quality_rank(EvidenceQuality::Unknown));
}

UC_TEST(evidence, find_contradictions_accepts_a_consistent_report) {
  UC_CHECK(find_contradictions(consistent_report()).empty());

  // Unknown never asserts anything, so a report that reports nothing is not
  // contradictory.
  TelemetryReport silent;
  silent.evidence = make_id<EvidenceId>("ev-0002");
  silent.ups = make_id<UpsId>("ups-under-test");
  silent.hardware = HardwareGeneration{1};
  silent.source = make_id<SourceId>("source-0002");
  silent.provenance = Provenance::AdapterReported;
  silent.observed_at = Tick{1000};
  silent.received_at = Tick{1000};
  UC_CHECK(find_contradictions(silent).empty());

  // These documented combinations are consistent, not contradictions.
  TelemetryReport maintenance = consistent_report();
  maintenance.operating = OperatingState::MaintenanceBypass;
  maintenance.transfer.bypass_kind = BypassKind::Maintenance;
  UC_CHECK(find_contradictions(maintenance).empty());

  TelemetryReport recharging = consistent_report();
  recharging.operating = OperatingState::Recharge;
  recharging.battery.activity = BatteryActivity::Charging;
  UC_CHECK(find_contradictions(recharging).empty());

  TelemetryReport online_charging = consistent_report();
  online_charging.battery.activity = BatteryActivity::Charging;
  UC_CHECK(find_contradictions(online_charging).empty());

  TelemetryReport on_battery = consistent_report();
  on_battery.operating = OperatingState::OnlineBattery;
  on_battery.battery.activity = BatteryActivity::Discharging;
  UC_CHECK(find_contradictions(on_battery).empty());

  TelemetryReport faulted = consistent_report();
  faulted.operating = OperatingState::Faulted;
  faulted.fault_present = Bool3::True;
  UC_CHECK(find_contradictions(faulted).empty());

  TelemetryReport unknown_state = consistent_report();
  unknown_state.operating = OperatingState::Unknown;
  unknown_state.fault_present = Bool3::True;
  UC_CHECK(find_contradictions(unknown_state).empty());
}

UC_TEST(evidence, find_contradictions_detects_a_fault_that_contradicts_the_reported_state) {
  TelemetryReport asserted_while_online = consistent_report();
  asserted_while_online.fault_present = Bool3::True;
  check_single_contradiction(asserted_while_online, "fault is asserted",
                             "a fault asserted while online normal");

  TelemetryReport asserted_while_testing = consistent_report();
  asserted_while_testing.operating = OperatingState::SelfTest;
  asserted_while_testing.fault_present = Bool3::True;
  check_single_contradiction(asserted_while_testing, "fault is asserted",
                             "a fault asserted during a self test");

  TelemetryReport faulted_without_a_fault = consistent_report();
  faulted_without_a_fault.operating = OperatingState::Faulted;
  faulted_without_a_fault.fault_present = Bool3::False;
  check_single_contradiction(faulted_without_a_fault, "no fault is asserted",
                             "a faulted state with no asserted fault");
}

UC_TEST(evidence, find_contradictions_detects_battery_activity_that_contradicts_the_state) {
  TelemetryReport charging_on_battery = consistent_report();
  charging_on_battery.operating = OperatingState::OnlineBattery;
  charging_on_battery.battery.activity = BatteryActivity::Charging;
  check_single_contradiction(charging_on_battery, "on battery while the battery reports charging",
                             "charging while on battery");

  TelemetryReport discharging_while_recharging = consistent_report();
  discharging_while_recharging.operating = OperatingState::Recharge;
  discharging_while_recharging.battery.activity = BatteryActivity::Discharging;
  check_single_contradiction(discharging_while_recharging,
                             "recharging while the battery reports discharging",
                             "discharging while recharging");

  TelemetryReport charging_while_inhibited = consistent_report();
  charging_while_inhibited.battery.activity = BatteryActivity::Charging;
  charging_while_inhibited.battery.charge_inhibited = Bool3::True;
  check_single_contradiction(charging_while_inhibited,
                             "charging is reported while charging is reported inhibited",
                             "charging while charge is inhibited");

  TelemetryReport discharging_while_inhibited = consistent_report();
  discharging_while_inhibited.battery.activity = BatteryActivity::Discharging;
  discharging_while_inhibited.battery.discharge_inhibited = Bool3::True;
  check_single_contradiction(discharging_while_inhibited,
                             "discharging is reported while discharging is reported inhibited",
                             "discharging while discharge is inhibited");
}

UC_TEST(evidence, find_contradictions_detects_bypass_and_reserve_conflicts) {
  TelemetryReport bypass_without_path = consistent_report();
  bypass_without_path.operating = OperatingState::MaintenanceBypass;
  bypass_without_path.transfer.bypass_kind = BypassKind::None;
  check_single_contradiction(bypass_without_path, "no bypass path is reported",
                             "maintenance bypass with no bypass path");

  TelemetryReport on_battery_with_zero_reserve = consistent_report();
  on_battery_with_zero_reserve.operating = OperatingState::OnlineBattery;
  on_battery_with_zero_reserve.battery.reserve =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 0}, EvidenceQuality::Measured);
  check_single_contradiction(on_battery_with_zero_reserve, "exactly zero",
                             "on battery with exactly zero reserve");

  TelemetryReport known_without_quality = consistent_report();
  ReserveObservation unqualified =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 600}, EvidenceQuality::Measured);
  unqualified.quality = EvidenceQuality::Unknown;
  known_without_quality.battery.reserve = unqualified;
  check_single_contradiction(known_without_quality, "unknown evidence quality",
                             "a known reserve with unknown quality");
}

UC_TEST(evidence, find_contradictions_detects_a_state_that_discharges_on_a_disabled_path) {
  TelemetryReport discharge_disabled_on_battery = consistent_report();
  discharge_disabled_on_battery.operating = OperatingState::OnlineBattery;
  discharge_disabled_on_battery.capability.discharge_enabled = Bool3::False;
  check_single_contradiction(discharge_disabled_on_battery,
                             "discharge path is reported disabled",
                             "discharging on battery with the discharge path disabled");

  TelemetryReport discharge_disabled_in_test = consistent_report();
  discharge_disabled_in_test.operating = OperatingState::BatteryTest;
  discharge_disabled_in_test.capability.discharge_enabled = Bool3::False;
  check_single_contradiction(discharge_disabled_in_test, "discharge path is reported disabled",
                             "a battery test with the discharge path disabled");

  TelemetryReport recharge_disabled = consistent_report();
  recharge_disabled.operating = OperatingState::Recharge;
  recharge_disabled.capability.recharge_enabled = Bool3::False;
  check_single_contradiction(recharge_disabled, "recharge path is reported disabled",
                             "recharging with the recharge path disabled");

  // Without a discharge the disabled path is not a contradiction.
  TelemetryReport idle_with_disabled_path = consistent_report();
  idle_with_disabled_path.capability.discharge_enabled = Bool3::False;
  idle_with_disabled_path.capability.recharge_enabled = Bool3::False;
  UC_CHECK(find_contradictions(idle_with_disabled_path).empty());
}

UC_TEST(evidence, find_contradictions_is_deterministic_and_independent_of_field_assignment_order) {
  TelemetryReport report = consistent_report();
  report.operating = OperatingState::OnlineBattery;
  report.battery.activity = BatteryActivity::Charging;
  report.battery.reserve =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 0}, EvidenceQuality::Measured);
  report.capability.discharge_enabled = Bool3::False;

  const std::vector<std::string> first = find_contradictions(report);
  const std::vector<std::string> second = find_contradictions(report);
  UC_CHECK_MSG(first == second, "find_contradictions returned different results for equal reports");
  UC_CHECK_EQ(first.size(), std::size_t{3});
  UC_CHECK_EQ(count_messages(first, "the battery reports charging"), std::size_t{1});
  UC_CHECK_EQ(count_messages(first, "exactly zero"), std::size_t{1});
  UC_CHECK_EQ(count_messages(first, "discharge path is reported disabled"), std::size_t{1});

  // The same logical report, built by assigning the same fields in a different
  // order, produces byte-identical findings.
  TelemetryReport reordered;
  reordered.capability.discharge_enabled = Bool3::False;
  reordered.battery.reserve =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 0}, EvidenceQuality::Measured);
  reordered.battery.activity = BatteryActivity::Charging;
  reordered.operating = OperatingState::OnlineBattery;
  reordered.fault_present = Bool3::False;
  reordered.battery.discharge_inhibited = Bool3::False;
  reordered.battery.charge_inhibited = Bool3::False;
  reordered.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 9000}, EvidenceQuality::Measured);
  reordered.capability.recharge_enabled = Bool3::True;
  reordered.transfer = consistent_report().transfer;
  reordered.received_at = Tick{1001};
  reordered.observed_at = Tick{1000};
  reordered.revision = SourceRevision{7};
  reordered.provenance = Provenance::AdapterReported;
  reordered.source = make_id<SourceId>("source-0001");
  reordered.hardware = HardwareGeneration{1};
  reordered.ups = make_id<UpsId>("ups-under-test");
  reordered.evidence = make_id<EvidenceId>("ev-0001");
  const std::vector<std::string> reordered_findings = find_contradictions(reordered);
  UC_CHECK_MSG(reordered_findings == first,
               "the findings depend on the order in which the report fields were assigned");
  for (const std::string& message : first) {
    UC_CHECK_MSG(!message.empty(), "a reported contradiction has an empty explanation");
  }
}

UC_TEST(evidence, validate_telemetry_report_accepts_a_complete_report) {
  UC_CHECK(validate_telemetry_report(consistent_report(), ResourceLimits{}).ok());

  TelemetryReport without_soc = consistent_report();
  without_soc.battery.state_of_charge = ReserveObservation{};
  UC_CHECK(validate_telemetry_report(without_soc, ResourceLimits{}).ok());

  TelemetryReport unknown_reserve_report = consistent_report();
  unknown_reserve_report.battery.reserve = unknown_reserve(UnknownReason::NotReported);
  UC_CHECK(validate_telemetry_report(unknown_reserve_report, ResourceLimits{}).ok());

  // Every documented provenance other than unknown is accepted.
  const std::vector<Provenance> provenances = {Provenance::AdapterReported,
                                               Provenance::OperatorAsserted,
                                               Provenance::SyntheticAdapter,
                                               Provenance::ImportedAudit};
  for (const Provenance provenance : provenances) {
    TelemetryReport report = consistent_report();
    report.provenance = provenance;
    UC_CHECK_MSG(validate_telemetry_report(report, ResourceLimits{}).ok(),
                 std::string("provenance ") + to_string(provenance) + " must be accepted");
  }
}

UC_TEST(evidence, validate_telemetry_report_refuses_missing_provenance) {
  TelemetryReport report = consistent_report();
  report.provenance = Provenance::Unknown;
  UC_REQUIRE_STATUS(validate_telemetry_report(report, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport defaulted;
  defaulted.evidence = make_id<EvidenceId>("ev-0001");
  defaulted.ups = make_id<UpsId>("ups-under-test");
  defaulted.source = make_id<SourceId>("source-0001");
  defaulted.observed_at = Tick{1000};
  defaulted.received_at = Tick{1000};
  UC_REQUIRE_STATUS(validate_telemetry_report(defaulted, ResourceLimits{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(evidence, validate_telemetry_report_refuses_non_positive_instants) {
  TelemetryReport zero_observed = consistent_report();
  zero_observed.observed_at = Tick{0};
  UC_REQUIRE_STATUS(validate_telemetry_report(zero_observed, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport negative_observed = consistent_report();
  negative_observed.observed_at = Tick{-1};
  UC_REQUIRE_STATUS(validate_telemetry_report(negative_observed, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport zero_received = consistent_report();
  zero_received.received_at = Tick{0};
  UC_REQUIRE_STATUS(validate_telemetry_report(zero_received, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport negative_received = consistent_report();
  negative_received.received_at = Tick{kInt64Min};
  UC_REQUIRE_STATUS(validate_telemetry_report(negative_received, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport both_zero = consistent_report();
  both_zero.observed_at = Tick{0};
  both_zero.received_at = Tick{0};
  UC_REQUIRE_STATUS(validate_telemetry_report(both_zero, ResourceLimits{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(evidence, validate_telemetry_report_refuses_unset_identifiers) {
  // An identifier that was never parsed is the only invalid identifier the typed
  // id makes representable: BasicId::parse refuses everything else, so an
  // unset id is exactly the case a report can carry.
  TelemetryReport empty_evidence = consistent_report();
  empty_evidence.evidence = EvidenceId{};
  UC_REQUIRE_STATUS(validate_telemetry_report(empty_evidence, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport empty_source = consistent_report();
  empty_source.source = SourceId{};
  UC_REQUIRE_STATUS(validate_telemetry_report(empty_source, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport empty_ups = consistent_report();
  empty_ups.ups = UpsId{};
  UC_REQUIRE_STATUS(validate_telemetry_report(empty_ups, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  // Every family cannot be built with invalid text in the first place.
  UC_CHECK(!UpsId::parse("ups under test").ok());
  UC_CHECK(!SourceId::parse("source/0001").ok());
  UC_CHECK(!EvidenceId::parse(std::string(129, 'e')).ok());
}

UC_TEST(evidence, validate_telemetry_report_applies_the_configured_identifier_bound) {
  ResourceLimits tight;
  tight.max_identifier_bytes = 4;

  TelemetryReport long_evidence = consistent_report();
  UC_REQUIRE_STATUS(validate_telemetry_report(long_evidence, tight), StatusCode::LimitExceeded);

  TelemetryReport short_evidence = consistent_report();
  short_evidence.evidence = make_id<EvidenceId>("ev1");
  UC_REQUIRE_STATUS(validate_telemetry_report(short_evidence, tight), StatusCode::LimitExceeded);

  TelemetryReport short_evidence_and_ups = consistent_report();
  short_evidence_and_ups.evidence = make_id<EvidenceId>("ev1");
  short_evidence_and_ups.ups = make_id<UpsId>("ups");
  UC_REQUIRE_STATUS(validate_telemetry_report(short_evidence_and_ups, tight),
                    StatusCode::LimitExceeded);

  TelemetryReport all_short = consistent_report();
  all_short.evidence = make_id<EvidenceId>("ev1");
  all_short.ups = make_id<UpsId>("ups");
  all_short.source = make_id<SourceId>("src");
  UC_CHECK(validate_telemetry_report(all_short, tight).ok());
  // The same report is accepted under the default bound, which proves the
  // previous refusals came from the configured bound and not from the text.
  UC_CHECK(validate_telemetry_report(consistent_report(), ResourceLimits{}).ok());
}

UC_TEST(evidence, validate_telemetry_report_refuses_an_invalid_reserve_observation) {
  TelemetryReport reserve_with_reason = consistent_report();
  ReserveObservation conflated =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 600}, EvidenceQuality::Measured);
  conflated.unknown_reason = UnknownReason::SensorFault;
  reserve_with_reason.battery.reserve = conflated;
  UC_REQUIRE_STATUS(validate_telemetry_report(reserve_with_reason, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport reserve_without_quality = consistent_report();
  reserve_without_quality.battery.reserve =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 600}, EvidenceQuality::Unknown);
  UC_REQUIRE_STATUS(validate_telemetry_report(reserve_without_quality, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport unknown_without_reason = consistent_report();
  ReserveObservation no_reason = unknown_reserve(UnknownReason::NotReported);
  no_reason.unknown_reason = UnknownReason::None;
  unknown_without_reason.battery.reserve = no_reason;
  UC_REQUIRE_STATUS(validate_telemetry_report(unknown_without_reason, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport soc_without_reason = consistent_report();
  ReserveObservation soc = known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 9000},
                                         EvidenceQuality::Measured);
  soc.unknown_reason = UnknownReason::Suppressed;
  soc_without_reason.battery.state_of_charge = soc;
  UC_REQUIRE_STATUS(validate_telemetry_report(soc_without_reason, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport negative_reserve = consistent_report();
  negative_reserve.battery.reserve =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, -1}, EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_telemetry_report(negative_reserve, ResourceLimits{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(evidence, validate_telemetry_report_requires_a_basis_point_state_of_charge) {
  TelemetryReport seconds_soc = consistent_report();
  seconds_soc.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 50}, EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_telemetry_report(seconds_soc, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport energy_soc = consistent_report();
  energy_soc.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::KilowattHours, 3}, EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_telemetry_report(energy_soc, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport out_of_range_soc = consistent_report();
  out_of_range_soc.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 10'001}, EvidenceQuality::Measured);
  UC_REQUIRE_STATUS(validate_telemetry_report(out_of_range_soc, ResourceLimits{}),
                    StatusCode::InvalidArgument);

  TelemetryReport full = consistent_report();
  full.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 10'000}, EvidenceQuality::Measured);
  UC_CHECK(validate_telemetry_report(full, ResourceLimits{}).ok());
  TelemetryReport empty = consistent_report();
  empty.battery.state_of_charge =
      known_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 0}, EvidenceQuality::Measured);
  UC_CHECK(validate_telemetry_report(empty, ResourceLimits{}).ok());
}

UC_TEST(evidence, is_synthetic_is_true_only_for_the_synthetic_adapter_provenance) {
  UC_CHECK(is_synthetic(Provenance::SyntheticAdapter));
  for (int index = 0; index <= 4; ++index) {
    const Provenance provenance = static_cast<Provenance>(index);
    const bool expected = provenance == Provenance::SyntheticAdapter;
    UC_CHECK_MSG(is_synthetic(provenance) == expected,
                 std::string("is_synthetic(") + to_string(provenance) + ") is not " +
                     (expected ? "true" : "false"));
  }
  UC_CHECK(!is_synthetic(Provenance::Unknown));
  UC_CHECK(!is_synthetic(Provenance::AdapterReported));
  UC_CHECK(!is_synthetic(Provenance::OperatorAsserted));
  UC_CHECK(!is_synthetic(Provenance::ImportedAudit));
}

UC_TEST(evidence, provenance_quality_reason_and_activity_tokens_round_trip) {
  const std::vector<Provenance> provenances = {Provenance::Unknown, Provenance::AdapterReported,
                                               Provenance::OperatorAsserted,
                                               Provenance::SyntheticAdapter,
                                               Provenance::ImportedAudit};
  std::vector<std::string> provenance_tokens;
  for (const Provenance provenance : provenances) {
    const std::string token = to_string(provenance);
    UC_CHECK_MSG(!token.empty(), "a provenance has an empty token");
    UC_CHECK_MSG(std::find(provenance_tokens.begin(), provenance_tokens.end(), token) ==
                     provenance_tokens.end(),
                 "two provenances share the token '" + token + "'");
    provenance_tokens.push_back(token);
    UC_CHECK_EQ(UC_REQUIRE_OK(parse_provenance(token)), provenance);
  }
  for (const std::string& token : {"", "AdapterReported", "adapter reported", "hardware",
                                   "synthetic", "audit"}) {
    UC_CHECK_MSG(!parse_provenance(token).ok(),
                 "parse_provenance accepted the unknown token '" + token + "'");
  }

  const std::vector<EvidenceQuality> qualities = {EvidenceQuality::Unknown,
                                                  EvidenceQuality::Measured,
                                                  EvidenceQuality::Estimated,
                                                  EvidenceQuality::Derived};
  std::vector<std::string> quality_tokens;
  for (const EvidenceQuality quality : qualities) {
    const std::string token = to_string(quality);
    UC_CHECK_MSG(!token.empty(), "an evidence quality has an empty token");
    UC_CHECK_MSG(std::find(quality_tokens.begin(), quality_tokens.end(), token) ==
                     quality_tokens.end(),
                 "two evidence qualities share the token '" + token + "'");
    quality_tokens.push_back(token);
    UC_CHECK_EQ(UC_REQUIRE_OK(parse_evidence_quality(token)), quality);
  }
  UC_CHECK(!parse_evidence_quality("Measured").ok());
  UC_CHECK(!parse_evidence_quality("measured ").ok());

  for (int index = 0; index <= 7; ++index) {
    const UnknownReason reason = static_cast<UnknownReason>(index);
    const std::string token = to_string(reason);
    UC_CHECK_MSG(!token.empty(), "an unknown reason has an empty token");
    UC_CHECK_EQ(UC_REQUIRE_OK(parse_unknown_reason(token)), reason);
  }
  UC_CHECK(!parse_unknown_reason("not reported").ok());
  UC_CHECK(!parse_unknown_reason("NotReported").ok());

  for (int index = 0; index <= 4; ++index) {
    const BatteryActivity activity = static_cast<BatteryActivity>(index);
    const std::string token = to_string(activity);
    UC_CHECK_MSG(!token.empty(), "a battery activity has an empty token");
    UC_CHECK_EQ(UC_REQUIRE_OK(parse_battery_activity(token)), activity);
  }
  UC_CHECK(!parse_battery_activity("Charging").ok());
  UC_CHECK(!parse_battery_activity("charge").ok());

  // ReserveState and FreshnessVerdict are rendered but not parsed; their tokens
  // must still be distinct and non-empty.
  UC_CHECK_EQ(std::string(to_string(ReserveState::Unknown)), std::string("unknown"));
  UC_CHECK_EQ(std::string(to_string(ReserveState::Known)), std::string("known"));
  const std::vector<FreshnessVerdict> verdicts = {
      FreshnessVerdict::Fresh,        FreshnessVerdict::Stale,
      FreshnessVerdict::NeverObserved, FreshnessVerdict::FutureObservation,
      FreshnessVerdict::NotRevalidated};
  std::vector<std::string> verdict_tokens;
  for (const FreshnessVerdict verdict : verdicts) {
    const std::string token = to_string(verdict);
    UC_CHECK_MSG(!token.empty() && token != "unknown", "a freshness verdict has no stable token");
    UC_CHECK_MSG(std::find(verdict_tokens.begin(), verdict_tokens.end(), token) ==
                     verdict_tokens.end(),
                 "two freshness verdicts share the token '" + token + "'");
    verdict_tokens.push_back(token);
  }
}
