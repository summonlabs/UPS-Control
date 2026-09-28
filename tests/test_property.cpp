// Seeded randomized state-machine and invariant walks.
//
// Every random choice comes from a fixed seed that is part of the test, and every
// failure message names the seed, the step index, the action and the observed
// outcome, so a failure reproduces exactly. The invariants are checked against an
// independent model built inside the test, never against a second reading of the
// same engine state.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "test_harness.hpp"

#include "ups_control/engine.hpp"

using namespace ups_control;

namespace {

constexpr const char* kUnitText = "property-unit";
constexpr const char* kGrantText = "property-grant";
/// The authority that asserts protected-load obligations. It is deliberately not
/// the authority a control request cites, so a MustRemainProtected obligation can
/// never be covered by a bare authority citation.
constexpr const char* kLoadAuthorityText = "load-authority";
constexpr std::int64_t kCreatedAt = 1000;
constexpr std::int64_t kStartTick = 2000;
constexpr std::int64_t kTickStep = 3;

std::uint64_t mix(std::uint64_t digest, std::uint64_t value) {
  digest ^= value;
  digest *= 1099511628211ull;
  return digest;
}

std::uint64_t mix_status(std::uint64_t digest, const Status& status) {
  return mix(digest, static_cast<std::uint64_t>(status.code()));
}

struct Setup {
  std::unique_ptr<uc_test::ScratchDirectory> scratch;
  std::filesystem::path store_path;
  std::shared_ptr<UpsControlEngine> engine;
  std::shared_ptr<SyntheticAdapter> adapter;
  UpsId ups;
  std::map<GrantScope, AuthorityRef> grants;
  ControlContext context{ControlEpoch{1}, Incarnation{1}};
  bool ok = false;
  std::string error;
};

EngineOpenOptions open_options_for(const Setup& setup) {
  EngineOpenOptions options;
  options.store.path = setup.store_path;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = false;
  options.adapter = setup.adapter;
  return options;
}

/// Opens the store, revalidates it, registers one unit, records one healthy
/// observation and issues one grant per control scope.
Setup build(const std::string& name) {
  Setup setup;
  setup.scratch = std::make_unique<uc_test::ScratchDirectory>(name);
  setup.store_path = setup.scratch->file("unit.upsstore");
  setup.adapter = std::make_shared<SyntheticAdapter>(SyntheticAdapter::Script{});

  EngineOpenOptions options;
  options.store.path = setup.store_path;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  options.store.created_at = Tick{kCreatedAt};
  options.store.epoch = ControlEpoch{1};
  options.store.incarnation = Incarnation{1};
  options.adapter = setup.adapter;
  const Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(options);
  if (!opened.ok()) {
    setup.error = "the store could not be opened: " + opened.status().to_string();
    return setup;
  }
  setup.engine = opened.value();

  RevalidateRequest revalidate;
  revalidate.authority = setup.context;
  revalidate.now = Tick{kCreatedAt};
  const Status revalidated = setup.engine->revalidate(revalidate).status();
  if (!revalidated.ok()) {
    setup.error = "revalidation failed: " + revalidated.to_string();
    return setup;
  }

  setup.ups = UpsId::parse(kUnitText).value();
  RegisterUpsRequest registration;
  registration.authority = setup.context;
  registration.now = Tick{kCreatedAt};
  registration.id = setup.ups;
  registration.label = "property unit";
  registration.hardware = HardwareGeneration{1};
  registration.lifecycle = LifecycleState::InService;
  registration.operating = OperatingState::OnlineNormal;
  registration.policy.max_evidence_age = TickSpan{600};
  registration.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
  const Result<UpsRecord> registered = setup.engine->register_ups(registration);
  if (!registered.ok()) {
    setup.error = "registration failed: " + registered.status().to_string();
    return setup;
  }
  const Result<ObservationRecord> observed = setup.engine->record_telemetry(RecordTelemetryRequest{
      setup.context,
      UpsRef{registered.value().id, registered.value().hardware, registered.value().revision},
      Tick{kCreatedAt},
      uc_test::healthy_report(registered.value().id, registered.value().hardware, Tick{kCreatedAt},
                              SourceRevision{1}, "property-evidence")});
  if (!observed.ok()) {
    setup.error = "the observation was refused: " + observed.status().to_string();
    return setup;
  }

  const GrantScope scopes[] = {GrantScope::BypassTransfer, GrantScope::MaintenanceEntry,
                               GrantScope::TestExecution, GrantScope::IsolationControl};
  std::size_t index = 0;
  for (const GrantScope scope : scopes) {
    ++index;
    const Result<std::vector<UpsRecord>> current = setup.engine->units();
    const UpsRecord* live_unit = nullptr;
    if (current.ok()) {
      for (const UpsRecord& candidate : current.value()) {
        if (candidate.id == registered.value().id) {
          live_unit = &candidate;
        }
      }
    }
    if (live_unit == nullptr) {
      setup.error = "the registered unit vanished before its grant was issued";
      return setup;
    }
    IssueGrantRequest grant;
    grant.authority = setup.context;
    grant.ref = UpsRef{live_unit->id, live_unit->hardware, live_unit->revision};
    grant.now = Tick{kCreatedAt};
    grant.grant.ref =
        AuthorityRef::parse(std::string(kGrantText) + "-" + std::to_string(index)).value();
    grant.grant.scope = scope;
    grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    grant.grant.issued_at = Tick{kCreatedAt};
    grant.grant.expires_at = Tick{100000000};
    const Result<AuthorityGrant> issued = setup.engine->issue_grant(grant);
    if (!issued.ok()) {
      setup.error = "the grant was refused: " + issued.status().to_string();
      return setup;
    }
    setup.grants[scope] = grant.grant.ref;
  }
  setup.ok = true;
  return setup;
}

Result<UpsRecord> current_unit(const std::shared_ptr<UpsControlEngine>& engine, const UpsId& ups) {
  const Result<std::vector<UpsRecord>> units = engine->units();
  if (!units.ok()) {
    return units.status();
  }
  for (const UpsRecord& unit : units.value()) {
    if (unit.id == ups) {
      return unit;
    }
  }
  return Status::error(StatusCode::NotFound, "the unit is not registered");
}

/// Every documented command, in enumeration order.
std::vector<CommandKind> all_commands() {
  std::vector<CommandKind> commands;
  for (int index = 1; index <= static_cast<int>(CommandKind::ClearFaults); ++index) {
    commands.push_back(static_cast<CommandKind>(index));
  }
  return commands;
}

/// The first documented command whose canonical target is the requested state.
std::optional<CommandKind> command_for_target(OperatingState target) {
  for (const CommandKind kind : all_commands()) {
    const Result<std::optional<OperatingState>> canonical = canonical_target(kind);
    if (canonical.ok() && canonical.value().has_value() && canonical.value().value() == target) {
      return kind;
    }
  }
  return std::nullopt;
}

std::string step_label(std::uint64_t seed, std::size_t step, const std::string& what) {
  return "seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " " + what;
}

struct GraphWalkResult {
  bool ok = true;
  std::string failure;
  std::size_t legal_steps = 0;
  std::size_t illegal_steps = 0;
  std::size_t accepted = 0;
  std::size_t refused = 0;
};

/// A seeded walk over the documented operating transition graph. At each step a
/// random legal successor of the current state is chosen, or, with a small
/// probability, a state that is not a legal successor at all. The corresponding
/// command is submitted through the engine and the legality of the edge is
/// checked against is_legal_operating_transition, which the walk never consults
/// to decide what the engine will do.
GraphWalkResult run_graph_walk(std::uint64_t seed, std::size_t steps) {
  GraphWalkResult result;
  Setup setup = build("property-graph-" + std::to_string(seed));
  if (!setup.ok) {
    result.ok = false;
    result.failure = setup.error;
    return result;
  }
  std::mt19937_64 random(seed);
  std::int64_t tick = kStartTick;
  std::uint64_t source_revision = 2;
  for (std::size_t step = 0; step < steps; ++step) {
    const Result<UpsRecord> unit = current_unit(setup.engine, setup.ups);
    if (!unit.ok()) {
      result.ok = false;
      result.failure = step_label(seed, step, "the unit cannot be read: " + unit.status().to_string());
      return result;
    }
    const OperatingState current = unit.value().operating;
    const std::vector<OperatingState> legal = legal_successors(current);

    std::vector<OperatingState> illegal_targets;
    std::vector<OperatingState> legal_targets;
    for (const OperatingState candidate : legal) {
      if (command_for_target(candidate).has_value()) {
        legal_targets.push_back(candidate);
      }
    }
    for (const CommandKind kind : all_commands()) {
      const Result<std::optional<OperatingState>> canonical = canonical_target(kind);
      if (!canonical.ok() || !canonical.value().has_value()) {
        continue;
      }
      const OperatingState candidate = canonical.value().value();
      if (candidate == current || is_legal_operating_transition(current, candidate)) {
        continue;
      }
      if (std::find(illegal_targets.begin(), illegal_targets.end(), candidate) ==
          illegal_targets.end()) {
        illegal_targets.push_back(candidate);
      }
    }

    const bool want_illegal = (random() % 8u) == 0u && !illegal_targets.empty();
    OperatingState target = current;
    if (want_illegal) {
      target = illegal_targets[static_cast<std::size_t>(random() % illegal_targets.size())];
    } else {
      if (legal_targets.empty()) {
        continue;
      }
      target = legal_targets[static_cast<std::size_t>(random() % legal_targets.size())];
    }
    const std::optional<CommandKind> kind = command_for_target(target);
    if (!kind.has_value()) {
      continue;
    }
    const Result<GrantScope> scope = required_scope(kind.value());
    if (!scope.ok()) {
      continue;
    }
    const auto grant = setup.grants.find(scope.value());
    if (grant == setup.grants.end()) {
      continue;
    }

    // Keep the evidence fresh so that the only blocking finding for a legal edge
    // would be a genuine one.
    RecordTelemetryRequest observation;
    observation.authority = setup.context;
    observation.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
    observation.now = Tick{tick};
    observation.report = uc_test::healthy_report(unit.value().id, unit.value().hardware, Tick{tick},
                                                 SourceRevision{source_revision},
                                                 "graph-observation-" + std::to_string(step));
    observation.report.operating = current;
    const Result<ObservationRecord> recorded = setup.engine->record_telemetry(observation);
    if (!recorded.ok()) {
      result.ok = false;
      result.failure = step_label(seed, step, "the observation was refused: " +
                                                  recorded.status().to_string());
      return result;
    }
    ++source_revision;
    ++tick;

    const Result<UpsRecord> planned = current_unit(setup.engine, setup.ups);
    if (!planned.ok()) {
      result.ok = false;
      result.failure = step_label(seed, step, "the unit cannot be read after the observation");
      return result;
    }
    ControlCommand command;
    command.authority = setup.context;
    command.ref = UpsRef{planned.value().id, planned.value().hardware, planned.value().revision};
    command.now = Tick{tick};
    command.kind = kind.value();
    command.key = IdempotencyKey::parse("graph-" + std::to_string(seed) + "-" + std::to_string(step))
                      .value();
    command.authority_ref = grant->second;

    const bool edge_legal = is_legal_operating_transition(current, target);
    const Result<AttemptRecord> attempt = setup.engine->submit(command);
    if (!attempt.ok()) {
      result.ok = false;
      result.failure = step_label(seed, step, std::string("the submission of ") +
                                                  to_string(kind.value()) + " from " +
                                                  to_string(current) + " to " + to_string(target) +
                                                  " failed outright: " +
                                                  attempt.status().to_string());
      return result;
    }
    const AttemptRecord& record = attempt.value();
    if (record.phase == AttemptPhase::Refused) {
      ++result.refused;
      const RefusalCode code =
          record.refusal.has_value() ? record.refusal->code : RefusalCode::None;
      if (edge_legal) {
        result.ok = false;
        result.failure = step_label(seed, step, std::string("the legal edge ") + to_string(current) +
                                                    " -> " + to_string(target) + " was refused with " +
                                                    to_string(code) + ": " + record.terminal_detail);
        return result;
      }
      if (code != RefusalCode::IllegalOperatingTransition) {
        result.ok = false;
        result.failure = step_label(seed, step, std::string("the illegal edge ") + to_string(current) +
                                                    " -> " + to_string(target) +
                                                    " was refused with " + to_string(code) +
                                                    " instead of illegal_operating_transition");
        return result;
      }
      const Result<UpsRecord> unchanged = current_unit(setup.engine, setup.ups);
      if (!unchanged.ok() || !(unchanged.value().operating == current)) {
        result.ok = false;
        result.failure = step_label(seed, step, "a refused command moved the operating state");
        return result;
      }
    } else if (record.phase == AttemptPhase::Acknowledged || record.phase == AttemptPhase::Issued) {
      ++result.accepted;
      if (!edge_legal) {
        result.ok = false;
        result.failure = step_label(seed, step, std::string("the illegal edge ") + to_string(current) +
                                                    " -> " + to_string(target) + " was accepted");
        return result;
      }
      if (!(record.target == target)) {
        result.ok = false;
        result.failure = step_label(seed, step, std::string("the accepted command ") +
                                                    to_string(kind.value()) + " recorded target " +
                                                    to_string(record.target) + " instead of " +
                                                    to_string(target));
        return result;
      }
      RecordTelemetryRequest effect;
      effect.authority = setup.context;
      effect.ref = UpsRef{planned.value().id, planned.value().hardware, planned.value().revision};
      effect.now = Tick{tick + 1};
      effect.report = uc_test::healthy_report(planned.value().id, planned.value().hardware,
                                              Tick{tick + 1}, SourceRevision{source_revision},
                                              "graph-effect-" + std::to_string(step));
      effect.report.operating = target;
      const Result<ObservationRecord> observed = setup.engine->record_telemetry(effect);
      if (!observed.ok()) {
        result.ok = false;
        result.failure = step_label(seed, step, "the effect observation was refused: " +
                                                    observed.status().to_string());
        return result;
      }
      ++source_revision;
      VerifyRequest verify;
      verify.authority = setup.context;
      verify.now = Tick{tick + 2};
      verify.attempt = record.id;
      const Result<AttemptRecord> verified = setup.engine->verify(verify);
      if (!verified.ok() || verified.value().phase != AttemptPhase::Verified) {
        result.ok = false;
        result.failure = step_label(seed, step, "the accepted command was not verified: " +
                                                    (verified.ok()
                                                         ? std::string(to_string(
                                                               verified.value().phase))
                                                         : verified.status().to_string()));
        return result;
      }
      const Result<UpsRecord> settled = current_unit(setup.engine, setup.ups);
      if (!settled.ok() || !(settled.value().operating == target)) {
        result.ok = false;
        result.failure = step_label(seed, step, "the verified effect did not reach the canonical "
                                                "target");
        return result;
      }
    } else {
      result.ok = false;
      result.failure = step_label(seed, step, std::string("the submission reached phase ") +
                                                  to_string(record.phase));
      return result;
    }
    if (edge_legal) {
      ++result.legal_steps;
    } else {
      ++result.illegal_steps;
    }
    tick += kTickStep;
  }
  (void)setup.engine->close();
  return result;
}

}  // namespace

