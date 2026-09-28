// Transition-graph and tri-state readiness proofs for the UPS Control library.
//
// The operating and lifecycle successor relations are compared against reference
// tables written out by hand here (a second, separate statement of the graph), so
// a missing or extra edge fails with the exact pair named. The documented
// properties that the tables are derived from are:
//
//  * docs/authority-and-transitions.md: Unknown is a closed source state, Retired
//    is terminal, control out of Faulted reaches only Isolated, Offline and
//    Retired, Decommissioned is terminal, and lifecycle_permits_control is true
//    only for Standby, InService and Degraded.
//  * include/ups_control/operating.hpp: the per-state descriptions (for example
//    MaintenanceBypass carries the load on the maintenance path while the UPS
//    output is isolated) plus the documented numeric values of each enumerator.
//  * docs/authority-and-transitions.md, "State": the complete successor set of
//    every lifecycle and operating state, and the readiness/evaluation capability
//    table.
//
// The tables below are the hand-written second statement of those documented
// tables: every pair, in both directions, is compared against them.

#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/status.hpp"

using namespace ups_control;

namespace {

constexpr int kFirstOperatingIndex = static_cast<int>(OperatingState::Unknown);
constexpr int kLastOperatingIndex = static_cast<int>(OperatingState::Retired);
constexpr std::size_t kOperatingCount = 13;

constexpr int kFirstLifecycleIndex = static_cast<int>(LifecycleState::Commissioning);
constexpr int kLastLifecycleIndex = static_cast<int>(LifecycleState::Decommissioned);
constexpr std::size_t kLifecycleCount = 8;

std::string state_name(OperatingState state) { return std::string(to_string(state)); }

std::string state_name(LifecycleState state) { return std::string(to_string(state)); }

/// The hand-written operating successor relation, a set per source state. The
/// predicate comparison is order-insensitive; the legal_successors comparison
/// sorts this set before comparing, so it also proves the returned order.
std::array<std::vector<OperatingState>, kOperatingCount> reference_operating_successors() {
  std::array<std::vector<OperatingState>, kOperatingCount> table;
  table[static_cast<std::size_t>(OperatingState::Unknown)] = {};
  table[static_cast<std::size_t>(OperatingState::Offline)] = {
      OperatingState::Standby, OperatingState::Isolated, OperatingState::Retired};
  table[static_cast<std::size_t>(OperatingState::Standby)] = {
      OperatingState::OnlineNormal, OperatingState::Offline, OperatingState::Isolated,
      OperatingState::Retired, OperatingState::Faulted};
  table[static_cast<std::size_t>(OperatingState::OnlineNormal)] = {
      OperatingState::OnlineBattery, OperatingState::StaticBypass,
      OperatingState::MaintenanceBypass, OperatingState::SelfTest, OperatingState::BatteryTest,
      OperatingState::Recharge, OperatingState::Standby, OperatingState::Faulted,
      OperatingState::Isolated};
  table[static_cast<std::size_t>(OperatingState::OnlineBattery)] = {
      OperatingState::OnlineNormal, OperatingState::Recharge, OperatingState::StaticBypass,
      OperatingState::Standby, OperatingState::Faulted, OperatingState::Isolated};
  table[static_cast<std::size_t>(OperatingState::StaticBypass)] = {
      OperatingState::OnlineNormal, OperatingState::MaintenanceBypass, OperatingState::Faulted,
      OperatingState::Standby, OperatingState::Isolated};
  table[static_cast<std::size_t>(OperatingState::MaintenanceBypass)] = {
      OperatingState::StaticBypass, OperatingState::OnlineNormal, OperatingState::Isolated,
      OperatingState::Retired, OperatingState::Standby};
  table[static_cast<std::size_t>(OperatingState::Recharge)] = {
      OperatingState::OnlineNormal, OperatingState::OnlineBattery, OperatingState::Faulted,
      OperatingState::Standby, OperatingState::Isolated};
  table[static_cast<std::size_t>(OperatingState::SelfTest)] = {
      OperatingState::OnlineNormal, OperatingState::OnlineBattery, OperatingState::Faulted,
      OperatingState::StaticBypass};
  table[static_cast<std::size_t>(OperatingState::BatteryTest)] = {
      OperatingState::OnlineNormal, OperatingState::Recharge, OperatingState::OnlineBattery,
      OperatingState::Faulted, OperatingState::StaticBypass};
  table[static_cast<std::size_t>(OperatingState::Faulted)] = {
      OperatingState::Isolated, OperatingState::Offline, OperatingState::Retired};
  table[static_cast<std::size_t>(OperatingState::Isolated)] = {
      OperatingState::Standby, OperatingState::Offline, OperatingState::Retired};
  table[static_cast<std::size_t>(OperatingState::Retired)] = {};
  return table;
}

bool reference_operating_edge(OperatingState from, OperatingState to) {
  const auto table = reference_operating_successors();
  const std::vector<OperatingState>& successors = table[static_cast<std::size_t>(from)];
  return std::find(successors.begin(), successors.end(), to) != successors.end();
}

/// The hand-written lifecycle successor relation, keyed by the documented numeric
/// value of each state.
std::array<std::vector<LifecycleState>, kLifecycleCount + 1> reference_lifecycle_successors() {
  std::array<std::vector<LifecycleState>, kLifecycleCount + 1> table;
  table[static_cast<std::size_t>(LifecycleState::Commissioning)] = {
      LifecycleState::Standby, LifecycleState::InService, LifecycleState::Decommissioned};
  table[static_cast<std::size_t>(LifecycleState::Standby)] = {
      LifecycleState::InService, LifecycleState::Maintenance, LifecycleState::Decommissioned,
      LifecycleState::Faulted};
  table[static_cast<std::size_t>(LifecycleState::InService)] = {
      LifecycleState::Maintenance, LifecycleState::Degraded, LifecycleState::Faulted,
      LifecycleState::Standby, LifecycleState::Decommissioned, LifecycleState::Isolated};
  table[static_cast<std::size_t>(LifecycleState::Maintenance)] = {
      LifecycleState::InService, LifecycleState::Standby, LifecycleState::Isolated,
      LifecycleState::Faulted, LifecycleState::Decommissioned};
  table[static_cast<std::size_t>(LifecycleState::Degraded)] = {
      LifecycleState::InService, LifecycleState::Maintenance, LifecycleState::Faulted,
      LifecycleState::Isolated, LifecycleState::Decommissioned};
  table[static_cast<std::size_t>(LifecycleState::Faulted)] = {
      LifecycleState::Maintenance, LifecycleState::Isolated, LifecycleState::Degraded,
      LifecycleState::InService, LifecycleState::Decommissioned};
  table[static_cast<std::size_t>(LifecycleState::Isolated)] = {
      LifecycleState::Maintenance, LifecycleState::Standby, LifecycleState::Decommissioned};
  table[static_cast<std::size_t>(LifecycleState::Decommissioned)] = {};
  return table;
}

bool reference_lifecycle_edge(LifecycleState from, LifecycleState to) {
  const auto table = reference_lifecycle_successors();
  const std::vector<LifecycleState>& successors = table[static_cast<std::size_t>(from)];
  return std::find(successors.begin(), successors.end(), to) != successors.end();
}

/// The hand-written (lifecycle, operating) consistency table. A decommissioned
/// unit is out of service, an isolated unit is locked out for service, and a unit
/// still being commissioned is not yet carrying the load; every other
/// administrative state may carry the load in any operating state except Retired,
/// because a retired device cannot be administratively in service.
bool reference_consistent(LifecycleState lifecycle, OperatingState operating) {
  switch (lifecycle) {
    case LifecycleState::Decommissioned:
      return operating == OperatingState::Retired || operating == OperatingState::Offline;
    case LifecycleState::Isolated:
      return operating == OperatingState::Isolated || operating == OperatingState::Offline ||
             operating == OperatingState::Retired;
    case LifecycleState::Commissioning:
      return operating == OperatingState::Unknown || operating == OperatingState::Offline ||
             operating == OperatingState::Standby || operating == OperatingState::Isolated;
    case LifecycleState::Standby:
    case LifecycleState::InService:
    case LifecycleState::Maintenance:
    case LifecycleState::Degraded:
    case LifecycleState::Faulted:
      return operating != OperatingState::Retired;
  }
  return false;
}

/// The documented lifecycle control permission: only Standby, InService and
/// Degraded permit load-affecting control.
bool reference_lifecycle_permits_control(LifecycleState state) {
  return state == LifecycleState::Standby || state == LifecycleState::InService ||
         state == LifecycleState::Degraded;
}

bool contains(const std::vector<OperatingState>& states, OperatingState state) {
  return std::find(states.begin(), states.end(), state) != states.end();
}

TransferStatus transfer_with(Bool3 available, Bool3 qualified, Bool3 synchronized,
                             Bool3 transfer_ready, Bool3 battery_ready) {
  TransferStatus status;
  status.bypass_kind = BypassKind::Static;
  status.bypass_available = available;
  status.bypass_qualified = qualified;
  status.output_synchronized = synchronized;
  status.transfer_ready = transfer_ready;
  status.battery_ready = battery_ready;
  return status;
}

std::string tri_state_text(Bool3 value) { return std::string(to_string(value)); }

std::string transfer_text(Bool3 available, Bool3 qualified, Bool3 synchronized, Bool3 ready,
                          Bool3 battery_ready) {
  return "available=" + tri_state_text(available) + " qualified=" + tri_state_text(qualified) +
         " synchronized=" + tri_state_text(synchronized) + " transfer_ready=" +
         tri_state_text(ready) + " battery_ready=" + tri_state_text(battery_ready);
}

}  // namespace

