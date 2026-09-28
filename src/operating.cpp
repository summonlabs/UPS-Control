#include "ups_control/operating.hpp"

#include <algorithm>

namespace ups_control {

const char* to_string(Bool3 value) noexcept {
  switch (value) {
    case Bool3::Unknown: return "unknown";
    case Bool3::True: return "true";
    case Bool3::False: return "false";
  }
  return "unknown";
}

Result<Bool3> parse_bool3(std::string_view text) {
  if (text == "unknown") {
    return Bool3::Unknown;
  }
  if (text == "true") {
    return Bool3::True;
  }
  if (text == "false") {
    return Bool3::False;
  }
  return Status::error(StatusCode::InvalidArgument,
                       "unknown tri-state '" + std::string(text) +
                           "'; accepted values are unknown, true, false");
}

const char* to_string(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Commissioning: return "commissioning";
    case LifecycleState::Standby: return "standby";
    case LifecycleState::InService: return "in_service";
    case LifecycleState::Maintenance: return "maintenance";
    case LifecycleState::Degraded: return "degraded";
    case LifecycleState::Faulted: return "faulted";
    case LifecycleState::Isolated: return "isolated";
    case LifecycleState::Decommissioned: return "decommissioned";
  }
  return "unknown";
}

Result<LifecycleState> parse_lifecycle_state(std::string_view text) {
  if (text == "commissioning") {
    return LifecycleState::Commissioning;
  }
  if (text == "standby") {
    return LifecycleState::Standby;
  }
  if (text == "in_service") {
    return LifecycleState::InService;
  }
  if (text == "maintenance") {
    return LifecycleState::Maintenance;
  }
  if (text == "degraded") {
    return LifecycleState::Degraded;
  }
  if (text == "faulted") {
    return LifecycleState::Faulted;
  }
  if (text == "isolated") {
    return LifecycleState::Isolated;
  }
  if (text == "decommissioned") {
    return LifecycleState::Decommissioned;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown lifecycle state '" + std::string(text) + "'");
}

bool is_terminal(LifecycleState state) noexcept {
  return state == LifecycleState::Decommissioned;
}

const char* to_string(OperatingState state) noexcept {
  switch (state) {
    case OperatingState::Unknown: return "unknown";
    case OperatingState::Offline: return "offline";
    case OperatingState::Standby: return "standby";
    case OperatingState::OnlineNormal: return "online_normal";
    case OperatingState::OnlineBattery: return "online_battery";
    case OperatingState::StaticBypass: return "static_bypass";
    case OperatingState::MaintenanceBypass: return "maintenance_bypass";
    case OperatingState::Recharge: return "recharge";
    case OperatingState::SelfTest: return "self_test";
    case OperatingState::BatteryTest: return "battery_test";
    case OperatingState::Faulted: return "faulted";
    case OperatingState::Isolated: return "isolated";
    case OperatingState::Retired: return "retired";
  }
  return "unknown";
}

Result<OperatingState> parse_operating_state(std::string_view text) {
  if (text == "unknown") {
    return OperatingState::Unknown;
  }
  if (text == "offline") {
    return OperatingState::Offline;
  }
  if (text == "standby") {
    return OperatingState::Standby;
  }
  if (text == "online_normal") {
    return OperatingState::OnlineNormal;
  }
  if (text == "online_battery") {
    return OperatingState::OnlineBattery;
  }
  if (text == "static_bypass") {
    return OperatingState::StaticBypass;
  }
  if (text == "maintenance_bypass") {
    return OperatingState::MaintenanceBypass;
  }
  if (text == "recharge") {
    return OperatingState::Recharge;
  }
  if (text == "self_test") {
    return OperatingState::SelfTest;
  }
  if (text == "battery_test") {
    return OperatingState::BatteryTest;
  }
  if (text == "faulted") {
    return OperatingState::Faulted;
  }
  if (text == "isolated") {
    return OperatingState::Isolated;
  }
  if (text == "retired") {
    return OperatingState::Retired;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown operating state '" + std::string(text) + "'");
}

bool is_terminal(OperatingState state) noexcept {
  return state == OperatingState::Retired;
}

bool is_protecting(OperatingState state) noexcept {
  switch (state) {
    case OperatingState::OnlineNormal:
    case OperatingState::OnlineBattery:
    case OperatingState::Recharge:
    case OperatingState::SelfTest:
    case OperatingState::BatteryTest:
      return true;
    default:
      return false;
  }
}

bool is_bypass(OperatingState state) noexcept {
  return state == OperatingState::StaticBypass || state == OperatingState::MaintenanceBypass;
}

bool is_testing(OperatingState state) noexcept {
  return state == OperatingState::SelfTest || state == OperatingState::BatteryTest;
}

const char* to_string(StateBasis basis) noexcept {
  switch (basis) {
    case StateBasis::Recovered: return "recovered";
    case StateBasis::Observed: return "observed";
    case StateBasis::CommandedUnverified: return "commanded_unverified";
    case StateBasis::Verified: return "verified";
  }
  return "unknown";
}

namespace {

/// The documented transition graph, as an explicit successor set per source
/// state. Every pair absent from this table is an illegal edge.
bool has_edge(OperatingState from, OperatingState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case OperatingState::Unknown:
      // Closed source state: only an adopted observation may replace Unknown, and
      // adoption is not a control transition.
      return false;
    case OperatingState::Offline:
      return to == OperatingState::Standby || to == OperatingState::Isolated ||
             to == OperatingState::Retired;
    case OperatingState::Standby:
      return to == OperatingState::OnlineNormal || to == OperatingState::Offline ||
             to == OperatingState::Isolated || to == OperatingState::Retired ||
             to == OperatingState::Faulted;
    case OperatingState::OnlineNormal:
      return to == OperatingState::OnlineBattery || to == OperatingState::StaticBypass ||
             to == OperatingState::MaintenanceBypass || to == OperatingState::SelfTest ||
             to == OperatingState::BatteryTest || to == OperatingState::Recharge ||
             to == OperatingState::Standby || to == OperatingState::Faulted ||
             to == OperatingState::Isolated;
    case OperatingState::OnlineBattery:
      return to == OperatingState::OnlineNormal || to == OperatingState::Recharge ||
             to == OperatingState::StaticBypass || to == OperatingState::Standby ||
             to == OperatingState::Faulted || to == OperatingState::Isolated;
    case OperatingState::StaticBypass:
      return to == OperatingState::OnlineNormal || to == OperatingState::MaintenanceBypass ||
             to == OperatingState::Faulted || to == OperatingState::Standby ||
             to == OperatingState::Isolated;
    case OperatingState::MaintenanceBypass:
      return to == OperatingState::StaticBypass || to == OperatingState::OnlineNormal ||
             to == OperatingState::Isolated || to == OperatingState::Retired ||
             to == OperatingState::Standby;
    case OperatingState::Recharge:
      return to == OperatingState::OnlineNormal || to == OperatingState::OnlineBattery ||
             to == OperatingState::Faulted || to == OperatingState::Standby ||
             to == OperatingState::Isolated;
    case OperatingState::SelfTest:
      return to == OperatingState::OnlineNormal || to == OperatingState::OnlineBattery ||
             to == OperatingState::Faulted || to == OperatingState::StaticBypass;
    case OperatingState::BatteryTest:
      return to == OperatingState::OnlineNormal || to == OperatingState::Recharge ||
             to == OperatingState::OnlineBattery || to == OperatingState::Faulted ||
             to == OperatingState::StaticBypass;
    case OperatingState::Faulted:
      // Control transitions out of Faulted are restricted to de-energizing or
      // locking out the unit. Returning to a healthy state is an observation, not
      // a command.
      return to == OperatingState::Isolated || to == OperatingState::Offline ||
             to == OperatingState::Retired;
    case OperatingState::Isolated:
      return to == OperatingState::Standby || to == OperatingState::Offline ||
             to == OperatingState::Retired;
    case OperatingState::Retired:
      return false;
  }
  return false;
}

}  // namespace

bool is_legal_operating_transition(OperatingState from, OperatingState to) noexcept {
  return has_edge(from, to);
}

bool is_legal_lifecycle_transition(LifecycleState from, LifecycleState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case LifecycleState::Commissioning:
      return to == LifecycleState::Standby || to == LifecycleState::InService ||
             to == LifecycleState::Decommissioned;
    case LifecycleState::Standby:
      return to == LifecycleState::InService || to == LifecycleState::Maintenance ||
             to == LifecycleState::Decommissioned || to == LifecycleState::Faulted;
    case LifecycleState::InService:
      return to == LifecycleState::Maintenance || to == LifecycleState::Degraded ||
             to == LifecycleState::Faulted || to == LifecycleState::Standby ||
             to == LifecycleState::Decommissioned || to == LifecycleState::Isolated;
    case LifecycleState::Maintenance:
      return to == LifecycleState::InService || to == LifecycleState::Standby ||
             to == LifecycleState::Isolated || to == LifecycleState::Faulted ||
             to == LifecycleState::Decommissioned;
    case LifecycleState::Degraded:
      return to == LifecycleState::InService || to == LifecycleState::Maintenance ||
             to == LifecycleState::Faulted || to == LifecycleState::Isolated ||
             to == LifecycleState::Decommissioned;
    case LifecycleState::Faulted:
      return to == LifecycleState::Maintenance || to == LifecycleState::Isolated ||
             to == LifecycleState::Degraded || to == LifecycleState::InService ||
             to == LifecycleState::Decommissioned;
    case LifecycleState::Isolated:
      return to == LifecycleState::Maintenance || to == LifecycleState::Standby ||
             to == LifecycleState::Decommissioned;
    case LifecycleState::Decommissioned:
      return false;
  }
  return false;
}