UC_TEST(property, seeded_walk_over_the_operating_graph_respects_legality) {
  const std::uint64_t seeds[] = {0x5EED0001ull, 0x5EED0002ull, 0x5EED0003ull};
  std::size_t legal_total = 0;
  std::size_t illegal_total = 0;
  std::size_t accepted_total = 0;
  std::size_t refused_total = 0;
  for (const std::uint64_t seed : seeds) {
    const GraphWalkResult result = run_graph_walk(seed, 120);
    UC_CHECK_MSG(result.ok, "seed " + std::to_string(seed) + ": " + result.failure);
    legal_total += result.legal_steps;
    illegal_total += result.illegal_steps;
    accepted_total += result.accepted;
    refused_total += result.refused;
  }
  UC_CHECK_MSG(legal_total > 0, "the walk never exercised a legal edge");
  UC_CHECK_MSG(illegal_total > 0, "the walk never exercised an illegal edge, so it proved nothing "
                                  "about legality");
  UC_CHECK_MSG(accepted_total > 0, "the walk never accepted a command");
  UC_CHECK_MSG(refused_total > 0, "the walk never refused a command");
}

namespace {

// ---------------------------------------------------------------------------
// The randomized mutation walk
// ---------------------------------------------------------------------------

enum class WalkAction : std::size_t {
  Telemetry = 0,
  Policy,
  Lifecycle,
  AdoptState,
  BindObligation,
  ReleaseObligation,
  IssueGrant,
  RevokeGrant,
  Submit,
  Verify,
  Abandon,
  Revalidate,
  Register,
};

constexpr std::size_t kWalkActionCount = 13;

const char* walk_action_name(WalkAction action) {
  switch (action) {
    case WalkAction::Telemetry: return "telemetry";
    case WalkAction::Policy: return "policy";
    case WalkAction::Lifecycle: return "lifecycle";
    case WalkAction::AdoptState: return "adopt_state";
    case WalkAction::BindObligation: return "bind_obligation";
    case WalkAction::ReleaseObligation: return "release_obligation";
    case WalkAction::IssueGrant: return "issue_grant";
    case WalkAction::RevokeGrant: return "revoke_grant";
    case WalkAction::Submit: return "submit";
    case WalkAction::Verify: return "verify";
    case WalkAction::Abandon: return "abandon";
    case WalkAction::Revalidate: return "revalidate";
    case WalkAction::Register: return "register";
  }
  return "unknown";
}

/// The independent model of one protected-load obligation. Nothing in the engine
/// writes it: it is updated only by the walk's own actions, and the store is
/// compared against it after every step.
struct ObligationModel {
  ProtectionRequirement requirement = ProtectionRequirement::MustRemainProtected;
  std::optional<Tick> expires_at;
  bool resolved = false;
  ObligationState outcome = ObligationState::Released;
  std::string unit;