UC_TEST(graph, operating_state_numeric_values_and_tokens_are_the_documented_ones) {
  // The header assigns these values explicitly; they are part of the contract.
  UC_CHECK_EQ(static_cast<int>(OperatingState::Unknown), 0);
  UC_CHECK_EQ(static_cast<int>(OperatingState::Offline), 1);
  UC_CHECK_EQ(static_cast<int>(OperatingState::Standby), 2);
  UC_CHECK_EQ(static_cast<int>(OperatingState::OnlineNormal), 3);
  UC_CHECK_EQ(static_cast<int>(OperatingState::OnlineBattery), 4);
  UC_CHECK_EQ(static_cast<int>(OperatingState::StaticBypass), 5);
  UC_CHECK_EQ(static_cast<int>(OperatingState::MaintenanceBypass), 6);
  UC_CHECK_EQ(static_cast<int>(OperatingState::Recharge), 7);
  UC_CHECK_EQ(static_cast<int>(OperatingState::SelfTest), 8);
  UC_CHECK_EQ(static_cast<int>(OperatingState::BatteryTest), 9);
  UC_CHECK_EQ(static_cast<int>(OperatingState::Faulted), 10);
  UC_CHECK_EQ(static_cast<int>(OperatingState::Isolated), 11);
  UC_CHECK_EQ(static_cast<int>(OperatingState::Retired), 12);

  // The stable tokens, in declaration order. README.md documents all but
  // offline and standby explicitly; those two are pinned by the parser contract
  // (parse must round-trip every token) and by this table.
  const std::vector<std::string> documented_tokens = {
      "unknown",  "offline",     "standby",   "online_normal", "online_battery",
      "static_bypass", "maintenance_bypass", "recharge", "self_test", "battery_test",
      "faulted",  "isolated",    "retired"};
  UC_CHECK_EQ(documented_tokens.size(), kOperatingCount);
  std::vector<std::string> tokens;
  for (int index = kFirstOperatingIndex; index <= kLastOperatingIndex; ++index) {
    const OperatingState state = static_cast<OperatingState>(index);
    const std::string token = to_string(state);
    UC_CHECK_MSG(!token.empty(), "operating state " + std::to_string(index) +
                                     " has no stable token");
    UC_CHECK_EQ(token, documented_tokens[static_cast<std::size_t>(index)]);
    UC_CHECK_MSG(std::find(tokens.begin(), tokens.end(), token) == tokens.end(),
                 "two operating states share the token '" + token + "'");
    tokens.push_back(token);
  }
  UC_CHECK_EQ(tokens.size(), kOperatingCount);
}

UC_TEST(graph, operating_graph_matches_the_hand_written_reference_table) {
  for (int from_index = kFirstOperatingIndex; from_index <= kLastOperatingIndex; ++from_index) {
    const OperatingState from = static_cast<OperatingState>(from_index);
    for (int to_index = kFirstOperatingIndex; to_index <= kLastOperatingIndex; ++to_index) {
      const OperatingState to = static_cast<OperatingState>(to_index);
      const bool actual = is_legal_operating_transition(from, to);
      const bool expected = reference_operating_edge(from, to);
      UC_CHECK_MSG(actual == expected, "edge " + state_name(from) + " -> " + state_name(to) +
                                           ": the library says " +
                                           (actual ? "legal" : "illegal") + ", the reference "
                                           "table says " + (expected ? "legal" : "illegal"));
    }
  }
}

UC_TEST(graph, operating_transition_is_irreflexive) {
  for (int index = kFirstOperatingIndex; index <= kLastOperatingIndex; ++index) {
    const OperatingState state = static_cast<OperatingState>(index);
    UC_CHECK_MSG(!is_legal_operating_transition(state, state),
                 "the graph contains the self edge " + state_name(state) + " -> " +
                     state_name(state));
  }
}