bool lifecycle_permits_control(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::InService:
    case LifecycleState::Degraded:
      return true;
    case LifecycleState::Standby:
      // Standby assets may be brought online but are not carrying protected load.
      return true;
    case LifecycleState::Commissioning:
    case LifecycleState::Maintenance:
    case LifecycleState::Faulted:
    case LifecycleState::Isolated:
    case LifecycleState::Decommissioned:
      return false;
  }
  return false;
}

bool is_consistent(LifecycleState lifecycle, OperatingState operating) noexcept {
  switch (lifecycle) {
    case LifecycleState::Decommissioned:
      return operating == OperatingState::Retired || operating == OperatingState::Offline;
    case LifecycleState::Isolated:
      return operating == OperatingState::Isolated || operating == OperatingState::Offline ||
             operating == OperatingState::Retired;
    case LifecycleState::Commissioning:
      return operating == OperatingState::Unknown || operating == OperatingState::Offline ||
             operating == OperatingState::Standby || operating == OperatingState::Isolated;
    case LifecycleState::Maintenance:
      // A unit under maintenance may still be carrying the load on a bypass path
      // or may be isolated.
      return operating != OperatingState::Retired;
    case LifecycleState::Standby:
      return operating != OperatingState::Retired;
    case LifecycleState::InService:
    case LifecycleState::Degraded:
      return operating != OperatingState::Retired;
    case LifecycleState::Faulted:
      return operating != OperatingState::Retired;
  }
  return true;
}

