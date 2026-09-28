#include "ups_control/telemetry.hpp"

#include <algorithm>
#include <string>

#include "ups_control/arith.hpp"

namespace ups_control {

const char* to_string(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::Unknown: return "unknown";
    case Provenance::AdapterReported: return "adapter_reported";
    case Provenance::OperatorAsserted: return "operator_asserted";
    case Provenance::SyntheticAdapter: return "synthetic_adapter";
    case Provenance::ImportedAudit: return "imported_audit";
  }
  return "unknown";
}

Result<Provenance> parse_provenance(std::string_view text) {
  if (text == "unknown") {
    return Provenance::Unknown;
  }
  if (text == "adapter_reported") {
    return Provenance::AdapterReported;
  }
  if (text == "operator_asserted") {
    return Provenance::OperatorAsserted;
  }
  if (text == "synthetic_adapter") {
    return Provenance::SyntheticAdapter;
  }
  if (text == "imported_audit") {
    return Provenance::ImportedAudit;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown provenance '" + std::string(text) + "'");
}

bool is_synthetic(Provenance provenance) noexcept {
  return provenance == Provenance::SyntheticAdapter;
}

const char* to_string(EvidenceQuality quality) noexcept {
  switch (quality) {
    case EvidenceQuality::Unknown: return "unknown";
    case EvidenceQuality::Measured: return "measured";
    case EvidenceQuality::Estimated: return "estimated";
    case EvidenceQuality::Derived: return "derived";
  }
  return "unknown";
}

Result<EvidenceQuality> parse_evidence_quality(std::string_view text) {
  if (text == "unknown") {
    return EvidenceQuality::Unknown;
  }
  if (text == "measured") {
    return EvidenceQuality::Measured;
  }
  if (text == "estimated") {
    return EvidenceQuality::Estimated;
  }
  if (text == "derived") {
    return EvidenceQuality::Derived;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown evidence quality '" + std::string(text) + "'");
}

int evidence_quality_rank(EvidenceQuality quality) noexcept {
  switch (quality) {
    case EvidenceQuality::Unknown: return 0;
    case EvidenceQuality::Derived: return 1;
    case EvidenceQuality::Estimated: return 2;
    case EvidenceQuality::Measured: return 3;
  }
  return 0;
}

const char* to_string(ReserveState state) noexcept {
  switch (state) {
    case ReserveState::Unknown: return "unknown";
    case ReserveState::Known: return "known";
  }
  return "unknown";
}

const char* to_string(UnknownReason reason) noexcept {
  switch (reason) {
    case UnknownReason::None: return "none";
    case UnknownReason::NotReported: return "not_reported";
    case UnknownReason::SensorFault: return "sensor_fault";
    case UnknownReason::CommunicationLost: return "communication_lost";
    case UnknownReason::Unsupported: return "unsupported";
    case UnknownReason::Suppressed: return "suppressed";
    case UnknownReason::NotApplicable: return "not_applicable";
    case UnknownReason::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

Result<UnknownReason> parse_unknown_reason(std::string_view text) {
  if (text == "none") {
    return UnknownReason::None;
  }
  if (text == "not_reported") {
    return UnknownReason::NotReported;
  }
  if (text == "sensor_fault") {
    return UnknownReason::SensorFault;
  }
  if (text == "communication_lost") {
    return UnknownReason::CommunicationLost;
  }
  if (text == "unsupported") {
    return UnknownReason::Unsupported;
  }
  if (text == "suppressed") {
    return UnknownReason::Suppressed;
  }
  if (text == "not_applicable") {
    return UnknownReason::NotApplicable;
  }
  if (text == "indeterminate") {
    return UnknownReason::Indeterminate;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown unknown-reason '" + std::string(text) + "'");
}

const char* to_string(BatteryActivity activity) noexcept {
  switch (activity) {
    case BatteryActivity::Unknown: return "unknown";
    case BatteryActivity::Idle: return "idle";
    case BatteryActivity::Charging: return "charging";
    case BatteryActivity::Discharging: return "discharging";
    case BatteryActivity::Testing: return "testing";
  }
  return "unknown";
}

Result<BatteryActivity> parse_battery_activity(std::string_view text) {
  if (text == "unknown") {
    return BatteryActivity::Unknown;
  }
  if (text == "idle") {
    return BatteryActivity::Idle;
  }
  if (text == "charging") {
    return BatteryActivity::Charging;
  }
  if (text == "discharging") {
    return BatteryActivity::Discharging;
  }
  if (text == "testing") {
    return BatteryActivity::Testing;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown battery activity '" + std::string(text) + "'");
}

const char* to_string(FreshnessVerdict verdict) noexcept {
  switch (verdict) {
    case FreshnessVerdict::Fresh: return "fresh";
    case FreshnessVerdict::Stale: return "stale";
    case FreshnessVerdict::NeverObserved: return "never_observed";
    case FreshnessVerdict::FutureObservation: return "future_observation";
    case FreshnessVerdict::NotRevalidated: return "not_revalidated";
  }
  return "unknown";
}

FreshnessVerdict evaluate_freshness(Tick observed_at, Tick now, const FreshnessPolicy& policy) {
  if (!is_valid_instant(observed_at) || !is_valid_instant(now)) {
    return FreshnessVerdict::NeverObserved;
  }
  if (observed_at.value() > now.value()) {
    const Result<std::int64_t> ahead =
        checked_sub(observed_at.value(), now.value());
    if (!ahead.ok()) {
      return FreshnessVerdict::FutureObservation;
    }
    if (ahead.value() > policy.max_future_skew.value()) {
      return FreshnessVerdict::FutureObservation;
    }
    return FreshnessVerdict::Fresh;
  }
  const Result<std::int64_t> age = checked_sub(now.value(), observed_at.value());
  if (!age.ok()) {
    return FreshnessVerdict::Stale;
  }
  if (policy.max_age.value() < 0) {
    return FreshnessVerdict::Stale;
  }
  // The window is inclusive: an observation exactly max_age old is still fresh.
  return age.value() > policy.max_age.value() ? FreshnessVerdict::Stale : FreshnessVerdict::Fresh;
}

Result<TickSpan> observation_age(Tick observed_at, Tick now) {
  if (!is_valid_instant(observed_at) || !is_valid_instant(now)) {
    return Status::error(StatusCode::InvalidArgument,
                         "observation age needs two valid instants");
  }
  if (observed_at.value() > now.value()) {
    return Status::error(StatusCode::InvalidArgument,
                         "observation instant " + std::to_string(observed_at.value()) +
                             " is ahead of the requested instant " + std::to_string(now.value()));
  }
  const Result<std::int64_t> age = checked_sub(now.value(), observed_at.value());
  if (!age.ok()) {
    return age.status();
  }
  return TickSpan{age.value()};
}

ReserveObservation known_reserve(ReserveQuantity quantity, EvidenceQuality quality) {
  ReserveObservation observation;
  observation.state = ReserveState::Known;
  observation.quantity = quantity;
  observation.unknown_reason = UnknownReason::None;
  observation.quality = quality;
  return observation;
}

ReserveObservation unknown_reserve(UnknownReason reason) {
  ReserveObservation observation;
  observation.state = ReserveState::Unknown;
  observation.quantity = ReserveQuantity{};
  observation.unknown_reason = reason;
  observation.quality = EvidenceQuality::Unknown;
  return observation;
}

Status validate_reserve_observation(const ReserveObservation& observation,
                                    const ResourceLimits& limits) {
  if (observation.state == ReserveState::Known) {
    if (observation.unknown_reason != UnknownReason::None) {
      return Status::error(StatusCode::InvalidArgument,
                           "a known reserve observation must not carry an unknown reason");
    }
    if (observation.quality == EvidenceQuality::Unknown) {
      return Status::error(StatusCode::InvalidArgument,
                           "a known reserve observation must state its evidence quality");
    }
    return validate_reserve_quantity(observation.quantity, limits);
  }
  if (observation.unknown_reason == UnknownReason::None) {
    return Status::error(StatusCode::InvalidArgument,
                         "an unknown reserve observation must state why the value is unknown");
  }
  if (observation.quality != EvidenceQuality::Unknown) {
    return Status::error(StatusCode::InvalidArgument,
                         "an unknown reserve observation must not carry an evidence quality");
  }
  return Status::success();
}

Status validate_telemetry_report(const TelemetryReport& report, const ResourceLimits& limits) {
  Status status = validate_identifier(report.evidence.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "telemetry evidence id: " + status.message());
  }
  status = validate_identifier(report.ups.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "telemetry ups id: " + status.message());
  }
  status = validate_identifier(report.source.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "telemetry source id: " + status.message());
  }
  if (report.provenance == Provenance::Unknown) {
    return Status::error(StatusCode::InvalidArgument, "telemetry must state its provenance");
  }
  if (!is_valid_instant(report.observed_at)) {
    return Status::error(StatusCode::InvalidArgument,
                         "telemetry must carry a positive observation instant");
  }
  if (!is_valid_instant(report.received_at)) {
    return Status::error(StatusCode::InvalidArgument,
                         "telemetry must carry a positive receipt instant");
  }
  status = validate_reserve_observation(report.battery.reserve, limits);
  if (!status.ok()) {
    return Status::error(status.code(), "telemetry reserve: " + status.message());
  }
  status = validate_reserve_observation(report.battery.state_of_charge, limits);
  if (!status.ok()) {
    return Status::error(status.code(), "telemetry state of charge: " + status.message());
  }
  if (report.battery.state_of_charge.state == ReserveState::Known &&
      report.battery.state_of_charge.quantity.unit != ReserveUnit::BasisPoints) {
    return Status::error(StatusCode::InvalidArgument,
                         "a state of charge must be expressed in basis points, got " +
                             std::string(to_string(report.battery.state_of_charge.quantity.unit)));
  }
  return Status::success();
}

std::vector<std::string> find_contradictions(const TelemetryReport& report) {
  std::vector<std::string> found;
  const OperatingState operating = report.operating;
  const BatteryReading& battery = report.battery;

  if (report.fault_present == Bool3::True && is_protecting(operating) &&
      operating != OperatingState::OnlineBattery) {
    found.push_back(std::string("a fault is asserted while the operating state is ") +
                    to_string(operating));
  }
  if (operating == OperatingState::Faulted && report.fault_present == Bool3::False) {
    found.push_back("the operating state is faulted while no fault is asserted");
  }
  if (operating == OperatingState::OnlineBattery && battery.activity == BatteryActivity::Charging) {
    found.push_back("the unit is on battery while the battery reports charging");
  }
  if (operating == OperatingState::Recharge && battery.activity == BatteryActivity::Discharging) {
    found.push_back("the unit is recharging while the battery reports discharging");
  }
  if (operating == OperatingState::MaintenanceBypass && report.transfer.bypass_kind == BypassKind::None) {
    found.push_back("the unit is on maintenance bypass while no bypass path is reported");
  }
  if (operating == OperatingState::OnlineBattery && battery.reserve.state == ReserveState::Known &&
      battery.reserve.quantity.value == 0) {
    found.push_back("the unit is on battery while the reported reserve is exactly zero");
  }
  if (battery.reserve.state == ReserveState::Known &&
      battery.reserve.quality == EvidenceQuality::Unknown) {
    found.push_back("the reserve is reported as known with an unknown evidence quality");
  }
  if (battery.charge_inhibited == Bool3::True && battery.activity == BatteryActivity::Charging) {
    found.push_back("charging is reported while charging is reported inhibited");
  }
  if (battery.discharge_inhibited == Bool3::True && battery.activity == BatteryActivity::Discharging) {
    found.push_back("discharging is reported while discharging is reported inhibited");
  }
  if (report.capability.discharge_enabled == Bool3::False &&
      (operating == OperatingState::OnlineBattery || operating == OperatingState::BatteryTest)) {
    found.push_back("the unit is discharging while the discharge path is reported disabled");
  }
  if (report.capability.recharge_enabled == Bool3::False && operating == OperatingState::Recharge) {
    found.push_back("the unit is recharging while the recharge path is reported disabled");
  }
  return found;
}

}  // namespace ups_control