UC_TEST(graph, unknown_is_a_closed_source_state_and_retired_is_terminal) {
  for (int index = kFirstOperatingIndex; index <= kLastOperatingIndex; ++index) {
    const OperatingState state = static_cast<OperatingState>(index);
    UC_CHECK_MSG(!is_legal_operating_transition(OperatingState::Unknown, state),
                 "Unknown has an outgoing control edge to " + state_name(state) +
                     "; only an adopted observation may replace Unknown");
    UC_CHECK_MSG(!is_legal_operating_transition(OperatingState::Retired, state),
                 "Retired has an outgoing control edge to " + state_name(state));
  }
  UC_CHECK(legal_successors(OperatingState::Unknown).empty());
  UC_CHECK(legal_successors(OperatingState::Retired).empty());
  UC_CHECK(is_terminal(OperatingState::Retired));
  for (int index = kFirstOperatingIndex; index < kLastOperatingIndex; ++index) {
    UC_CHECK(!is_terminal(static_cast<OperatingState>(index)));
  }
}

UC_TEST(graph, no_operating_edge_targets_unknown) {
  for (int from_index = kFirstOperatingIndex; from_index <= kLastOperatingIndex; ++from_index) {
    const OperatingState from = static_cast<OperatingState>(from_index);
    UC_CHECK_MSG(!is_legal_operating_transition(from, OperatingState::Unknown),
                 "the control edge " + state_name(from) +
                     " -> unknown exists; entering Unknown is adoption, not control");
  }
}

UC_TEST(graph, every_operating_state_except_unknown_has_an_incoming_control_edge) {
  for (int to_index = kFirstOperatingIndex + 1; to_index <= kLastOperatingIndex; ++to_index) {
    const OperatingState to = static_cast<OperatingState>(to_index);
    bool reachable = false;
    for (int from_index = kFirstOperatingIndex; from_index <= kLastOperatingIndex; ++from_index) {
      if (is_legal_operating_transition(static_cast<OperatingState>(from_index), to)) {
        reachable = true;
      }
    }
    UC_CHECK_MSG(reachable, "no control edge reaches " + state_name(to) +
                                ", so the state could never be entered by control");
  }
}

UC_TEST(graph, faulted_reaches_only_isolated_offline_and_retired) {
  const std::vector<OperatingState> expected = {OperatingState::Isolated, OperatingState::Offline,
                                                OperatingState::Retired};
  for (int index = kFirstOperatingIndex; index <= kLastOperatingIndex; ++index) {
    const OperatingState to = static_cast<OperatingState>(index);
    const bool legal = is_legal_operating_transition(OperatingState::Faulted, to);
    const bool wanted = contains(expected, to);
    UC_CHECK_MSG(legal == wanted, "control out of faulted to " + state_name(to) + " is " +
                                      (legal ? "legal" : "illegal") +
                                      "; returning a faulted unit to a healthy state is an "
                                      "observation, not a command");
  }
  UC_CHECK_EQ(legal_successors(OperatingState::Faulted).size(), std::size_t{3});
}

UC_TEST(graph, documented_operating_edges_hold) {
  // docs/authority-and-transitions.md lists the complete successor set of every
  // operating state; these are the pairs the rest of the model depends on.
  UC_CHECK(is_legal_operating_transition(OperatingState::OnlineNormal,
                                         OperatingState::MaintenanceBypass));
  UC_CHECK(is_legal_operating_transition(OperatingState::MaintenanceBypass,
                                         OperatingState::OnlineNormal));
  UC_CHECK(is_legal_operating_transition(OperatingState::OnlineNormal,
                                         OperatingState::StaticBypass));
  UC_CHECK(is_legal_operating_transition(OperatingState::StaticBypass,
                                         OperatingState::OnlineNormal));
  UC_CHECK(is_legal_operating_transition(OperatingState::BatteryTest,
                                         OperatingState::OnlineBattery));
  UC_CHECK(is_legal_operating_transition(OperatingState::BatteryTest,
                                         OperatingState::OnlineNormal));
  UC_CHECK(is_legal_operating_transition(OperatingState::SelfTest,
                                         OperatingState::OnlineNormal));
  UC_CHECK(is_legal_operating_transition(OperatingState::OnlineNormal,
                                         OperatingState::OnlineBattery));
  UC_CHECK(is_legal_operating_transition(OperatingState::OnlineBattery,
                                         OperatingState::Recharge));
  UC_CHECK(is_legal_operating_transition(OperatingState::Recharge,
                                         OperatingState::OnlineNormal));
  UC_CHECK(is_legal_operating_transition(OperatingState::Standby, OperatingState::OnlineNormal));
  UC_CHECK(is_legal_operating_transition(OperatingState::Isolated, OperatingState::Standby));
  UC_CHECK(is_legal_operating_transition(OperatingState::StaticBypass,
                                          OperatingState::MaintenanceBypass));
  // Undefined pairs are illegal, in both directions.
  UC_CHECK(!is_legal_operating_transition(OperatingState::Faulted,
                                          OperatingState::OnlineNormal));
  UC_CHECK(!is_legal_operating_transition(OperatingState::Retired, OperatingState::Offline));
  UC_CHECK(!is_legal_operating_transition(OperatingState::Offline,
                                          OperatingState::OnlineNormal));
}

UC_TEST(graph, legal_successors_returns_exactly_the_reference_successors_in_a_stable_order) {
  const auto table = reference_operating_successors();
  for (int from_index = kFirstOperatingIndex; from_index <= kLastOperatingIndex; ++from_index) {
    const OperatingState from = static_cast<OperatingState>(from_index);
    std::vector<OperatingState> expected = table[static_cast<std::size_t>(from)];
    // The reference table above is written in the order the document lists the
    // successors; the library documents a stable order, which is ascending
    // declaration order. Compare as sets, then check the order separately.
    std::sort(expected.begin(), expected.end(),
              [](OperatingState left, OperatingState right) {
                return static_cast<int>(left) < static_cast<int>(right);
              });
    const std::vector<OperatingState> actual = legal_successors(from);
    UC_CHECK_MSG(actual == expected, "legal_successors(" + state_name(from) +
                                         ") does not equal the reference successor set");
    // Deterministic: a second call returns exactly the same list.
    UC_CHECK(legal_successors(from) == actual);
    // Stable order: unique and strictly ascending in declaration order.
    for (std::size_t index = 0; index < actual.size(); ++index) {
      UC_CHECK_MSG(static_cast<int>(actual[index]) >= kFirstOperatingIndex &&
                       static_cast<int>(actual[index]) <= kLastOperatingIndex,
                   "legal_successors returned a value outside the documented enumerators");
      if (index > 0) {
        UC_CHECK_MSG(static_cast<int>(actual[index - 1]) < static_cast<int>(actual[index]),
                     "legal_successors(" + state_name(from) + ") is not in ascending order");
      }
    }
    // The list and the predicate agree for every pair.
    for (int to_index = kFirstOperatingIndex; to_index <= kLastOperatingIndex; ++to_index) {
      const OperatingState to = static_cast<OperatingState>(to_index);
      UC_CHECK_MSG(contains(actual, to) == is_legal_operating_transition(from, to),
                   "legal_successors(" + state_name(from) + ") disagrees with the predicate for " +
                       state_name(to));
    }
    // A control edge never stays in place.
    UC_CHECK(!contains(actual, from));
  }
}

