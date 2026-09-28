#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/status.hpp"
#include "ups_control/units.hpp"

namespace ups_control {

/// Where a value came from. Provenance is recorded with the value and is never
/// discarded in favour of the value alone.
enum class Provenance : std::uint8_t {
  Unknown = 0,
  /// Read from device telemetry through the adapter boundary.
  AdapterReported = 1,
  /// Asserted by an operator through the documented administration path.
  OperatorAsserted = 2,
  /// Produced by the deterministic synthetic adapter. Evidence carrying this
  /// provenance is labelled SYNTHETIC everywhere it is reported and is never
  /// presented as hardware evidence.
  SyntheticAdapter = 3,
  /// Imported from an external audit record.
  ImportedAudit = 4,
};

const char* to_string(Provenance provenance) noexcept;
Result<Provenance> parse_provenance(std::string_view text);
/// True for provenance that is not hardware evidence.
bool is_synthetic(Provenance provenance) noexcept;

/// How the device or operator obtained a value.
enum class EvidenceQuality : std::uint8_t {
  Unknown = 0,
  /// Directly measured.
  Measured = 1,
  /// Estimated by the reporting device.
  Estimated = 2,
  /// Derived by the reporting device from other values.
  Derived = 3,
};

const char* to_string(EvidenceQuality quality) noexcept;
Result<EvidenceQuality> parse_evidence_quality(std::string_view text);
/// Ordering used by policy: Measured is the strongest, Unknown the weakest.
int evidence_quality_rank(EvidenceQuality quality) noexcept;

/// Whether a reserve quantity is known.
enum class ReserveState : std::uint8_t {
  /// The reporting device or operator did not establish a value.
  Unknown = 1,
  /// A value is present.
  Known = 2,
};

const char* to_string(ReserveState state) noexcept;

/// Why a reserve value is unknown. Required whenever the state is Unknown, and
/// never inferred.
enum class UnknownReason : std::uint8_t {
  None = 0,
  NotReported = 1,
  SensorFault = 2,
  CommunicationLost = 3,
  Unsupported = 4,
  Suppressed = 5,
  NotApplicable = 6,
  Indeterminate = 7,
};

const char* to_string(UnknownReason reason) noexcept;
Result<UnknownReason> parse_unknown_reason(std::string_view text);

/// A reserve observation: either a known quantity with a stated quality, or an
/// explicit unknown with a stated reason. The two are never conflated, and an
/// unknown is never zero.
struct ReserveObservation {
  ReserveState state = ReserveState::Unknown;
  ReserveQuantity quantity{};
  UnknownReason unknown_reason = UnknownReason::NotReported;
  EvidenceQuality quality = EvidenceQuality::Unknown;

  friend bool operator==(const ReserveObservation&, const ReserveObservation&) = default;
};

Status validate_reserve_observation(const ReserveObservation& observation,
                                    const ResourceLimits& limits);

/// Builds a known reserve observation with a stated evidence quality. This is the
/// documented way to construct one: the unknown reason is cleared, because a known
/// value has no reason to give.
ReserveObservation known_reserve(ReserveQuantity quantity, EvidenceQuality quality);

/// Builds an explicit unknown reserve observation with a stated reason. The
/// quantity is never read for an unknown observation and is never a zero stand-in
/// for a missing value.
ReserveObservation unknown_reserve(UnknownReason reason);

/// Reported battery activity. Vendor-neutral; this runtime does not infer it.
enum class BatteryActivity : std::uint8_t {
  Unknown = 0,
  Idle = 1,
  Charging = 2,
  Discharging = 3,
  Testing = 4,
};

const char* to_string(BatteryActivity activity) noexcept;
Result<BatteryActivity> parse_battery_activity(std::string_view text);

/// The battery portion of one telemetry report.
struct BatteryReading {
  ReserveObservation reserve;
  ReserveObservation state_of_charge;
  BatteryActivity activity = BatteryActivity::Unknown;
  Bool3 charge_inhibited = Bool3::Unknown;
  Bool3 discharge_inhibited = Bool3::Unknown;