  bool binding(Tick now) const {
    if (resolved || requirement == ProtectionRequirement::Informational) {
      return false;
    }
    return !expires_at.has_value() || now.value() < expires_at->value();
  }
};

/// The model of everything the walk tracks independently of the engine.
struct WalkModel {
  std::map<std::string, std::uint64_t> revisions;
  std::map<std::string, ObligationModel> obligations;
  StoreGeneration generation;
};

struct StepOutcome {
  WalkAction action = WalkAction::Telemetry;
  /// True when the engine reported success, so a durable commit happened.
  bool mutation = false;
  /// The number of durable commits the successful operation performs: a submit
  /// that reaches the adapter commits the plan and then the acknowledgement.
  std::uint64_t commits = 0;
  bool unit_mutation = false;
  bool submitted = false;
  bool accepted_attempt = false;
  bool refused_attempt = false;
  CommandKind kind = CommandKind::EnterStaticBypass;
  RefusalCode refusal = RefusalCode::None;
  AttemptPhase phase = AttemptPhase::Planned;
  StatusCode error = StatusCode::Ok;
  std::string new_unit;
  Status status;
};

struct WalkReport {
  bool ok = true;
  std::string failure;
  std::size_t steps = 0;
  std::size_t committed = 0;
  std::size_t failed = 0;
  std::size_t accepted = 0;
  std::size_t refused = 0;
  std::size_t reopens = 0;
  std::size_t illegal_drops = 0;
  std::uint64_t transcript = 14695981039346656037ull;
};

std::string walk_label(std::uint64_t seed, std::size_t step, WalkAction action,
                       const std::string& detail) {
  return "seed=" + std::to_string(seed) + " step=" + std::to_string(step) + " action=" +
         walk_action_name(action) + ": " + detail;
}

/// What one step is required to have changed, derived from the action the walk
/// took and the result the engine reported.
struct StepExpectation {
  WalkAction action = WalkAction::Telemetry;
  /// The exact number of generations this step must have committed.
  std::uint64_t commits = 0;
  bool unit_mutation = false;
  bool submitted = false;
  bool accepted = false;
  bool refused = false;
  CommandKind kind = CommandKind::EnterStaticBypass;
  RefusalCode refusal = RefusalCode::None;
  Tick now;
  std::string new_unit;
};

/// Checks every invariant the walk claims after one step, and folds the observed
/// state into the model. An empty result means every invariant held.
std::string check_invariants(const Setup& setup, WalkModel& model,
                             const StepExpectation& expected) {
  const Result<StoreAuditReport> audit = setup.engine->store_audit();
  if (!audit.ok()) {
    return "the store could not be audited: " + audit.status().to_string();
  }
  const std::uint64_t before = model.generation.value();
  const std::uint64_t after = audit.value().generation.value();
  if (after != before + expected.commits) {
    return "the step committed " + std::to_string(after - before) +
           " generations where exactly " + std::to_string(expected.commits) +
           " was required (generation " + std::to_string(before) + " to " +
           std::to_string(after) + ")";
  }
  model.generation = audit.value().generation;

  const Result<std::vector<UpsRecord>> units = setup.engine->units();
  if (!units.ok()) {
    return "the units could not be read: " + units.status().to_string();
  }
  for (const UpsRecord& unit : units.value()) {
    const auto found = model.revisions.find(unit.id.value());
    if (found == model.revisions.end()) {
      if (unit.id.value() != expected.new_unit || unit.revision.value() != 1u) {
        return "unit '" + unit.id.value() +
               "' appeared without being registered by this step, or without revision 1";
      }
      model.revisions[unit.id.value()] = unit.revision.value();
      continue;
    }
    const std::uint64_t previous = found->second;
    const std::uint64_t required =
        previous + ((expected.unit_mutation && unit.id == setup.ups) ? 1u : 0u);
    if (unit.revision.value() != required) {
      return "unit '" + unit.id.value() + "' is at revision " +
             std::to_string(unit.revision.value()) + " where the model requires " +
             std::to_string(required);
    }
    if (unit.revision.value() < previous) {
      return "unit '" + unit.id.value() + "' moved its revision backwards";
    }
    model.revisions[unit.id.value()] = unit.revision.value();
  }
  for (const auto& entry : model.revisions) {
    bool present = false;
    for (const UpsRecord& unit : units.value()) {
      if (unit.id.value() == entry.first) {
        present = true;
      }
    }
    if (!present) {
      return "unit '" + entry.first + "' vanished from the store";
    }
  }

  const Result<std::vector<AttemptRecord>> journal = setup.engine->history(HistoryQuery{});
  if (!journal.ok()) {
    return "the journal could not be read: " + journal.status().to_string();
  }
  std::map<std::string, std::vector<AttemptId>> unresolved;
  std::size_t consulted = 0;
  for (const AttemptRecord& attempt : journal.value()) {
    if (attempt.ack != AckOutcome::None) {
      ++consulted;
      if (attempt.phase == AttemptPhase::Planned || attempt.phase == AttemptPhase::Refused) {
        return "an attempt the adapter answered is recorded in phase " +
               std::string(to_string(attempt.phase));
      }
    } else if (attempt.phase == AttemptPhase::Issued ||
               attempt.phase == AttemptPhase::Acknowledged ||
               attempt.phase == AttemptPhase::Observed ||
               attempt.phase == AttemptPhase::Verified) {
      return "an attempt that reached the adapter records no acknowledgement at all";
    }
    if (!is_terminal(attempt.phase)) {
      unresolved[attempt.ups.value()].push_back(attempt.id);
    }
  }
  for (const UpsRecord& unit : units.value()) {
    const auto found = unresolved.find(unit.id.value());
    const std::size_t count = found == unresolved.end() ? 0u : found->second.size();
    if (count > 1u) {
      return "unit '" + unit.id.value() + "' has " + std::to_string(count) +
             " unresolved control attempts at once";
    }
    if (unit.in_flight.has_value()) {
      if (count != 1u || !(found->second.front() == unit.in_flight.value())) {
        return "the in-flight gate of unit '" + unit.id.value() +
               "' does not name its single unresolved attempt";
      }
    } else if (count != 0u) {
      return "unit '" + unit.id.value() +
             "' has an unresolved attempt that does not hold the in-flight gate";
    }
  }

  if (setup.adapter->issue_count() != consulted) {
    return "the adapter issued " + std::to_string(setup.adapter->issue_count()) +
           " commands but " + std::to_string(consulted) +
           " attempts record an adapter acknowledgement";
  }

  for (const UpsRecord& unit : units.value()) {
    for (const ProtectedLoadObligation& obligation : unit.obligations) {
      const auto found = model.obligations.find(obligation.ref.value());
      if (found == model.obligations.end()) {
        return "the store holds obligation '" + obligation.ref.value() +
               "' that the model never bound";
      }
      if (!found->second.unit.empty() && found->second.unit != unit.id.value()) {
        return "obligation '" + obligation.ref.value() + "' moved between units";
      }
      if (found->second.resolved) {
        if (obligation.state != found->second.outcome) {
          return "obligation '" + obligation.ref.value() + "' is " +
                 std::string(to_string(obligation.state)) +
                 " where the explicit release recorded " +
                 std::string(to_string(found->second.outcome));
        }
      } else {
        if (obligation.state != ObligationState::Active) {
          return "obligation '" + obligation.ref.value() +
                 "' stopped asserting protection without an explicit release or suspension";
        }
        if (obligation.protection != found->second.requirement) {
          return "obligation '" + obligation.ref.value() +
                 "' changed its protection requirement without an explicit re-binding";
        }
      }
    }
  }
  for (const auto& entry : model.obligations) {
    bool present = false;
    for (const UpsRecord& unit : units.value()) {
      for (const ProtectedLoadObligation& obligation : unit.obligations) {
        if (obligation.ref.value() == entry.first) {
          present = true;
        }
      }
    }
    if (!present) {
      return "obligation '" + entry.first +
             "' was bound and then dropped without an explicit release";
    }
  }

  // The protection-dropping contract, decided from the independent model.
  if (expected.submitted && drops_protection(expected.kind)) {
    bool any_active_protective = false;
    bool any_binding_protective = false;
    for (const auto& entry : model.obligations) {
      if (entry.second.resolved ||
          entry.second.requirement == ProtectionRequirement::Informational) {
        continue;
      }
      any_active_protective = true;
      if (!entry.second.expires_at.has_value() ||
          expected.now.value() < entry.second.expires_at->value()) {
        any_binding_protective = true;
      }
    }
    if (expected.accepted && any_active_protective) {
      return "a protection-dropping command was accepted while a protective obligation was still "
             "asserting protection";
    }
    if (expected.refused && any_active_protective) {
      const RefusalCode required = any_binding_protective ? RefusalCode::ObligationUnreleased
                                                          : RefusalCode::ObligationExpired;
      // A finding with a lower numeric code legitimately masks the obligation
      // finding; anything else must be exactly the obligation refusal the model
      // predicts, and it must never be a later-precedence code.
      const int code = static_cast<int>(expected.refusal);
      const int obligation_precedence = static_cast<int>(RefusalCode::ObligationUnreleased);
      if (code >= obligation_precedence) {
        if (expected.refusal != required) {
          return std::string("a protection-dropping command was refused with ") +
                 to_string(expected.refusal) + " instead of " + to_string(required) +
                 " while the model still held " +
                 (any_binding_protective ? "a binding" : "a lapsed") +
                 " protective obligation";
        }
      }
    }
  }

  const Result<std::shared_ptr<const UpsState>> state =
      UpsStore::read_file(setup.store_path, StoreReadOptions{});
  if (!state.ok()) {
    return "the durable store could not be read: " + state.status().to_string();
  }
  const Status valid = validate_state(*state.value(), ResourceLimits::defaults());
  if (!valid.ok()) {
    return "the durable store is structurally invalid: " + valid.to_string();
  }
  return std::string();
}

WalkReport run_mutation_walk(std::uint64_t seed, std::size_t steps, bool reopen_in_middle) {
  WalkReport report;
  Setup setup = build("property-walk-" + std::to_string(seed));
  if (!setup.ok) {
    report.ok = false;
    report.failure = setup.error;
    return report;
  }
  std::mt19937_64 random(seed);
  std::int64_t tick = kStartTick;
  std::uint64_t source_revision = 2;
  std::size_t serial = 0;
  std::size_t extra_units = 0;
  WalkModel model;
  const Result<StoreAuditReport> initial_audit = setup.engine->store_audit();
  const Result<std::vector<UpsRecord>> initial_units = setup.engine->units();
  if (!initial_audit.ok() || !initial_units.ok()) {
    report.ok = false;
    report.failure = "the prepared store could not be read";
    return report;
  }
  model.generation = initial_audit.value().generation;
  for (const UpsRecord& unit : initial_units.value()) {
    model.revisions[unit.id.value()] = unit.revision.value();
  }

  const WalkAction table[] = {
      WalkAction::Telemetry,          WalkAction::Telemetry,   WalkAction::Telemetry,
      WalkAction::Telemetry,          WalkAction::Policy,      WalkAction::Lifecycle,
      WalkAction::Lifecycle,          WalkAction::AdoptState,  WalkAction::AdoptState,
      WalkAction::BindObligation,     WalkAction::BindObligation,
      WalkAction::ReleaseObligation,  WalkAction::IssueGrant,  WalkAction::IssueGrant,
      WalkAction::RevokeGrant,        WalkAction::Submit,      WalkAction::Submit,
      WalkAction::Submit,             WalkAction::Verify,      WalkAction::Verify,
      WalkAction::Abandon,            WalkAction::Revalidate,  WalkAction::Register};
  constexpr std::size_t kTableSize = sizeof(table) / sizeof(table[0]);

  for (std::size_t step = 0; step < steps; ++step) {
    WalkAction action = table[random() % kTableSize];
    StepExpectation expected;
    expected.action = action;
    expected.now = Tick{tick};
    StepOutcome outcome;
    outcome.action = action;
    ++serial;

    if (reopen_in_middle && step == steps / 2) {
      // Reopen in the middle of the walk: the independent model must still
      // describe the recovered store, modulo the recovery downgrade of the state
      // basis (which the model does not track) and the revalidation that follows.
      (void)setup.engine->close();
      setup.engine.reset();
      const Result<std::shared_ptr<UpsControlEngine>> reopened =
          UpsControlEngine::open(open_options_for(setup));
      if (!reopened.ok()) {
        report.ok = false;
        report.failure = walk_label(seed, step, action, "the store could not be reopened: " +
                                                            reopened.status().to_string());
        return report;
      }
      setup.engine = reopened.value();
      const std::string recovered = check_invariants(setup, model, expected);
      if (!recovered.empty()) {
        report.ok = false;
        report.failure = walk_label(seed, step, action,
                                    "the recovered store disagrees with the model: " + recovered);
        return report;
      }
      RevalidateRequest revalidate;
      revalidate.authority = setup.context;
      revalidate.now = Tick{tick};
      const Status revalidated = setup.engine->revalidate(revalidate).status();
      if (!revalidated.ok()) {
        report.ok = false;
        report.failure = walk_label(seed, step, action,
                                    "revalidation failed: " + revalidated.to_string());
        return report;
      }
      model.generation = StoreGeneration{model.generation.value() + 1};
      ++report.committed;

      const Result<UpsRecord> recovered_unit = current_unit(setup.engine, setup.ups);
      if (!recovered_unit.ok()) {
        report.ok = false;
        report.failure = walk_label(seed, step, action, "the unit is missing after the reopen");
        return report;
      }
      RecordTelemetryRequest observation;
      observation.authority = setup.context;
      observation.ref = UpsRef{recovered_unit.value().id, recovered_unit.value().hardware,
                               recovered_unit.value().revision};
      observation.now = Tick{tick};
      observation.report = uc_test::healthy_report(recovered_unit.value().id,
                                                   recovered_unit.value().hardware, Tick{tick},
                                                   SourceRevision{source_revision},
                                                   "reopen-observation");
      observation.report.operating = recovered_unit.value().operating;
      const Result<ObservationRecord> recorded = setup.engine->record_telemetry(observation);
      if (!recorded.ok()) {
        report.ok = false;
        report.failure = walk_label(seed, step, action, "the post-reopen observation was refused: " +
                                                            recorded.status().to_string());
        return report;
      }
      ++source_revision;
      StepExpectation after_reopen;
      after_reopen.action = action;
      after_reopen.now = Tick{tick};
      after_reopen.commits = 1;
      after_reopen.unit_mutation = true;
      const std::string mismatch = check_invariants(setup, model, after_reopen);
      if (!mismatch.empty()) {
        report.ok = false;
        report.failure = walk_label(seed, step, action,
                                    "the post-reopen observation broke an invariant: " + mismatch);
        return report;
      }
      report.transcript = mix(report.transcript, 0x1234u);
      ++report.committed;
      ++report.reopens;
      ++report.steps;
      tick += kTickStep;
      continue;
    }

    const Result<UpsRecord> unit = current_unit(setup.engine, setup.ups);
    if (!unit.ok()) {
      report.ok = false;
      report.failure = walk_label(seed, step, action, "the unit cannot be read");
      return report;
    }

    if (action == WalkAction::Register && extra_units >= 2) {
      action = WalkAction::Telemetry;
      expected.action = action;
      outcome.action = action;
    }

    if (action == WalkAction::Telemetry) {
      RecordTelemetryRequest observation;
      observation.authority = setup.context;
      observation.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
      observation.now = Tick{tick};
      observation.report = uc_test::healthy_report(unit.value().id, unit.value().hardware, Tick{tick},
                                                   SourceRevision{source_revision},
                                                   "walk-observation-" + std::to_string(serial));
      observation.report.operating = unit.value().operating;
      const Result<ObservationRecord> recorded = setup.engine->record_telemetry(observation);
      outcome.mutation = recorded.ok();
      outcome.unit_mutation = recorded.ok();
      outcome.error = recorded.status().code();
      ++source_revision;
    } else if (action == WalkAction::Policy) {
      SetReservePolicyRequest policy;
      policy.authority = setup.context;
      policy.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
      policy.now = Tick{tick};
      policy.policy.max_evidence_age = TickSpan{600};
      policy.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
      const Result<UpsRecord> changed = setup.engine->set_reserve_policy(policy);
      outcome.mutation = changed.ok();
      outcome.unit_mutation = changed.ok();
      outcome.error = changed.status().code();
    } else if (action == WalkAction::Lifecycle) {
      std::vector<LifecycleState> candidates;
      for (int index = 1; index <= static_cast<int>(LifecycleState::Decommissioned); ++index) {
        const auto candidate = static_cast<LifecycleState>(index);
        if (candidate == unit.value().lifecycle ||
            candidate == LifecycleState::Decommissioned ||
            !is_legal_lifecycle_transition(unit.value().lifecycle, candidate) ||
            !is_consistent(candidate, unit.value().operating)) {
          continue;
        }
        candidates.push_back(candidate);
      }
      if (!candidates.empty()) {
        SetLifecycleRequest change;
        change.authority = setup.context;
        change.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
        change.now = Tick{tick};
        change.lifecycle = candidates[static_cast<std::size_t>(random() % candidates.size())];
        change.reason = "walk lifecycle change";
        const Result<UpsRecord> changed = setup.engine->set_lifecycle(change);
        outcome.mutation = changed.ok();
        outcome.unit_mutation = changed.ok();
        outcome.error = changed.status().code();
      }
    } else if (action == WalkAction::AdoptState) {
      std::vector<OperatingState> candidates;
      for (int index = 1; index <= static_cast<int>(OperatingState::Retired); ++index) {
        const auto candidate = static_cast<OperatingState>(index);
        if (candidate == OperatingState::Retired ||
            !is_consistent(unit.value().lifecycle, candidate)) {
          continue;
        }
        candidates.push_back(candidate);
      }
      if (!candidates.empty()) {
        AdoptOperatingStateRequest adoption;
        adoption.authority = setup.context;
        adoption.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
        adoption.now = Tick{tick};
        adoption.operating = candidates[static_cast<std::size_t>(random() % candidates.size())];
        adoption.reason = "walk state adoption";
        const Result<UpsRecord> adopted = setup.engine->adopt_operating_state(adoption);
        outcome.mutation = adopted.ok();
        outcome.unit_mutation = adopted.ok();
        outcome.error = adopted.status().code();
      }
    } else if (action == WalkAction::BindObligation) {
      BindObligationRequest bind;
      bind.authority = setup.context;
      bind.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
      bind.now = Tick{tick};
      bind.obligation.ref =
          ObligationRef::parse("obligation-" + std::to_string(serial)).value();
      bind.obligation.load = LoadId::parse("load-" + std::to_string(serial)).value();
      bind.obligation.tier = ObligationTier::Critical;
      const unsigned roll = static_cast<unsigned>(random() % 10u);
      bind.obligation.protection = roll == 0u ? ProtectionRequirement::Informational
                                              : (roll <= 2u
                                                     ? ProtectionRequirement::MayBeInterruptedWithAuthority
                                                     : ProtectionRequirement::MustRemainProtected);
      bind.obligation.asserted_by = AuthorityRef::parse(kLoadAuthorityText).value();
      bind.obligation.epoch = setup.context.epoch;
      bind.obligation.incarnation = setup.context.incarnation;
      bind.obligation.asserted_at = Tick{tick};
      if ((random() % 2u) == 0u) {
        bind.obligation.expires_at = Tick{tick + 2000};
      }
      const Result<ProtectedLoadObligation> bound = setup.engine->bind_obligation(bind);
      outcome.mutation = bound.ok();
      outcome.unit_mutation = bound.ok();
      outcome.error = bound.status().code();
      if (bound.ok()) {
        ObligationModel entry;
        entry.requirement = bound.value().protection;
        entry.expires_at = bound.value().expires_at;
        entry.unit = unit.value().id.value();
        model.obligations[bound.value().ref.value()] = entry;
      }
    } else if (action == WalkAction::ReleaseObligation) {
      std::vector<std::string> candidates;
      for (const auto& entry : model.obligations) {
        if (!entry.second.resolved) {
          candidates.push_back(entry.first);
        }
      }
      if (!candidates.empty()) {
        const std::string reference =
            candidates[static_cast<std::size_t>(random() % candidates.size())];
        const Result<ObligationRef> parsed = ObligationRef::parse(reference);
        const ProtectedLoadObligation* binding =
            parsed.ok() ? find_obligation(unit.value(), parsed.value()) : nullptr;
        if (binding != nullptr) {
          ReleaseObligationRequest release;
          release.authority = setup.context;
          release.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
          release.now = Tick{tick};
          release.obligation = binding->ref;
          release.obligation_revision = binding->revision;
          release.release_authority = AuthorityRef::parse("release-authority").value();
          release.reason = "walk release";
          release.outcome = (random() % 3u) == 0u ? ObligationState::Suspended
                                                 : ObligationState::Released;
          const Result<ProtectedLoadObligation> released = setup.engine->release_obligation(release);
          outcome.mutation = released.ok();
          outcome.unit_mutation = released.ok();
          outcome.error = released.status().code();
          if (released.ok()) {
            model.obligations[reference].resolved = true;
            model.obligations[reference].outcome = released.value().state;
          }
        }
      }
    } else if (action == WalkAction::IssueGrant) {
      IssueGrantRequest grant;
      grant.authority = setup.context;
      grant.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
      grant.now = Tick{tick};
      grant.grant.ref = AuthorityRef::parse("walk-grant-" + std::to_string(serial)).value();
      grant.grant.scope = static_cast<GrantScope>(1u + (random() % 8u));
      grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
      grant.grant.issued_at = Tick{tick};
      grant.grant.expires_at = Tick{tick + 5000};
      const Result<AuthorityGrant> issued = setup.engine->issue_grant(grant);
      outcome.mutation = issued.ok();
      outcome.unit_mutation = issued.ok();
      outcome.error = issued.status().code();
    } else if (action == WalkAction::RevokeGrant) {
      std::vector<AuthorityRef> candidates;
      for (const AuthorityGrant& grant : unit.value().grants) {
        if (!grant.revoked_at.has_value() && grant.ref.value().rfind("walk-grant-", 0) == 0) {
          candidates.push_back(grant.ref);
        }
      }
      if (!candidates.empty()) {
        RevokeGrantRequest revoke;
        revoke.authority = setup.context;
        revoke.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
        revoke.now = Tick{tick};
        revoke.grant = candidates[static_cast<std::size_t>(random() % candidates.size())];
        revoke.revocation_authority = AuthorityRef::parse("walk-revoker").value();
        revoke.reason = "walk revocation";
        const Result<AuthorityGrant> revoked = setup.engine->revoke_grant(revoke);
        outcome.mutation = revoked.ok();
        outcome.unit_mutation = revoked.ok();
        outcome.error = revoked.status().code();
      }
    } else if (action == WalkAction::Submit) {
      std::vector<CommandKind> candidates;
      for (const OperatingState successor : legal_successors(unit.value().operating)) {
        const std::optional<CommandKind> candidate = command_for_target(successor);
        if (!candidate.has_value()) {
          continue;
        }
        const Result<GrantScope> scope = required_scope(candidate.value());
        if (!scope.ok() || setup.grants.find(scope.value()) == setup.grants.end()) {
          continue;
        }
        candidates.push_back(candidate.value());
      }
      if (!candidates.empty()) {
        const CommandKind kind = candidates[static_cast<std::size_t>(random() % candidates.size())];
        ControlCommand command;
        command.authority = setup.context;
        command.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
        command.now = Tick{tick};
        command.kind = kind;
        command.key =
            IdempotencyKey::parse("walk-key-" + std::to_string(serial)).value();
        command.authority_ref = setup.grants[required_scope(kind).value()];
        const Result<AttemptRecord> attempt = setup.engine->submit(command);
        outcome.mutation = attempt.ok();
        outcome.error = attempt.status().code();
        expected.submitted = true;
        expected.kind = kind;
        if (attempt.ok()) {
          outcome.phase = attempt.value().phase;
          if (attempt.value().phase == AttemptPhase::Refused) {
            expected.refused = true;
            expected.refusal = attempt.value().refusal.has_value() ? attempt.value().refusal->code
                                                                  : RefusalCode::None;
          } else if (attempt.value().phase == AttemptPhase::Acknowledged ||
                     attempt.value().phase == AttemptPhase::Issued) {
            expected.accepted = true;
          }
        }
      }
    } else if (action == WalkAction::Verify) {
      if (unit.value().in_flight.has_value()) {
        VerifyRequest verify;
        verify.authority = setup.context;
        verify.now = Tick{tick};
        verify.attempt = unit.value().in_flight.value();
        const Result<AttemptRecord> verified = setup.engine->verify(verify);
        outcome.mutation = verified.ok();
        outcome.error = verified.status().code();
        if (verified.ok()) {
          outcome.phase = verified.value().phase;
        }
      }
    } else if (action == WalkAction::Abandon) {
      if (unit.value().in_flight.has_value()) {
        AbandonRequest abandon;
        abandon.authority = setup.context;
        abandon.now = Tick{tick};
        abandon.attempt = unit.value().in_flight.value();
        abandon.reason = "walk abandonment";
        const Result<AttemptRecord> abandoned = setup.engine->abandon(abandon);
        outcome.mutation = abandoned.ok();
        outcome.error = abandoned.status().code();
        if (abandoned.ok()) {
          outcome.phase = abandoned.value().phase;
        }
      }
    } else if (action == WalkAction::Revalidate) {
      RevalidateRequest revalidate;
      revalidate.authority = setup.context;
      revalidate.now = Tick{tick};
      const Result<RevalidationReport> revalidated = setup.engine->revalidate(revalidate);
      outcome.mutation = revalidated.ok();
      outcome.error = revalidated.status().code();
    } else if (action == WalkAction::Register) {
      ++extra_units;
      RegisterUpsRequest registration;
      registration.authority = setup.context;
      registration.now = Tick{tick};
      registration.id =
          UpsId::parse("property-unit-" + std::to_string(extra_units)).value();
      registration.label = "walk unit " + std::to_string(extra_units);
      registration.hardware = HardwareGeneration{1};
      registration.lifecycle = LifecycleState::Standby;
      registration.operating = OperatingState::OnlineNormal;
      registration.policy.max_evidence_age = TickSpan{600};
      registration.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
      const Result<UpsRecord> registered = setup.engine->register_ups(registration);
      outcome.mutation = registered.ok();
      outcome.error = registered.status().code();
      if (registered.ok()) {
        expected.new_unit = registered.value().id.value();
      }
    }

    if (outcome.mutation) {
      // A submission that reached the adapter commits its durable plan and then
      // its acknowledgement; every other successful operation commits once.
      outcome.commits =
          (action == WalkAction::Submit && expected.accepted) ? std::uint64_t{2} : std::uint64_t{1};
    }
    expected.commits = outcome.commits;
    expected.unit_mutation = outcome.unit_mutation;
    const std::string failure = check_invariants(setup, model, expected);
    if (!failure.empty()) {
      report.ok = false;
      report.failure = walk_label(seed, step, action, failure);
      return report;
    }

    report.transcript = mix(report.transcript, static_cast<std::uint64_t>(action));
    report.transcript = mix(report.transcript, outcome.mutation ? 1u : 0u);
    report.transcript = mix(report.transcript, static_cast<std::uint64_t>(outcome.phase));
    report.transcript = mix(report.transcript, static_cast<std::uint64_t>(expected.refusal));
    report.transcript = mix(report.transcript, static_cast<std::uint64_t>(outcome.error));
    if (outcome.mutation) {
      ++report.committed;
    } else {
      ++report.failed;
    }
    if (expected.accepted) {
      ++report.accepted;
    }
    if (expected.refused) {
      ++report.refused;
    }
    ++report.steps;
    tick += kTickStep;
  }
  (void)setup.engine->close();
  return report;
}

}  // namespace