UC_TEST(graph, lifecycle_state_numeric_values_and_tokens_are_the_documented_ones) {
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Commissioning), 1);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Standby), 2);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::InService), 3);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Maintenance), 4);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Degraded), 5);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Faulted), 6);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Isolated), 7);
  UC_CHECK_EQ(static_cast<int>(LifecycleState::Decommissioned), 8);

  const std::vector<std::string> documented_tokens = {
      "commissioning", "standby",  "in_service", "maintenance",
      "degraded",      "faulted",  "isolated",   "decommissioned"};
  UC_CHECK_EQ(documented_tokens.size(), kLifecycleCount);
  std::vector<std::string> tokens;
  for (int index = kFirstLifecycleIndex; index <= kLastLifecycleIndex; ++index) {
    const LifecycleState state = static_cast<LifecycleState>(index);
    const std::string token = to_string(state);
    UC_CHECK_MSG(!token.empty(), "lifecycle state " + std::to_string(index) +
                                     " has no stable token");
    UC_CHECK_EQ(token, documented_tokens[static_cast<std::size_t>(index - 1)]);
    UC_CHECK_MSG(std::find(tokens.begin(), tokens.end(), token) == tokens.end(),
                 "two lifecycle states share the token '" + token + "'");
    tokens.push_back(token);
  }
  UC_CHECK_EQ(tokens.size(), kLifecycleCount);
}

UC_TEST(graph, lifecycle_graph_matches_the_hand_written_reference_table) {
  for (int from_index = kFirstLifecycleIndex; from_index <= kLastLifecycleIndex; ++from_index) {
    const LifecycleState from = static_cast<LifecycleState>(from_index);
    for (int to_index = kFirstLifecycleIndex; to_index <= kLastLifecycleIndex; ++to_index) {
      const LifecycleState to = static_cast<LifecycleState>(to_index);
      const bool actual = is_legal_lifecycle_transition(from, to);
      const bool expected = reference_lifecycle_edge(from, to);
      UC_CHECK_MSG(actual == expected, "edge " + state_name(from) + " -> " + state_name(to) +
                                           ": the library says " +
                                           (actual ? "legal" : "illegal") + ", the reference "
                                           "table says " + (expected ? "legal" : "illegal"));
    }
  }
}

UC_TEST(graph, lifecycle_graph_has_no_self_edges_no_return_to_commissioning_and_is_reachable) {
  for (int index = kFirstLifecycleIndex; index <= kLastLifecycleIndex; ++index) {
    const LifecycleState state = static_cast<LifecycleState>(index);
    UC_CHECK_MSG(!is_legal_lifecycle_transition(state, state),
                 "the lifecycle graph contains the self edge " + state_name(state));
  }
  for (int from_index = kFirstLifecycleIndex; from_index <= kLastLifecycleIndex; ++from_index) {
    const LifecycleState from = static_cast<LifecycleState>(from_index);
    UC_CHECK_MSG(!is_legal_lifecycle_transition(from, LifecycleState::Commissioning),
                 "an administrative edge returns to commissioning from " + state_name(from) +
                     "; commissioning is the initial state");
    UC_CHECK_MSG(!(from == LifecycleState::Decommissioned &&
                   is_legal_lifecycle_transition(from, LifecycleState::Standby)),
                 "decommissioned is terminal");
  }
  // Every lifecycle state is reachable by administrative transition from
  // Commissioning, and every state except Decommissioned can be left.
  for (int to_index = kFirstLifecycleIndex; to_index <= kLastLifecycleIndex; ++to_index) {
    const LifecycleState to = static_cast<LifecycleState>(to_index);
    bool outgoing = false;
    for (int probe = kFirstLifecycleIndex; probe <= kLastLifecycleIndex; ++probe) {
      if (is_legal_lifecycle_transition(to, static_cast<LifecycleState>(probe))) {
        outgoing = true;
      }
    }
    UC_CHECK_MSG(outgoing == (to != LifecycleState::Decommissioned),
                 "lifecycle state " + state_name(to) + " has unexpected outgoing edges");
  }
  UC_CHECK(is_terminal(LifecycleState::Decommissioned));
  for (int index = kFirstLifecycleIndex; index < kLastLifecycleIndex; ++index) {
    UC_CHECK(!is_terminal(static_cast<LifecycleState>(index)));
  }
}

UC_TEST(graph, documented_lifecycle_edges_hold) {
  // docs/authority-and-transitions.md, "Lifecycle (administrative, durable)":
  // the complete successor set of every lifecycle state.
  const std::vector<LifecycleState> commissioning = {
      LifecycleState::Standby, LifecycleState::InService, LifecycleState::Decommissioned};
  const std::vector<LifecycleState> standby = {
      LifecycleState::InService, LifecycleState::Maintenance, LifecycleState::Faulted,
      LifecycleState::Decommissioned};
  const std::vector<LifecycleState> in_service = {
      LifecycleState::Maintenance, LifecycleState::Degraded, LifecycleState::Faulted,
      LifecycleState::Standby, LifecycleState::Isolated, LifecycleState::Decommissioned};
  const std::vector<LifecycleState> maintenance = {
      LifecycleState::InService, LifecycleState::Standby, LifecycleState::Isolated,
      LifecycleState::Faulted, LifecycleState::Decommissioned};
  const std::vector<LifecycleState> degraded = {
      LifecycleState::InService, LifecycleState::Maintenance, LifecycleState::Faulted,
      LifecycleState::Isolated, LifecycleState::Decommissioned};
  const std::vector<LifecycleState> faulted = {
      LifecycleState::InService, LifecycleState::Degraded, LifecycleState::Maintenance,
      LifecycleState::Isolated, LifecycleState::Decommissioned};
  const std::vector<LifecycleState> isolated = {
      LifecycleState::Maintenance, LifecycleState::Standby, LifecycleState::Decommissioned};
  const std::vector<std::vector<LifecycleState>> documented = {
      commissioning, standby, in_service, maintenance, degraded, faulted, isolated};
  for (std::size_t index = 0; index < documented.size(); ++index) {
    const LifecycleState from = static_cast<LifecycleState>(index + 1);
    for (const LifecycleState to : documented[index]) {
      UC_CHECK_MSG(is_legal_lifecycle_transition(from, to),
                   "the documented lifecycle edge " + state_name(from) + " -> " + state_name(to) +
                       " is illegal");
    }
  }
  // The progression chain of the document is exactly this edge list: in
  // particular Maintenance -> Degraded is NOT an edge, and neither is the return
  // to Commissioning, which is the initial state.
  UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Maintenance, LifecycleState::Degraded));
  UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Commissioning,
                                          LifecycleState::Faulted));
  UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Isolated, LifecycleState::InService));
  UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Degraded, LifecycleState::Standby));
  UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Decommissioned,
                                          LifecycleState::InService));
  UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Decommissioned,
                                          LifecycleState::Commissioning));
  for (int index = kFirstLifecycleIndex; index <= kLastLifecycleIndex; ++index) {
    UC_CHECK(!is_legal_lifecycle_transition(LifecycleState::Commissioning,
                                            static_cast<LifecycleState>(index)) ||
             index == static_cast<int>(LifecycleState::Standby) ||
             index == static_cast<int>(LifecycleState::InService) ||
             index == static_cast<int>(LifecycleState::Decommissioned));
  }
}