std::vector<OperatingState> legal_successors(OperatingState from) {
  std::vector<OperatingState> successors;
  for (int index = 0; index <= static_cast<int>(OperatingState::Retired); ++index) {
    const auto candidate = static_cast<OperatingState>(index);
    if (has_edge(from, candidate)) {
      successors.push_back(candidate);
    }
  }
  return successors;
}

const char* to_string(BypassKind kind) noexcept {
  switch (kind) {
    case BypassKind::Unknown: return "unknown";
    case BypassKind::None: return "none";
    case BypassKind::Static: return "static";
    case BypassKind::Maintenance: return "maintenance";
  }
  return "unknown";
}

bool static_bypass_ready(const TransferStatus& status) noexcept {
  // All five reported capabilities are required, which is exactly the set
  // \c EnterStaticBypass requires of the same status. A transfer window in which
  // the input fails is carried by the battery, so a battery that is not reported
  // ready is not assumed ready: readiness and evaluation must agree, and they agree
  // in the fail-closed direction.
  return certainly_true(status.bypass_available) && certainly_true(status.bypass_qualified) &&
         certainly_true(status.output_synchronized) && certainly_true(status.transfer_ready) &&
         certainly_true(status.battery_ready);
}

bool maintenance_bypass_ready(const TransferStatus& status) noexcept {
  return certainly_true(status.bypass_available) && certainly_true(status.bypass_qualified);
}

}  // namespace ups_control