UC_TEST(property, seeded_mutation_walk_holds_every_invariant) {
  const std::uint64_t seeds[] = {0xC0FFEE01ull, 0xC0FFEE02ull, 0xC0FFEE03ull};
  for (const std::uint64_t seed : seeds) {
    const WalkReport report = run_mutation_walk(seed, 200, true);
    UC_CHECK_MSG(report.ok, report.failure);
    UC_CHECK_MSG(report.steps >= 200, "seed " + std::to_string(seed) + ": the walk ran " +
                                          std::to_string(report.steps) + " steps");
    UC_CHECK_MSG(report.reopens == 1,
                 "seed " + std::to_string(seed) + ": the walk reopened the store " +
                     std::to_string(report.reopens) + " times");
    if (seed == seeds[0]) {
      UC_CHECK_MSG(report.committed > 0, "the walk never committed a mutation");
      UC_CHECK_MSG(report.failed > 0, "the walk never exercised a refused operation");
      UC_CHECK_MSG(report.accepted > 0, "the walk never accepted a control attempt");
    }
  }
}

UC_TEST(property, the_same_seed_reproduces_the_same_transcript) {
  const std::uint64_t seed = 0xC0FFEE01ull;
  const WalkReport first = run_mutation_walk(seed, 200, true);
  const WalkReport second = run_mutation_walk(seed, 200, true);
  UC_CHECK_MSG(first.ok, first.failure);
  UC_CHECK_MSG(second.ok, second.failure);
  UC_CHECK_MSG(first.transcript == second.transcript,
               "seed " + std::to_string(seed) + " produced transcript " +
                   std::to_string(first.transcript) + " and then " +
                   std::to_string(second.transcript));
  UC_CHECK_MSG(first.accepted == second.accepted && first.refused == second.refused &&
                   first.committed == second.committed && first.failed == second.failed,
               "seed " + std::to_string(seed) +
                   " produced different accept/refuse counts on the two runs: " +
                   std::to_string(first.accepted) + "/" + std::to_string(first.refused) + " and " +
                   std::to_string(second.accepted) + "/" + std::to_string(second.refused));
  // A different seed must produce a different transcript, or the digest proves
  // nothing about determinism.
  const WalkReport other = run_mutation_walk(0xC0FFEE02ull, 200, true);
  UC_CHECK_MSG(other.ok, other.failure);
  UC_CHECK_MSG(other.transcript != first.transcript,
               "two different seeds produced the same transcript");
}