UC_TEST(graph, lifecycle_permits_control_matches_the_documented_set) {
  for (int index = kFirstLifecycleIndex; index <= kLastLifecycleIndex; ++index) {
    const LifecycleState state = static_cast<LifecycleState>(index);
    const bool actual = lifecycle_permits_control(state);
    const bool expected = reference_lifecycle_permits_control(state);
    UC_CHECK_MSG(actual == expected, "lifecycle_permits_control(" + state_name(state) + ") is " +
                                         (actual ? "true" : "false") + ", documented as " +
                                         (expected ? "true" : "false"));
  }
  UC_CHECK(!lifecycle_permits_control(LifecycleState::Commissioning));
  UC_CHECK(lifecycle_permits_control(LifecycleState::Standby));
  UC_CHECK(lifecycle_permits_control(LifecycleState::InService));
  UC_CHECK(lifecycle_permits_control(LifecycleState::Degraded));
  UC_CHECK(!lifecycle_permits_control(LifecycleState::Maintenance));
  UC_CHECK(!lifecycle_permits_control(LifecycleState::Faulted));
  UC_CHECK(!lifecycle_permits_control(LifecycleState::Isolated));
  UC_CHECK(!lifecycle_permits_control(LifecycleState::Decommissioned));
}

UC_TEST(graph, is_consistent_matches_the_hand_written_reference_table) {
  for (int lifecycle_index = kFirstLifecycleIndex; lifecycle_index <= kLastLifecycleIndex;
       ++lifecycle_index) {
    const LifecycleState lifecycle = static_cast<LifecycleState>(lifecycle_index);
    for (int operating_index = kFirstOperatingIndex; operating_index <= kLastOperatingIndex;
         ++operating_index) {
      const OperatingState operating = static_cast<OperatingState>(operating_index);
      const bool actual = is_consistent(lifecycle, operating);
      const bool expected = reference_consistent(lifecycle, operating);
      UC_CHECK_MSG(actual == expected, "is_consistent(" + state_name(lifecycle) + ", " +
                                           state_name(operating) + ") is " +
                                           (actual ? "true" : "false") + ", the reference table "
                                           "says " + (expected ? "true" : "false"));
    }
  }
}

UC_TEST(graph, is_consistent_holds_the_documented_cases) {
  UC_CHECK(is_consistent(LifecycleState::InService, OperatingState::OnlineNormal));
  UC_CHECK(is_consistent(LifecycleState::InService, OperatingState::OnlineBattery));
  UC_CHECK(is_consistent(LifecycleState::Degraded, OperatingState::OnlineNormal));
  UC_CHECK(is_consistent(LifecycleState::Maintenance, OperatingState::MaintenanceBypass));
  UC_CHECK(is_consistent(LifecycleState::Standby, OperatingState::Offline));
  UC_CHECK(is_consistent(LifecycleState::Commissioning, OperatingState::Standby));
  UC_CHECK(is_consistent(LifecycleState::Decommissioned, OperatingState::Retired));
  UC_CHECK(is_consistent(LifecycleState::Decommissioned, OperatingState::Offline));
  UC_CHECK(is_consistent(LifecycleState::Isolated, OperatingState::Isolated));
  UC_CHECK(is_consistent(LifecycleState::Isolated, OperatingState::Offline));

  // A decommissioned or isolated unit cannot be reported as carrying the load.
  const std::vector<OperatingState> carrying = {
      OperatingState::OnlineNormal, OperatingState::OnlineBattery, OperatingState::StaticBypass,
      OperatingState::MaintenanceBypass, OperatingState::SelfTest, OperatingState::BatteryTest,
      OperatingState::Recharge};
  for (const OperatingState operating : carrying) {
    UC_CHECK_MSG(!is_consistent(LifecycleState::Decommissioned, operating),
                 "a decommissioned unit was reported consistent with " + state_name(operating));
    UC_CHECK_MSG(!is_consistent(LifecycleState::Commissioning, operating),
                 "a commissioning unit was reported consistent with " + state_name(operating));
    UC_CHECK_MSG(!is_consistent(LifecycleState::Isolated, operating),
                 "an isolated unit was reported consistent with " + state_name(operating));
  }
  // A retired device cannot be administratively in service.
  for (int index = kFirstLifecycleIndex; index < kLastLifecycleIndex; ++index) {
    const LifecycleState lifecycle = static_cast<LifecycleState>(index);
    if (lifecycle == LifecycleState::Isolated || lifecycle == LifecycleState::Commissioning) {
      continue;
    }
    UC_CHECK_MSG(!is_consistent(lifecycle, OperatingState::Retired),
                 "lifecycle " + state_name(lifecycle) + " was consistent with a retired device");
  }
  UC_CHECK(!is_consistent(LifecycleState::Decommissioned, OperatingState::Unknown));
  UC_CHECK(!is_consistent(LifecycleState::InService, OperatingState::Retired));

  // Both relations must be total in the sense that every lifecycle state admits
  // at least one operating state and vice versa, or no telemetry could ever be
  // adopted for it.
  for (int lifecycle_index = kFirstLifecycleIndex; lifecycle_index <= kLastLifecycleIndex;
       ++lifecycle_index) {
    const LifecycleState lifecycle = static_cast<LifecycleState>(lifecycle_index);
    bool any = false;
    for (int operating_index = kFirstOperatingIndex; operating_index <= kLastOperatingIndex;
         ++operating_index) {
      if (is_consistent(lifecycle, static_cast<OperatingState>(operating_index))) {
        any = true;
      }
    }
    UC_CHECK_MSG(any, "no operating state is consistent with lifecycle " + state_name(lifecycle));
  }
  for (int operating_index = kFirstOperatingIndex; operating_index <= kLastOperatingIndex;
       ++operating_index) {
    const OperatingState operating = static_cast<OperatingState>(operating_index);
    bool any = false;
    for (int lifecycle_index = kFirstLifecycleIndex; lifecycle_index <= kLastLifecycleIndex;
         ++lifecycle_index) {
      if (is_consistent(static_cast<LifecycleState>(lifecycle_index), operating)) {
        any = true;
      }
    }
    UC_CHECK_MSG(any, "no lifecycle state is consistent with operating state " +
                          state_name(operating));
  }
}