  friend bool operator==(const BatteryReading&, const BatteryReading&) = default;
};

/// Device-reported capability state for the recharge and discharge paths. These
/// are observations of what the device currently allows, not authority: this
/// runtime never converts an observed capability into permission to act.
struct CapabilityObservation {
  Bool3 recharge_enabled = Bool3::Unknown;
  Bool3 discharge_enabled = Bool3::Unknown;

  friend bool operator==(const CapabilityObservation&, const CapabilityObservation&) = default;
};

/// How old an observation is still allowed to be.
struct FreshnessPolicy {
  /// Largest accepted age. A negative value is invalid.
  TickSpan max_age{0};
  /// Largest tolerated amount by which an observation may be ahead of the
  /// requested instant before it is treated as a future observation.
  TickSpan max_future_skew{0};

  friend bool operator==(const FreshnessPolicy&, const FreshnessPolicy&) = default;
};

/// The freshness of one observation at a requested instant.
enum class FreshnessVerdict : std::uint8_t {
  /// Within the configured window.
  Fresh = 1,
  /// Older than the configured window.
  Stale = 2,
  /// No observation instant was ever recorded.
  NeverObserved = 3,
  /// The observation claims an instant beyond the tolerated future skew.
  FutureObservation = 4,
  /// The store was recovered and has not been revalidated at the requested
  /// instant, so no freshness claim may be made yet.
  NotRevalidated = 5,
};

const char* to_string(FreshnessVerdict verdict) noexcept;

/// Evaluates freshness of one observation instant against a requested instant.
///
/// The origin instant is never observed, a negative age is impossible, an
/// observation ahead of \c now beyond \c policy.max_future_skew is a future
/// observation and never fresh, and an age above \c policy.max_age is stale.
FreshnessVerdict evaluate_freshness(Tick observed_at, Tick now, const FreshnessPolicy& policy);

/// The exact age of an observation, or an error when the observation is not in
/// the past relative to \c now.
Result<TickSpan> observation_age(Tick observed_at, Tick now);

/// One complete telemetry report for one UPS as reported by one source.
struct TelemetryReport {
  EvidenceId evidence;
  UpsId ups;
  HardwareGeneration hardware;
  SourceId source;
  Provenance provenance = Provenance::Unknown;
  SourceRevision revision;
  Tick observed_at;
  Tick received_at;

  OperatingState operating = OperatingState::Unknown;
  TransferStatus transfer;
  CapabilityObservation capability;
  BatteryReading battery;

  /// True when the device asserts a fault is present. Distinct from the
  /// operating state, which may or may not have moved to \c Faulted yet.
  Bool3 fault_present = Bool3::Unknown;

  friend bool operator==(const TelemetryReport&, const TelemetryReport&) = default;
};

/// The durable record of the last telemetry accepted for one UPS.
///
/// The observation instant is retained verbatim so that a later reader can
/// compute, and explain, exactly why the evidence is or is not fresh. Recovery
/// never rewrites it.
struct ObservationRecord {
  EvidenceId evidence;
  SourceId source;
  Provenance provenance = Provenance::Unknown;
  SourceRevision revision;
  Tick observed_at;
  Tick received_at;

  OperatingState operating = OperatingState::Unknown;
  TransferStatus transfer;
  CapabilityObservation capability;
  BatteryReading battery;
  Bool3 fault_present = Bool3::Unknown;

  /// True when the observation is internally contradictory (for example a fault
  /// asserted while the state claims normal operation). A contradictory report is
  /// recorded but never used to satisfy a precondition.
  bool contradictory = false;
  /// Human-readable, deterministic account of the contradictions found.
  std::string contradiction_detail;

  friend bool operator==(const ObservationRecord&, const ObservationRecord&) = default;
};

/// Detects internal contradictions in a telemetry report. Deterministic and
/// order-independent.
std::vector<std::string> find_contradictions(const TelemetryReport& report);

Status validate_telemetry_report(const TelemetryReport& report, const ResourceLimits& limits);

}  // namespace ups_control
