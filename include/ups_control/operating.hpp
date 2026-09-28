#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ups_control/ids.hpp"
#include "ups_control/status.hpp"

namespace ups_control {

/// A three-valued observation. Missing evidence is never collapsed into false.
enum class Bool3 : std::uint8_t {
  Unknown = 0,
  True = 1,
  False = 2,
};

const char* to_string(Bool3 value) noexcept;
Result<Bool3> parse_bool3(std::string_view text);

/// True only when the observation is explicitly \c True.
inline bool certainly_true(Bool3 value) noexcept { return value == Bool3::True; }

/// Where one UPS sits in its service life. This is administrative and durable.
enum class LifecycleState : std::uint8_t {
  Commissioning = 1,
  Standby = 2,
  InService = 3,
  Maintenance = 4,
  Degraded = 5,
  Faulted = 6,
  Isolated = 7,
  Decommissioned = 8,
};

const char* to_string(LifecycleState state) noexcept;
Result<LifecycleState> parse_lifecycle_state(std::string_view text);
bool is_terminal(LifecycleState state) noexcept;

/// What the electrical path of one UPS is doing. This is physical and observable.
///
/// \c MaintenanceBypass means the protected load is carried by the maintenance
/// bypass path while the UPS output is isolated; it is therefore a state in which
/// the UPS itself is no longer protecting the load.
enum class OperatingState : std::uint8_t {
  /// No validated observation establishes the state. Fail closed: no control
  /// transition is a legal edge out of \c Unknown.
  Unknown = 0,
  /// No output, and the unit is not ready to transfer.
  Offline = 1,
  /// Input available, output not energized, ready to transfer.
  Standby = 2,
  /// Inverter carrying the protected load from the normal input.
  OnlineNormal = 3,
  /// Inverter carrying the protected load from battery.
  OnlineBattery = 4,
  /// Protected load carried by the static bypass path.
  StaticBypass = 5,
  /// Protected load carried by the maintenance bypass path; the UPS output is
  /// isolated, so the UPS is not protecting the load.
  MaintenanceBypass = 6,
  /// Online and restoring battery charge after a discharge.
  Recharge = 7,
  /// A diagnostic test is running with the output still online on the normal
  /// input.
  SelfTest = 8,
  /// A test is running that discharges the battery into the protected load.
  BatteryTest = 9,
  /// A fault is present; output quality is not assured.
  Faulted = 10,
  /// Locked out for service. No output.
  Isolated = 11,
  /// Decommissioned. Terminal.
  Retired = 12,
};

const char* to_string(OperatingState state) noexcept;
Result<OperatingState> parse_operating_state(std::string_view text);
bool is_terminal(OperatingState state) noexcept;

/// True when the operating state means the UPS is actively protecting its
/// protected load from its own energy store or conditioned path.
bool is_protecting(OperatingState state) noexcept;

/// True when the operating state means the protected load is on a raw bypass
/// path.
bool is_bypass(OperatingState state) noexcept;

/// True when a test is running.
bool is_testing(OperatingState state) noexcept;

/// How the current operating state was established.
///
/// Acknowledgement is not effect: an acknowledged command never establishes an
/// operating state by itself. On reopen every record is downgraded to
/// \c Recovered, because a persistence recovery must never make a prior
/// freshness claim current again.
enum class StateBasis : std::uint8_t {
  /// Read from the durable store and not confirmed by anything after the reopen.
  Recovered = 1,
  /// Adopted from a validated telemetry observation.
  Observed = 2,
  /// A control attempt was issued against this state and its effect has not been
  /// verified.
  CommandedUnverified = 3,
  /// Established by a verified control effect.
  Verified = 4,
};

const char* to_string(StateBasis basis) noexcept;

/// The documented transition graph.
///
/// The legal control edges are exactly those listed below. Every undefined pair
/// is illegal and is refused with \c RefusalCode::IllegalOperatingTransition.
/// \c Unknown is a closed source state: no control transition leaves it; only an
/// adopted telemetry observation may replace it.
bool is_legal_operating_transition(OperatingState from, OperatingState to) noexcept;

/// The legal lifecycle edges. \c Decommissioned is terminal.
bool is_legal_lifecycle_transition(LifecycleState from, LifecycleState to) noexcept;

/// True when a load-affecting control transition may be planned while the asset
/// is in this lifecycle state.
bool lifecycle_permits_control(LifecycleState state) noexcept;

/// True when the pair (lifecycle, operating) is internally consistent. An
/// inconsistent pair reported by telemetry is refused as contradictory rather
/// than adopted.
bool is_consistent(LifecycleState lifecycle, OperatingState operating) noexcept;

/// Every legal successor of \c from, in a stable documented order. Bounded by
/// \c OperatingState::Retired.
std::vector<OperatingState> legal_successors(OperatingState from);

// ---------------------------------------------------------------------------
// Transfer and bypass status
// ---------------------------------------------------------------------------

enum class BypassKind : std::uint8_t {
  Unknown = 0,
  /// No bypass path is present or engaged.
  None = 1,
  /// The internal static bypass path.
  Static = 2,
  /// The external maintenance bypass path.
  Maintenance = 3,
};

const char* to_string(BypassKind kind) noexcept;

/// Device-reported transfer capability. Every member is tri-state: a device that
/// does not report a capability yields \c Unknown, and \c Unknown never
/// satisfies a readiness precondition.
struct TransferStatus {
  BypassKind bypass_kind = BypassKind::Unknown;
  /// A bypass source is present and energized.
  Bool3 bypass_available = Bool3::Unknown;
  /// The bypass source is qualified to carry the protected load (within the
  /// configured voltage and frequency tolerance, and of an acceptable class).
  Bool3 bypass_qualified = Bool3::Unknown;
  /// The UPS output is synchronized to the bypass source.
  Bool3 output_synchronized = Bool3::Unknown;
  /// The device asserts that a transfer may proceed now.
  Bool3 transfer_ready = Bool3::Unknown;
  /// The battery can accept the load now.
  Bool3 battery_ready = Bool3::Unknown;

  friend bool operator==(const TransferStatus&, const TransferStatus&) = default;
};

/// True only when every precondition for a static bypass transfer is explicitly
/// established: bypass availability and qualification, output synchronisation,
/// device transfer readiness, and battery readiness. The set is exactly the set
/// \c EnterStaticBypass requires of the same status, so readiness and evaluation
/// never disagree.
bool static_bypass_ready(const TransferStatus& status) noexcept;

/// True only when every precondition for a maintenance bypass transfer is
/// explicitly established.
bool maintenance_bypass_ready(const TransferStatus& status) noexcept;

}  // namespace ups_control