UC_TEST(graph, protecting_bypass_and_testing_cover_exactly_their_documented_states) {
  const std::vector<OperatingState> protecting = {
      OperatingState::OnlineNormal, OperatingState::OnlineBattery, OperatingState::Recharge,
      OperatingState::SelfTest,     OperatingState::BatteryTest};
  const std::vector<OperatingState> bypass = {OperatingState::StaticBypass,
                                              OperatingState::MaintenanceBypass};
  const std::vector<OperatingState> testing = {OperatingState::SelfTest,
                                               OperatingState::BatteryTest};
  for (int index = kFirstOperatingIndex; index <= kLastOperatingIndex; ++index) {
    const OperatingState state = static_cast<OperatingState>(index);
    UC_CHECK_MSG(is_protecting(state) == contains(protecting, state),
                 "is_protecting(" + state_name(state) + ") is not the documented set: the UPS "
                 "protects the load from its own conditioned path or energy store");
    UC_CHECK_MSG(is_bypass(state) == contains(bypass, state),
                 "is_bypass(" + state_name(state) + ") is not the documented set of raw bypass "
                 "paths");
    UC_CHECK_MSG(is_testing(state) == contains(testing, state),
                 "is_testing(" + state_name(state) + ") is not the documented set of test states");
    // A raw bypass path is not the UPS's own conditioned path, so the two
    // predicates are disjoint.
    UC_CHECK_MSG(!(is_protecting(state) && is_bypass(state)),
                 state_name(state) + " is both protecting and on a raw bypass path");
    // Both documented test states keep the load on the inverter.
    if (is_testing(state)) {
      UC_CHECK_MSG(is_protecting(state),
                   state_name(state) + " is a test state but not reported as protecting");
    }
    // Exactly one classification for every state except Unknown, which claims
    // nothing.
    if (state == OperatingState::Unknown) {
      UC_CHECK(!is_protecting(state) && !is_bypass(state) && !is_testing(state));
    }
  }
  UC_CHECK(is_protecting(OperatingState::BatteryTest));
  UC_CHECK(is_protecting(OperatingState::SelfTest));
  UC_CHECK(is_bypass(OperatingState::MaintenanceBypass));
  UC_CHECK(!is_protecting(OperatingState::MaintenanceBypass));
  UC_CHECK(!is_protecting(OperatingState::Faulted));
  UC_CHECK(!is_bypass(OperatingState::OnlineNormal));
  UC_CHECK(!is_testing(OperatingState::Recharge));
}

UC_TEST(graph, static_bypass_ready_requires_all_five_tri_states) {
  // docs/authority-and-transitions.md, "Bypass and transfer readiness": a static
  // bypass transfer requires availability, qualification, synchronisation,
  // transfer readiness AND battery readiness, because the transfer window is
  // carried by the battery if the input fails part-way through. Readiness and
  // evaluation require the same set, so that they never disagree.
  int ready_combinations = 0;
  for (int code = 0; code < 243; ++code) {
    int digits[5] = {0, 0, 0, 0, 0};
    int remaining = code;
    for (int index = 0; index < 5; ++index) {
      digits[index] = remaining % 3;
      remaining /= 3;
    }
    const Bool3 available = static_cast<Bool3>(digits[0]);
    const Bool3 qualified = static_cast<Bool3>(digits[1]);
    const Bool3 synchronized = static_cast<Bool3>(digits[2]);
    const Bool3 transfer_ready = static_cast<Bool3>(digits[3]);
    const Bool3 battery_ready = static_cast<Bool3>(digits[4]);
    const TransferStatus status =
        transfer_with(available, qualified, synchronized, transfer_ready, battery_ready);
    const bool expected = available == Bool3::True && qualified == Bool3::True &&
                          synchronized == Bool3::True && transfer_ready == Bool3::True &&
                          battery_ready == Bool3::True;
    const bool actual = static_bypass_ready(status);
    UC_CHECK_MSG(actual == expected,
                 "static_bypass_ready(" + transfer_text(available, qualified, synchronized,
                                                        transfer_ready, battery_ready) +
                     ") is " + (actual ? "true" : "false") + ", the reference predicate says " +
                     (expected ? "true" : "false"));
    if (actual) {
      ++ready_combinations;
    }
  }
  // Exactly one of the 243 combinations is ready: every one of the five
  // tri-states must be exactly True, and nothing is free.
  UC_CHECK_EQ(ready_combinations, 1);

  // Unknown and False are equally unsatisfying for every required member.
  const std::vector<Bool3> not_true = {Bool3::Unknown, Bool3::False};
  for (const Bool3 value : not_true) {
    UC_CHECK(!static_bypass_ready(transfer_with(value, Bool3::True, Bool3::True, Bool3::True,
                                                Bool3::True)));
    UC_CHECK(!static_bypass_ready(transfer_with(Bool3::True, value, Bool3::True, Bool3::True,
                                                Bool3::True)));
    UC_CHECK(!static_bypass_ready(transfer_with(Bool3::True, Bool3::True, value, Bool3::True,
                                                Bool3::True)));
    UC_CHECK(!static_bypass_ready(transfer_with(Bool3::True, Bool3::True, Bool3::True, value,
                                                Bool3::True)));
    UC_CHECK(!static_bypass_ready(transfer_with(Bool3::True, Bool3::True, Bool3::True, Bool3::True,
                                                value)));
  }
  UC_CHECK(static_bypass_ready(transfer_with(Bool3::True, Bool3::True, Bool3::True, Bool3::True,
                                             Bool3::True)));
  // A default-constructed transfer status satisfies nothing.
  UC_CHECK(!static_bypass_ready(TransferStatus{}));
}

UC_TEST(graph, maintenance_bypass_ready_requires_availability_and_qualification) {
  int ready_combinations = 0;
  for (int code = 0; code < 243; ++code) {
    int digits[5] = {0, 0, 0, 0, 0};
    int remaining = code;
    for (int index = 0; index < 5; ++index) {
      digits[index] = remaining % 3;
      remaining /= 3;
    }
    const Bool3 available = static_cast<Bool3>(digits[0]);
    const Bool3 qualified = static_cast<Bool3>(digits[1]);
    const Bool3 synchronized = static_cast<Bool3>(digits[2]);
    const Bool3 transfer_ready = static_cast<Bool3>(digits[3]);
    const Bool3 battery_ready = static_cast<Bool3>(digits[4]);
    const TransferStatus status =
        transfer_with(available, qualified, synchronized, transfer_ready, battery_ready);
    const bool expected = available == Bool3::True && qualified == Bool3::True;
    const bool actual = maintenance_bypass_ready(status);
    UC_CHECK_MSG(actual == expected,
                 "maintenance_bypass_ready(" + transfer_text(available, qualified, synchronized,
                                                              transfer_ready, battery_ready) +
                     ") is " + (actual ? "true" : "false") + ", the reference predicate says " +
                     (expected ? "true" : "false"));
    if (actual) {
      ++ready_combinations;
    }
  }
  UC_CHECK_EQ(ready_combinations, 27);

  UC_CHECK(!maintenance_bypass_ready(transfer_with(Bool3::Unknown, Bool3::True, Bool3::True,
                                                   Bool3::True, Bool3::True)));
  UC_CHECK(!maintenance_bypass_ready(transfer_with(Bool3::False, Bool3::True, Bool3::True,
                                                   Bool3::True, Bool3::True)));
  UC_CHECK(!maintenance_bypass_ready(transfer_with(Bool3::True, Bool3::Unknown, Bool3::True,
                                                   Bool3::True, Bool3::True)));
  UC_CHECK(!maintenance_bypass_ready(transfer_with(Bool3::True, Bool3::False, Bool3::True,
                                                   Bool3::True, Bool3::True)));
  UC_CHECK(maintenance_bypass_ready(transfer_with(Bool3::True, Bool3::True, Bool3::True,
                                                  Bool3::True, Bool3::True)));
  UC_CHECK(!maintenance_bypass_ready(TransferStatus{}));
}

UC_TEST(graph, readiness_and_evaluation_agree_on_the_static_bypass_preconditions) {
  // docs/authority-and-transitions.md: "Readiness and evaluation require the
  // same set of capabilities for the same operation, so they never disagree."
  // A device that does not report battery readiness must both fail the readiness
  // snapshot and make the command itself refuse with BatteryNotReady.
  uc_test::Fixture fixture("graph-bypass-agreement");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UpsQuery query;
  query.ups = unit.id;
  query.now = fixture.now;

  TelemetryReport unknown_battery =
      uc_test::healthy_report(unit.id, unit.hardware, fixture.now, SourceRevision{2}, "ev-2");
  unknown_battery.transfer.battery_ready = Bool3::Unknown;
  UC_REQUIRE_OK(fixture.observe(unknown_battery, fixture.now));

  const ReadinessReport not_ready = UC_REQUIRE_OK(fixture.engine->readiness(query));
  UC_CHECK_MSG(!not_ready.static_bypass_ready,
               "readiness reported a static bypass transfer as ready while the battery is not "
               "reported ready, which the command would refuse");
  const TransitionEvaluation refused = UC_REQUIRE_OK(fixture.engine->evaluate(
      fixture.command(CommandKind::EnterStaticBypass, "bypass-unknown-battery", fixture.now,
                      "test-authority")));
  UC_CHECK_MSG(!refused.report.allowed, "the evaluation allowed a static bypass transfer whose "
                                        "battery readiness was never reported");
  UC_CHECK_EQ(refused.report.primary, RefusalCode::BatteryNotReady);

  // The same report with battery readiness established satisfies both.
  TelemetryReport ready_battery =
      uc_test::healthy_report(unit.id, unit.hardware, fixture.now, SourceRevision{3}, "ev-3");
  UC_REQUIRE_OK(fixture.observe(ready_battery, fixture.now));
  const ReadinessReport ready = UC_REQUIRE_OK(fixture.engine->readiness(query));
  UC_CHECK_MSG(ready.static_bypass_ready,
               "readiness did not report a fully established static bypass transfer as ready");
  const TransitionEvaluation allowed = UC_REQUIRE_OK(fixture.engine->evaluate(
      fixture.command(CommandKind::EnterStaticBypass, "bypass-ready-battery", fixture.now,
                      "test-authority")));
  UC_CHECK_MSG(allowed.report.allowed,
               "the evaluation refused a fully established static bypass transfer: " +
                   std::string(to_string(allowed.report.primary)) + " " +
                   allowed.report.primary_detail);
}

UC_TEST(graph, readiness_and_evaluation_agree_on_the_maintenance_bypass_preconditions) {
  // The documented capability table has one row per operation. The maintenance
  // row is availability plus qualification, and it must not require more: a
  // status with everything else unknown still reports a ready maintenance bypass.
  uc_test::Fixture fixture("graph-maintenance-agreement");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry, ReserveUnit::Seconds, 600));

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UpsQuery query;
  query.ups = unit.id;
  query.now = fixture.now;

  TelemetryReport unqualified =
      uc_test::healthy_report(unit.id, unit.hardware, fixture.now, SourceRevision{2}, "ev-2");
  unqualified.transfer.bypass_qualified = Bool3::Unknown;
  UC_REQUIRE_OK(fixture.observe(unqualified, fixture.now));
  const ReadinessReport not_qualified = UC_REQUIRE_OK(fixture.engine->readiness(query));
  UC_CHECK_MSG(!not_qualified.maintenance_bypass_ready,
               "readiness reported a maintenance bypass transfer as ready while the bypass path "
               "is not reported qualified");
  const TransitionEvaluation refused = UC_REQUIRE_OK(fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "maintenance-unqualified", fixture.now,
                      "test-authority")));
  UC_CHECK(!refused.report.allowed);
  UC_CHECK_EQ(refused.report.primary, RefusalCode::BypassNotQualified);

  // Transfer readiness is required for leaving a bypass, not for entering a
  // maintenance bypass, so it must not make the maintenance predicate false.
  TelemetryReport transfer_not_ready =
      uc_test::healthy_report(unit.id, unit.hardware, fixture.now, SourceRevision{3}, "ev-3");
  transfer_not_ready.transfer.transfer_ready = Bool3::Unknown;
  transfer_not_ready.transfer.battery_ready = Bool3::Unknown;
  UC_REQUIRE_OK(fixture.observe(transfer_not_ready, fixture.now));
  const ReadinessReport partial = UC_REQUIRE_OK(fixture.engine->readiness(query));
  UC_CHECK_MSG(partial.maintenance_bypass_ready,
               "the maintenance bypass predicate required transfer readiness or battery "
               "readiness, which the documented capability table does not");
  UC_CHECK_MSG(!partial.transfer_ready,
               "readiness reported transfer readiness while the device does not assert it");
  UC_CHECK_MSG(!partial.static_bypass_ready,
               "readiness reported a static bypass transfer as ready with transfer readiness and "
               "battery readiness unreported");

  TelemetryReport fully_established =
      uc_test::healthy_report(unit.id, unit.hardware, fixture.now, SourceRevision{4}, "ev-4");
  UC_REQUIRE_OK(fixture.observe(fully_established, fixture.now));
  const ReadinessReport ready = UC_REQUIRE_OK(fixture.engine->readiness(query));
  UC_CHECK(ready.maintenance_bypass_ready);
  UC_CHECK(ready.transfer_ready);
  const TransitionEvaluation allowed = UC_REQUIRE_OK(fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "maintenance-ready", fixture.now,
                      "test-authority")));
  UC_CHECK_MSG(allowed.report.allowed,
               "the evaluation refused a fully established maintenance bypass transfer: " +
                   std::string(to_string(allowed.report.primary)) + " " +
                   allowed.report.primary_detail);
}

UC_TEST(graph, readiness_predicates_are_monotone_in_the_tri_states) {
  // Making a tri-state stronger (Unknown or False -> True) can only preserve
  // readiness, never remove it. This holds for both predicates and for every
  // combination, independently of how many members each predicate requires.
  for (int code = 0; code < 243; ++code) {
    int digits[5] = {0, 0, 0, 0, 0};
    int remaining = code;
    for (int index = 0; index < 5; ++index) {
      digits[index] = remaining % 3;
      remaining /= 3;
    }
    const TransferStatus base =
        transfer_with(static_cast<Bool3>(digits[0]), static_cast<Bool3>(digits[1]),
                      static_cast<Bool3>(digits[2]), static_cast<Bool3>(digits[3]),
                      static_cast<Bool3>(digits[4]));
    for (int upgrade = 0; upgrade < 5; ++upgrade) {
      int stronger[5] = {digits[0], digits[1], digits[2], digits[3], digits[4]};
      stronger[upgrade] = 1;  // Bool3::True
      const TransferStatus upgraded =
          transfer_with(static_cast<Bool3>(stronger[0]), static_cast<Bool3>(stronger[1]),
                        static_cast<Bool3>(stronger[2]), static_cast<Bool3>(stronger[3]),
                        static_cast<Bool3>(stronger[4]));
      if (static_bypass_ready(base)) {
        UC_CHECK_MSG(static_bypass_ready(upgraded),
                     "static_bypass_ready was lost by strengthening tri-state " +
                         std::to_string(upgrade));
      }
      if (maintenance_bypass_ready(base)) {
        UC_CHECK_MSG(maintenance_bypass_ready(upgraded),
                     "maintenance_bypass_ready was lost by strengthening tri-state " +
                         std::to_string(upgrade));
      }
    }
  }
}

UC_TEST(graph, bool3_tokens_round_trip_and_refuse_unknown_tokens) {
  UC_CHECK_EQ(static_cast<int>(Bool3::Unknown), 0);
  UC_CHECK_EQ(static_cast<int>(Bool3::True), 1);
  UC_CHECK_EQ(static_cast<int>(Bool3::False), 2);
  const std::vector<Bool3> values = {Bool3::Unknown, Bool3::True, Bool3::False};
  std::vector<std::string> tokens;
  for (const Bool3 value : values) {
    const std::string token = to_string(value);
    UC_CHECK_MSG(!token.empty(), "a Bool3 value has an empty token");
    UC_CHECK_MSG(std::find(tokens.begin(), tokens.end(), token) == tokens.end(),
                 "two Bool3 values share the token '" + token + "'");
    tokens.push_back(token);
    const Bool3 parsed = UC_REQUIRE_OK(parse_bool3(token));
    UC_CHECK_MSG(parsed == value, "Bool3 token '" + token + "' did not round-trip");
  }
  const std::vector<std::string> unknown_tokens = {
      "",        " ",        "Unknown", "TRUE",  "True",   "true ",  " true",
      "falsey",  "unknown_", "yes",     "no",    "1",      "0",      "t",
      "f",       "unknow",   "fals",    "tru"};
  for (const std::string& token : unknown_tokens) {
    const Result<Bool3> parsed = parse_bool3(token);
    UC_CHECK_MSG(!parsed.ok(), "parse_bool3 accepted the unknown token '" + token + "'");
    UC_CHECK_MSG(parsed.status().code() == StatusCode::InvalidArgument,
                 "parse_bool3('" + token + "') must be invalid_argument, got " +
                     std::string(to_string(parsed.status().code())));
  }
}

UC_TEST(graph, lifecycle_state_tokens_round_trip_and_refuse_unknown_tokens) {
  for (int index = kFirstLifecycleIndex; index <= kLastLifecycleIndex; ++index) {
    const LifecycleState state = static_cast<LifecycleState>(index);
    const std::string token = to_string(state);
    const LifecycleState parsed = UC_REQUIRE_OK(parse_lifecycle_state(token));
    UC_CHECK_MSG(parsed == state, "lifecycle token '" + token + "' did not round-trip");
  }
  // A near miss must be refused, never mapped to the neighbouring state.
  const std::vector<std::string> unknown_tokens = {
      "",         " ",          "InService",  "in service", "in_service_",
      "commission", "commisioning", "stand by",   "standby ",
      "decommission", "decommissioned ", "Decommissioned", "fault", "faulted_",
      "isolate",  "isolated_",  "degraded ",  "maintenance_"};
  for (const std::string& token : unknown_tokens) {
    const Result<LifecycleState> parsed = parse_lifecycle_state(token);
    UC_CHECK_MSG(!parsed.ok(), "parse_lifecycle_state accepted the unknown token '" + token + "'");
    UC_CHECK_MSG(parsed.status().code() == StatusCode::InvalidArgument,
                 "parse_lifecycle_state('" + token + "') must be invalid_argument, got " +
                     std::string(to_string(parsed.status().code())));
  }
}

UC_TEST(graph, operating_state_tokens_round_trip_and_refuse_unknown_tokens) {
  for (int index = kFirstOperatingIndex; index <= kLastOperatingIndex; ++index) {
    const OperatingState state = static_cast<OperatingState>(index);
    const std::string token = to_string(state);
    const OperatingState parsed = UC_REQUIRE_OK(parse_operating_state(token));
    UC_CHECK_MSG(parsed == state, "operating token '" + token + "' did not round-trip");
  }
  const std::vector<std::string> unknown_tokens = {
      "",           " ",             "Unknown",     "OnlineNormal", "online normal",
      "online-normal", "online",     "on_battery",  "online_batteries", "static",
      "static_bypass ", "maintenance", "maintenance bypass", "MaintenanceBypass",
      "battery_test ", "self-test",  "self_test_",  "fault",       "isolate",
      "retire",     "off_line",      "standby_"};
  for (const std::string& token : unknown_tokens) {
    const Result<OperatingState> parsed = parse_operating_state(token);
    UC_CHECK_MSG(!parsed.ok(), "parse_operating_state accepted the unknown token '" + token + "'");
    UC_CHECK_MSG(parsed.status().code() == StatusCode::InvalidArgument,
                 "parse_operating_state('" + token + "') must be invalid_argument, got " +
                     std::string(to_string(parsed.status().code())));
  }
}

UC_TEST(graph, bypass_kind_tokens_are_distinct_and_the_defaults_are_unknown) {
  const std::vector<BypassKind> kinds = {BypassKind::Unknown, BypassKind::None,
                                         BypassKind::Static, BypassKind::Maintenance};
  std::vector<std::string> tokens;
  for (const BypassKind kind : kinds) {
    const std::string token = to_string(kind);
    UC_CHECK_MSG(!token.empty(), "a bypass kind has an empty token");
    UC_CHECK_MSG(std::find(tokens.begin(), tokens.end(), token) == tokens.end(),
                 "two bypass kinds share the token '" + token + "'");
    tokens.push_back(token);
  }
  UC_CHECK_EQ(tokens.size(), std::size_t{4});
  // The tri-state transfer status defaults to "unknown", never to "none".
  UC_CHECK_EQ(TransferStatus{}.bypass_kind, BypassKind::Unknown);
  UC_CHECK_EQ(TransferStatus{}.bypass_available, Bool3::Unknown);
  UC_CHECK_EQ(TransferStatus{}.bypass_qualified, Bool3::Unknown);
  UC_CHECK_EQ(TransferStatus{}.output_synchronized, Bool3::Unknown);
  UC_CHECK_EQ(TransferStatus{}.transfer_ready, Bool3::Unknown);
  UC_CHECK_EQ(TransferStatus{}.battery_ready, Bool3::Unknown);
}
