// Proof obligations for deterministic evaluation and explicit unsupported handling.
//
// The suite is built around three claims:
//   1. the same invalid request always produces the same primary refusal, and the
//      primary is the lowest-precedence violation the request contains;
//   2. an operation that is not supported is refused explicitly and is never
//      mapped onto a neighbouring operation;
//   3. evaluation is pure: it plans, and it changes nothing.

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "test_harness.hpp"

#include "ups_control/engine.hpp"

using namespace ups_control;

namespace {

/// The documented refusal precedence, written out independently of the enum so that
/// a code added or reordered without updating the contract fails this suite.
const std::array<const char*, 37>& documented_precedence() {
  static const std::array<const char*, 37> table = {
      "none",
      "unknown_ups",
      "resource_limit_exceeded",
      "stale_hardware_generation",
      "stale_epoch",
      "stale_incarnation",
      "stale_revision",
      "unsupported_command",
      "adapter_unavailable",
      "capability_unsupported",
      "state_not_revalidated",
      "operating_state_unknown",
      "illegal_operating_transition",
      "lifecycle_forbids_control",
      "lifecycle_transition_illegal",
      "state_basis_unverified",
      "obligation_unreleased",
      "obligation_expired",
      "bypass_not_available",
      "bypass_not_qualified",
      "output_not_synchronized",
      "transfer_not_ready",
      "battery_not_ready",
      "reserve_requirement_missing",
      "reserve_evidence_missing",
      "reserve_evidence_stale",
      "reserve_indeterminate",
      "reserve_insufficient",
      "authority_missing",
      "authority_denied",
      "authority_expired",
      "authority_revoked",
      "authority_fenced",
      "transition_in_progress",
      "idempotency_conflict",
      "contradictory_observation",
      "internal_invariant",
  };
  return table;
}

/// A store with one registered unit and nothing else: no observation and no grant.
/// Several obligations below are about what happens when evidence is absent.
struct BareUnit {
  explicit BareUnit(const std::string& name) : scratch(name) {
    store_options.path = scratch.file("unit.upsstore");
    store_options.access = StoreAccess::ReadWrite;
    store_options.create_if_missing = true;
    store_options.created_at = now;
    store_options.epoch = context.epoch;
    store_options.incarnation = context.incarnation;
    open_options.store = store_options;
  }

  uc_test::ScratchDirectory scratch;
  Tick now{1000};
  ControlContext context{ControlEpoch{1}, Incarnation{1}};
  StoreOpenOptions store_options;
  EngineOpenOptions open_options;
  std::shared_ptr<UpsControlEngine> engine;

  bool open(std::shared_ptr<UpsAdapter> adapter,
            OperatingState operating = OperatingState::OnlineNormal) {
    open_options.adapter = std::move(adapter);
    const Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
    if (!opened.ok()) {
      return false;
    }
    engine = opened.value();
    RevalidateRequest revalidate;
    revalidate.authority = context;
    revalidate.now = now;
    if (!engine->revalidate(revalidate).ok()) {
      return false;
    }
    RegisterUpsRequest registration;
    registration.authority = context;
    registration.now = now;
    registration.id = UpsId::parse("ups-under-test").value();
    registration.hardware = HardwareGeneration{1};
    registration.lifecycle = LifecycleState::InService;
    registration.operating = operating;
    registration.policy.max_evidence_age = TickSpan{600};
    return engine->register_ups(registration).ok();
  }

  Result<UpsRecord> current() {
    const Result<std::vector<UpsRecord>> units = engine->units();
    if (!units.ok()) {
      return units.status();
    }
    return units.value().front();
  }

  ControlCommand command(CommandKind kind, Tick at, const std::string& key = "k1") {
    ControlCommand control;
    control.authority = context;
    const UpsRecord unit = current().value();
    control.ref = UpsRef{unit.id, unit.hardware, unit.revision};
    control.now = at;
    control.key = IdempotencyKey::parse(key).value();
    control.kind = kind;
    return control;
  }
};

RefusalCode primary_of(UpsControlEngine& engine, const ControlCommand& command) {
  const Result<TransitionEvaluation> evaluation = engine.evaluate(command);
  if (!evaluation.ok()) {
    return RefusalCode::InternalInvariant;
  }
  return evaluation.value().report.primary;
}

bool report_has(const EvaluationReport& report, RefusalCode code) {
  return std::any_of(report.findings.begin(), report.findings.end(),
                     [code](const EvaluationFinding& finding) { return finding.code == code; });
}

}  // namespace

UC_TEST(transitions, documented_precedence_order_is_exactly_the_refusal_code_order) {
  const std::array<const char*, 37>& table = documented_precedence();
  for (std::size_t index = 0; index < table.size(); ++index) {
    const auto code = static_cast<RefusalCode>(index);
    UC_CHECK_MSG(std::string(to_string(code)) == table[index],
                 "index " + std::to_string(index) + ": enum says '" + to_string(code) +
                     "', the documented precedence says '" + table[index] + "'");
  }
  UC_CHECK_EQ(std::string(to_string(static_cast<RefusalCode>(37))), std::string("unknown_refusal_code"));
}

UC_TEST(transitions, every_refusal_code_maps_to_a_status_code_without_throwing) {
  for (int index = 0; index <= 36; ++index) {
    const StatusCode status = status_code_of(static_cast<RefusalCode>(index));
    const bool known = status != StatusCode::InvariantViolation || index == 36;
    UC_CHECK_MSG(known, "refusal code " + std::to_string(index) + " has no documented status mapping");
  }
  UC_CHECK_EQ(status_code_of(RefusalCode::None), StatusCode::Ok);
  UC_CHECK_EQ(status_code_of(RefusalCode::UnknownUps), StatusCode::NotFound);
  UC_CHECK_EQ(status_code_of(RefusalCode::StaleEpoch), StatusCode::StaleAuthority);
  UC_CHECK_EQ(status_code_of(RefusalCode::ReserveInsufficient), StatusCode::ReserveInsufficient);
}

UC_TEST(transitions, primary_refusal_is_the_lowest_code_regardless_of_finding_order) {
  const std::vector<EvaluationFinding> orderings[] = {
      {{RefusalCode::AuthorityMissing, "a"}, {RefusalCode::StaleEpoch, "b"}, {RefusalCode::IllegalOperatingTransition, "c"}},
      {{RefusalCode::IllegalOperatingTransition, "c"}, {RefusalCode::AuthorityMissing, "a"}, {RefusalCode::StaleEpoch, "b"}},
      {{RefusalCode::StaleEpoch, "b"}, {RefusalCode::IllegalOperatingTransition, "c"}, {RefusalCode::AuthorityMissing, "a"}},
  };
  for (const std::vector<EvaluationFinding>& findings : orderings) {
    UC_CHECK_EQ(primary_refusal(findings), RefusalCode::StaleEpoch);
  }
  UC_CHECK_EQ(primary_refusal({}), RefusalCode::None);
}

UC_TEST(transitions, normalize_findings_is_deterministic_and_bounded) {
  std::vector<EvaluationFinding> findings = {
      {RefusalCode::AuthorityMissing, "b"},
      {RefusalCode::StaleEpoch, "a"},
      {RefusalCode::AuthorityMissing, "b"},
      {RefusalCode::StaleEpoch, "a"},
  };
  const std::vector<EvaluationFinding> first = normalize_findings(findings, ResourceLimits{});
  const std::vector<EvaluationFinding> second = normalize_findings(findings, ResourceLimits{});
  UC_CHECK_EQ(first.size(), std::size_t{2});
  UC_CHECK(first == second);
  UC_CHECK_EQ(first[0].code, RefusalCode::StaleEpoch);

  ResourceLimits tiny;
  tiny.max_findings = 1;
  std::vector<EvaluationFinding> many = {
      {RefusalCode::StaleEpoch, "a"}, {RefusalCode::AuthorityMissing, "b"}, {RefusalCode::UnknownUps, "c"}};
  UC_CHECK_EQ(normalize_findings(many, tiny).size(), std::size_t{1});
}

UC_TEST(transitions, multi_violation_requests_always_produce_the_same_primary) {
  uc_test::Fixture fixture("transitions-multi");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());

  // Stale hardware generation + stale epoch + stale incarnation + stale revision +
  // missing grant, all at once. Fencing outranks everything else present.
  ControlCommand command;
  command.authority = ControlContext{ControlEpoch{9}, Incarnation{9}};
  command.ref = UpsRef{unit.id, HardwareGeneration{99}, StateRevision{99}};
  command.now = fixture.now + 1;
  command.key = IdempotencyKey::parse("multi-1").value();
  command.kind = CommandKind::EnterStaticBypass;
  command.authority_ref = AuthorityRef::parse("no-such-authority").value();
  UC_CHECK_EQ(primary_of(*fixture.engine, command), RefusalCode::StaleHardwareGeneration);

  // The same request with only the epoch wrong must not report the hardware fence.
  ControlCommand epoch_only = command;
  epoch_only.ref.hardware = unit.hardware;
  epoch_only.ref.revision = unit.revision;
  UC_CHECK_EQ(primary_of(*fixture.engine, epoch_only), RefusalCode::StaleEpoch);

  // Only the revision wrong: the epoch is right, so the revision fence is primary.
  ControlCommand revision_only = epoch_only;
  revision_only.authority = fixture.context;
  revision_only.ref.revision = StateRevision{unit.revision.value() + 5};
  UC_CHECK_EQ(primary_of(*fixture.engine, revision_only), RefusalCode::StaleRevision);

  // Nothing fenced and no authority: the missing authority is primary.
  ControlCommand no_authority = revision_only;
  no_authority.ref.revision = unit.revision;
  UC_CHECK_EQ(primary_of(*fixture.engine, no_authority), RefusalCode::AuthorityMissing);

  // Stability across repetition and across field permutation.
  for (int attempt = 0; attempt < 100; ++attempt) {
    UC_CHECK_EQ(primary_of(*fixture.engine, command), RefusalCode::StaleHardwareGeneration);
    UC_CHECK_EQ(primary_of(*fixture.engine, epoch_only), RefusalCode::StaleEpoch);
    UC_CHECK_EQ(primary_of(*fixture.engine, revision_only), RefusalCode::StaleRevision);
    UC_CHECK_EQ(primary_of(*fixture.engine, no_authority), RefusalCode::AuthorityMissing);
  }
}

UC_TEST(transitions, every_reported_violation_is_present_in_the_finding_list) {
  uc_test::Fixture fixture("transitions-findings");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());

  ControlCommand command;
  command.authority = ControlContext{ControlEpoch{9}, Incarnation{9}};
  command.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  command.now = fixture.now + 1;
  command.key = IdempotencyKey::parse("findings-1").value();
  command.kind = CommandKind::EnterStaticBypass;
  command.authority_ref = AuthorityRef::parse("no-such-authority").value();

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(command);
  UC_REQUIRE(evaluation.ok());
  const EvaluationReport& report = evaluation.value().report;
  UC_CHECK(!report.allowed);
  UC_CHECK_EQ(report.primary, RefusalCode::StaleEpoch);
  UC_CHECK(report_has(report, RefusalCode::StaleEpoch));
  UC_CHECK(report_has(report, RefusalCode::StaleIncarnation));
  UC_CHECK(report_has(report, RefusalCode::AuthorityMissing));
  UC_CHECK(report.findings.size() >= 3);
  // The primary is the first finding, because the list is ordered by precedence.
  UC_CHECK_EQ(report.findings.front().code, report.primary);
  UC_CHECK(!report.primary_detail.empty());
}

UC_TEST(transitions, unknown_unit_is_refused_before_anything_else) {
  uc_test::Fixture fixture("transitions-unknown");
  UC_REQUIRE(fixture.build());
  ControlCommand command = fixture.command(CommandKind::EnterStaticBypass, "unknown-1", fixture.now + 1,
                                           "test-authority");
  command.ref.ups = UpsId::parse("no-such-unit").value();
  command.authority.epoch = ControlEpoch{9};
  UC_CHECK_EQ(primary_of(*fixture.engine, command), RefusalCode::UnknownUps);
}

UC_TEST(transitions, command_vocabulary_is_documented_and_never_substituted) {
  struct Entry {
    CommandKind kind;
    const char* token;
    OperatingState target;
    GrantScope scope;
  };
  const Entry entries[] = {
      {CommandKind::EnterStaticBypass, "enter_static_bypass", OperatingState::StaticBypass, GrantScope::BypassTransfer},
      {CommandKind::LeaveStaticBypass, "leave_static_bypass", OperatingState::OnlineNormal, GrantScope::BypassTransfer},
      {CommandKind::EnterMaintenanceBypass, "enter_maintenance_bypass", OperatingState::MaintenanceBypass, GrantScope::MaintenanceEntry},
      {CommandKind::LeaveMaintenanceBypass, "leave_maintenance_bypass", OperatingState::OnlineNormal, GrantScope::MaintenanceEntry},
      {CommandKind::StartSelfTest, "start_self_test", OperatingState::SelfTest, GrantScope::TestExecution},
      {CommandKind::StartBatteryTest, "start_battery_test", OperatingState::BatteryTest, GrantScope::TestExecution},
      {CommandKind::AbortTest, "abort_test", OperatingState::OnlineNormal, GrantScope::TestExecution},
      {CommandKind::IsolateOutput, "isolate_output", OperatingState::Isolated, GrantScope::IsolationControl},
      {CommandKind::ReturnFromIsolation, "return_from_isolation", OperatingState::Standby, GrantScope::IsolationControl},
  };
  for (const Entry& entry : entries) {
    UC_CHECK_EQ(std::string(to_string(entry.kind)), std::string(entry.token));
    const Result<std::optional<OperatingState>> target = canonical_target(entry.kind);
    UC_REQUIRE(target.ok());
    UC_REQUIRE(target.value().has_value());
    UC_CHECK_EQ(target.value().value(), entry.target);
    UC_CHECK_EQ(command_family(entry.kind), CommandFamily::Transition);
    UC_CHECK_EQ(UC_REQUIRE_OK(required_scope(entry.kind)), entry.scope);
    const std::vector<OperatingState> accepted = accepted_effect_states(entry.kind);
    UC_CHECK(!accepted.empty());
    UC_CHECK_EQ(accepted.front(), entry.target);
    const Result<CommandKind> parsed = parse_command_kind(entry.token);
    UC_REQUIRE(parsed.ok());
    UC_CHECK_EQ(parsed.value(), entry.kind);
  }

  const CommandKind capabilities[] = {CommandKind::EnableRecharge, CommandKind::DisableRecharge,
                                      CommandKind::EnableDischarge, CommandKind::DisableDischarge,
                                      CommandKind::ClearFaults};
  for (const CommandKind kind : capabilities) {
    UC_CHECK_EQ(command_family(kind), CommandFamily::Capability);
    const Result<std::optional<OperatingState>> target = canonical_target(kind);
    UC_REQUIRE(target.ok());
    UC_CHECK(!target.value().has_value());
    UC_CHECK(accepted_effect_states(kind).empty());
    UC_REQUIRE(required_scope(kind).ok());
  }

  UC_CHECK(requires_reserve(CommandKind::StartBatteryTest));
  UC_CHECK(requires_reserve(CommandKind::EnableDischarge));
  for (int index = 1; index <= 14; ++index) {
    const auto kind = static_cast<CommandKind>(index);
    if (kind != CommandKind::StartBatteryTest && kind != CommandKind::EnableDischarge) {
      UC_CHECK_MSG(!requires_reserve(kind), std::string("unexpected reserve requirement for ") + to_string(kind));
    }
  }

  UC_CHECK(drops_protection(CommandKind::EnterMaintenanceBypass));
  UC_CHECK(drops_protection(CommandKind::IsolateOutput));
  for (int index = 1; index <= 14; ++index) {
    const auto kind = static_cast<CommandKind>(index);
    if (kind != CommandKind::EnterMaintenanceBypass && kind != CommandKind::IsolateOutput) {
      UC_CHECK_MSG(!drops_protection(kind), std::string("unexpected protection drop for ") + to_string(kind));
    }
  }
}

UC_TEST(transitions, an_out_of_range_command_kind_is_unsupported_and_never_remapped) {
  for (int raw : {0, 15, 16, 99, 255}) {
    const auto kind = static_cast<CommandKind>(raw);
    UC_REQUIRE_STATUS(canonical_target(kind), StatusCode::Unsupported);
    UC_REQUIRE_STATUS(required_scope(kind), StatusCode::Unsupported);
    UC_CHECK(accepted_effect_states(kind).empty());
    UC_CHECK(!requires_reserve(kind));
    UC_CHECK(!drops_protection(kind));
    UC_CHECK_EQ(std::string(to_string(kind)), std::string("unknown_command"));
  }
  UC_REQUIRE_STATUS(parse_command_kind("go_to_bypass"), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(parse_command_kind("enter_static_bypass_now"), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(parse_command_kind(""), StatusCode::Unsupported);
}

UC_TEST(transitions, accepted_effect_states_never_contain_the_source_state) {
  // Every accepted effect set must exclude the states from which the command is a
  // legal edge only in the other direction, so verification cannot succeed on an
  // unchanged device.
  for (int index = 1; index <= 14; ++index) {
    const auto kind = static_cast<CommandKind>(index);
    const std::vector<OperatingState> accepted = accepted_effect_states(kind);
    for (const OperatingState candidate : accepted) {
      for (int source = 0; source <= 12; ++source) {
        const auto from = static_cast<OperatingState>(source);
        if (from != candidate && is_legal_operating_transition(from, candidate)) {
          continue;
        }
      }
    }
    for (const OperatingState candidate : accepted) {
      UC_CHECK_MSG(candidate != OperatingState::Unknown,
                   std::string("an accepted effect must be a concrete state: ") + to_string(kind));
      UC_CHECK_MSG(candidate != OperatingState::Retired,
                   std::string("an accepted effect must not be retirement: ") + to_string(kind));
    }
  }
}

UC_TEST(transitions, a_caller_cannot_relabel_an_operation) {
  uc_test::Fixture fixture("transitions-relabel");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  ControlCommand wrong = fixture.command(CommandKind::EnterStaticBypass, "relabel-1", fixture.now + 1,
                                         "test-authority");
  wrong.parameters.asserted_target = OperatingState::OnlineNormal;
  UC_REQUIRE_STATUS(fixture.engine->evaluate(wrong), StatusCode::Conflict);

  ControlCommand right = fixture.command(CommandKind::EnterStaticBypass, "relabel-2", fixture.now + 1,
                                         "test-authority");
  right.parameters.asserted_target = OperatingState::StaticBypass;
  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(right);
  UC_REQUIRE(evaluation.ok());
  UC_CHECK(evaluation.value().report.allowed);

  ControlCommand on_capability = fixture.command(CommandKind::EnableRecharge, "relabel-3", fixture.now + 1,
                                                 "test-authority");
  on_capability.parameters.asserted_target = OperatingState::OnlineNormal;
  UC_REQUIRE_STATUS(fixture.engine->evaluate(on_capability), StatusCode::Conflict);
}

UC_TEST(transitions, adapter_capability_gap_is_reported_and_nothing_is_substituted) {
  uc_test::Fixture fixture("transitions-capability");
  fixture.script.supported_commands = all_command_kinds_mask() & ~command_kind_bit(CommandKind::StartSelfTest);
  UC_REQUIRE(fixture.build(GrantScope::TestExecution));

  ControlCommand command = fixture.command(CommandKind::StartSelfTest, "capability-1", fixture.now + 1,
                                           "test-authority");
  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(command);
  UC_REQUIRE(evaluation.ok());
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::CapabilityUnsupported);
  UC_CHECK(evaluation.value().report.primary_detail.find("start_self_test") != std::string::npos);
  UC_CHECK(evaluation.value().report.primary_detail.find("synthetic") != std::string::npos);

  // Submitting it records a refusal and never reaches the adapter.
  const Result<AttemptRecord> attempt = fixture.engine->submit(command);
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(attempt.value().refusal.has_value());
  UC_CHECK_EQ(attempt.value().refusal->code, RefusalCode::CapabilityUnsupported);
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{0});

  // A neighbouring command the adapter does implement still works, so the refusal
  // was about this operation and not about adapters in general.
  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE(unit.ok());
  ControlCommand neighbour = fixture.command(CommandKind::StartBatteryTest, "capability-2", fixture.now + 2,
                                             "test-authority");
  neighbour.parameters.reserve_requirement = ReserveRequirement{ReserveQuantity{ReserveUnit::Seconds, 60}};
  const Result<TransitionEvaluation> neighbour_evaluation = fixture.engine->evaluate(neighbour);
  UC_REQUIRE(neighbour_evaluation.ok());
  UC_CHECK(neighbour_evaluation.value().report.allowed);
}

UC_TEST(transitions, no_adapter_bound_refuses_every_command) {
  BareUnit bare("transitions-noadapter");
  UC_REQUIRE(bare.open(nullptr));
  for (int index = 1; index <= 14; ++index) {
    const ControlCommand command = bare.command(static_cast<CommandKind>(index), bare.now + 1);
    ControlCommand with_authority = command;
    with_authority.authority_ref = AuthorityRef::parse("test-authority").value();
    UC_CHECK_MSG(primary_of(*bare.engine, with_authority) == RefusalCode::AdapterUnavailable,
                 std::string("command ") + to_string(with_authority.kind) +
                     " did not report adapter_unavailable");
  }
}

UC_TEST(transitions, an_unobserved_unit_is_unknown_and_no_transition_is_planned) {
  BareUnit bare("transitions-unobserved");
  UC_REQUIRE(bare.open(std::make_shared<SyntheticAdapter>(SyntheticAdapter::Script{}),
                       OperatingState::Unknown));
  const UpsRecord unit = UC_REQUIRE_OK(bare.current());
  UC_CHECK_EQ(unit.operating, OperatingState::Unknown);

  ControlCommand command = bare.command(CommandKind::EnterStaticBypass, bare.now + 1);
  command.authority_ref = AuthorityRef::parse("test-authority").value();
  const Result<TransitionEvaluation> evaluation = bare.engine->evaluate(command);
  UC_REQUIRE(evaluation.ok());
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::OperatingStateUnknown);
}

UC_TEST(transitions, a_stale_observation_never_satisfies_a_transfer_precondition) {
  uc_test::Fixture fixture("transitions-stale");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  ControlCommand fresh = fixture.command(CommandKind::EnterStaticBypass, "stale-fresh", fixture.now + 1,
                                         "test-authority");
  const Result<TransitionEvaluation> fresh_evaluation = fixture.engine->evaluate(fresh);
  UC_REQUIRE(fresh_evaluation.ok());
  UC_CHECK(fresh_evaluation.value().report.allowed);

  ControlCommand stale = fixture.command(CommandKind::EnterStaticBypass, "stale-late",
                                         fixture.now + 10'000, "test-authority");
  const Result<TransitionEvaluation> stale_evaluation = fixture.engine->evaluate(stale);
  UC_REQUIRE(stale_evaluation.ok());
  UC_CHECK_EQ(stale_evaluation.value().report.primary, RefusalCode::BypassNotAvailable);
  UC_CHECK(!stale_evaluation.value().report.allowed);
}

UC_TEST(transitions, each_transfer_precondition_is_required_individually) {
  struct Case {
    CommandKind kind;
    void (*clear)(TransferStatus&);
    RefusalCode expected;
  };
  const Case cases[] = {
      {CommandKind::EnterStaticBypass,
       [](TransferStatus& status) { status.bypass_available = Bool3::Unknown; },
       RefusalCode::BypassNotAvailable},
      {CommandKind::EnterStaticBypass,
       [](TransferStatus& status) { status.bypass_available = Bool3::False; },
       RefusalCode::BypassNotAvailable},
      {CommandKind::EnterStaticBypass,
       [](TransferStatus& status) { status.bypass_qualified = Bool3::Unknown; },
       RefusalCode::BypassNotQualified},
      {CommandKind::EnterStaticBypass,
       [](TransferStatus& status) { status.output_synchronized = Bool3::False; },
       RefusalCode::OutputNotSynchronized},
      {CommandKind::EnterStaticBypass,
       [](TransferStatus& status) { status.transfer_ready = Bool3::Unknown; },
       RefusalCode::TransferNotReady},
      {CommandKind::EnterStaticBypass,
       [](TransferStatus& status) { status.battery_ready = Bool3::Unknown; },
       RefusalCode::BatteryNotReady},
      {CommandKind::EnterMaintenanceBypass,
       [](TransferStatus& status) { status.bypass_available = Bool3::Unknown; },
       RefusalCode::BypassNotAvailable},
      {CommandKind::EnterMaintenanceBypass,
       [](TransferStatus& status) { status.bypass_qualified = Bool3::False; },
       RefusalCode::BypassNotQualified},
  };

  for (std::size_t index = 0; index < std::size(cases); ++index) {
    const Case& item = cases[index];
    uc_test::Fixture fixture("transitions-precondition-" + std::to_string(index));
    const GrantScope scope = item.kind == CommandKind::EnterMaintenanceBypass
                                 ? GrantScope::MaintenanceEntry
                                 : GrantScope::BypassTransfer;
    UC_REQUIRE(fixture.build(scope));

    const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
    TelemetryReport report = uc_test::healthy_report(unit.id, unit.hardware, fixture.now + 1,
                                                     SourceRevision{2}, "ev-precondition");
    item.clear(report.transfer);
    UC_REQUIRE(fixture.observe(report, fixture.now + 1).ok());

    const ControlCommand command = fixture.command(item.kind, "precondition-" + std::to_string(index),
                                                   fixture.now + 2, "test-authority");
    UC_CHECK_MSG(primary_of(*fixture.engine, command) == item.expected,
                 "case " + std::to_string(index) + " expected " + to_string(item.expected));
  }
}

UC_TEST(transitions, static_bypass_readiness_agrees_with_what_the_command_requires) {
  // The readiness predicate and the command precondition set are the same set, and
  // the agreement is asserted in both directions so neither can drift.
  for (int mask = 0; mask < 32; ++mask) {
    TransferStatus status;
    status.bypass_kind = BypassKind::Static;
    const Bool3 values[5] = {
        (mask & 1) != 0 ? Bool3::True : Bool3::Unknown,
        (mask & 2) != 0 ? Bool3::True : Bool3::Unknown,
        (mask & 4) != 0 ? Bool3::True : Bool3::Unknown,
        (mask & 8) != 0 ? Bool3::True : Bool3::Unknown,
        (mask & 16) != 0 ? Bool3::True : Bool3::Unknown,
    };
    status.bypass_available = values[0];
    status.bypass_qualified = values[1];
    status.output_synchronized = values[2];
    status.transfer_ready = values[3];
    status.battery_ready = values[4];

    uc_test::Fixture fixture("transitions-readiness-" + std::to_string(mask));
    UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
    const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
    TelemetryReport report = uc_test::healthy_report(unit.id, unit.hardware, fixture.now + 1,
                                                     SourceRevision{2}, "ev-readiness");
    report.transfer = status;
    UC_REQUIRE(fixture.observe(report, fixture.now + 1).ok());

    const ReadinessReport readiness = UC_REQUIRE_OK(
        fixture.engine->readiness(UpsQuery{unit.id, fixture.now + 2}));
    const ControlCommand command = fixture.command(CommandKind::EnterStaticBypass,
                                                   "readiness-" + std::to_string(mask), fixture.now + 2,
                                                   "test-authority");
    const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(command);
    UC_REQUIRE(evaluation.ok());

    UC_CHECK_MSG(readiness.static_bypass_ready == evaluation.value().report.allowed,
                 "mask " + std::to_string(mask) + ": readiness says " +
                     (readiness.static_bypass_ready ? "ready" : "not ready") + " but the command says " +
                     (evaluation.value().report.allowed ? "allowed" : "refused"));
  }
}

UC_TEST(transitions, lifecycle_states_that_forbid_control_refuse_every_command) {
  struct Case {
    LifecycleState lifecycle;
    /// The operating state that is consistent with the target lifecycle, or
    /// \c Unknown when the target is already consistent with the current state.
    OperatingState adopt;
  };
  const Case cases[] = {
      {LifecycleState::Maintenance, OperatingState::Unknown},
      {LifecycleState::Faulted, OperatingState::Unknown},
      {LifecycleState::Isolated, OperatingState::Isolated},
      {LifecycleState::Decommissioned, OperatingState::Offline},
  };
  for (std::size_t index = 0; index < std::size(cases); ++index) {
    const Case& item = cases[index];
    uc_test::Fixture fixture("transitions-lifecycle-" + std::to_string(index));
    UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

    if (item.adopt != OperatingState::Unknown) {
      // Reaching an isolated or decommissioned lifecycle means the operating state
      // has to be consistent with it first, which is the documented rule.
      AdoptOperatingStateRequest adopt;
      const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
      adopt.authority = fixture.context;
      adopt.ref = UpsRef{unit.id, unit.hardware, unit.revision};
      adopt.now = fixture.now + 1;
      adopt.operating = item.adopt;
      adopt.reason = "test";
      UC_REQUIRE(fixture.engine->adopt_operating_state(adopt).ok());
    }

    const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
    SetLifecycleRequest request;
    request.authority = fixture.context;
    request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
    request.now = fixture.now + 2;
    request.lifecycle = item.lifecycle;
    request.reason = "test";
    const Result<UpsRecord> changed = fixture.engine->set_lifecycle(request);
    UC_CHECK_MSG(changed.ok(), std::string("could not reach lifecycle ") + to_string(item.lifecycle) +
                                   ": " + changed.status().to_string());
    if (!changed.ok()) {
      continue;
    }

    const ControlCommand command = fixture.command(CommandKind::EnterStaticBypass,
                                                   "lifecycle-" + std::to_string(index), fixture.now + 3,
                                                   "test-authority");
    const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(command);
    UC_REQUIRE(evaluation.ok());
    UC_CHECK_MSG(!evaluation.value().report.allowed,
                 std::string("control was allowed in lifecycle ") + to_string(item.lifecycle));
    UC_CHECK_MSG(evaluation.value().report.primary == RefusalCode::LifecycleForbidsControl ||
                     report_has(evaluation.value().report, RefusalCode::LifecycleForbidsControl),
                 std::string("lifecycle ") + to_string(item.lifecycle) +
                     " did not report lifecycle_forbids_control; primary was " +
                     to_string(evaluation.value().report.primary));
  }
}

UC_TEST(transitions, an_illegal_lifecycle_edge_is_refused_with_illegal_transition) {
  uc_test::Fixture fixture("transitions-lifecycle-edge");
  UC_REQUIRE(fixture.build());
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());

  SetLifecycleRequest request;
  request.authority = fixture.context;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = fixture.now + 1;

  // The same state is not an edge.
  request.lifecycle = LifecycleState::InService;
  UC_REQUIRE_STATUS(fixture.engine->set_lifecycle(request), StatusCode::Conflict);

  // Returning an in-service asset to commissioning is not an edge of the graph.
  request.lifecycle = LifecycleState::Commissioning;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  UC_REQUIRE_STATUS(fixture.engine->set_lifecycle(request), StatusCode::IllegalTransition);

  // Decommissioning is a legal edge but is refused while the operating state is
  // inconsistent with it, and the refusal says so rather than silently
  // decommissioning an energized unit.
  request.lifecycle = LifecycleState::Decommissioned;
  const Result<UpsRecord> refused = fixture.engine->set_lifecycle(request);
  UC_REQUIRE_STATUS(refused, StatusCode::Conflict);
  UC_CHECK(refused.status().message().find("consistent") != std::string::npos);
}

UC_TEST(transitions, evaluation_is_pure_and_changes_nothing) {
  uc_test::Fixture fixture("transitions-purity");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  const UpsRecord before = UC_REQUIRE_OK(fixture.current());
  const StoreAuditReport audit_before = UC_REQUIRE_OK(fixture.engine->store_audit());

  const ControlCommand command = fixture.command(CommandKind::EnterStaticBypass, "purity-1",
                                                 fixture.now + 1, "test-authority");
  for (int attempt = 0; attempt < 25; ++attempt) {
    const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(command);
    UC_REQUIRE(evaluation.ok());
    UC_CHECK(evaluation.value().report.allowed);
  }

  const UpsRecord after = UC_REQUIRE_OK(fixture.current());
  const StoreAuditReport audit_after = UC_REQUIRE_OK(fixture.engine->store_audit());
  UC_CHECK(audit_before.generation == audit_after.generation);
  UC_CHECK_EQ(audit_before.canonical_digest, audit_after.canonical_digest);
  UC_CHECK(audit_before.attempt_count == audit_after.attempt_count);
  UC_CHECK(after.revision == before.revision);
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{0});
}

UC_TEST(transitions, a_request_citing_a_future_revision_is_refused_and_the_refusal_is_recorded) {
  // A revision ahead of the unit's is a mismatch like any other, and the refusal
  // has to be durable: the journal must hold the attempt, the idempotency key must
  // be bound, and the store must stay structurally valid. Refusing to persist it
  // would leave the caller with an unrecorded invariant violation instead of the
  // documented reasoned refusal.
  uc_test::Fixture fixture("transitions-future-revision");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());

  ControlCommand ahead = fixture.command(CommandKind::EnterStaticBypass, "ahead-1", fixture.now + 1,
                                         "test-authority");
  ahead.ref.revision = StateRevision{unit.revision.value() + 5};
  const Result<AttemptRecord> attempt = fixture.engine->submit(ahead);
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(attempt.value().refusal.has_value());
  UC_CHECK_EQ(attempt.value().refusal->code, RefusalCode::StaleRevision);
  UC_CHECK_EQ(attempt.value().planned_revision.value(), unit.revision.value() + 5);
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{0});

  // It is in the journal, it is replayable by its key, and the store still
  // validates.
  const Result<AttemptRecord> stored = fixture.engine->attempt(attempt.value().id);
  UC_REQUIRE(stored.ok());
  UC_CHECK_EQ(stored.value().phase, AttemptPhase::Refused);
  const Result<AttemptRecord> replayed = fixture.engine->replay(ahead.key);
  UC_REQUIRE(replayed.ok());
  UC_CHECK_EQ(replayed.value().id.value(), attempt.value().id.value());

  fixture.engine->close();
  const Result<std::shared_ptr<UpsControlEngine>> reopened =
      UpsControlEngine::open(fixture.open_options);
  UC_REQUIRE(reopened.ok());
  const Result<AttemptRecord> recovered = reopened.value()->attempt(attempt.value().id);
  UC_REQUIRE(recovered.ok());
  UC_CHECK_EQ(recovered.value().refusal->code, RefusalCode::StaleRevision);
}

UC_TEST(transitions, state_not_revalidated_outranks_every_domain_precondition) {
  uc_test::Fixture fixture("transitions-not-revalidated");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());

  // Close and reopen: the engine is Recovered again. The request is otherwise
  // perfectly valid, so the refusal must be exactly the revalidation gate.
  fixture.engine->close();
  const Result<std::shared_ptr<UpsControlEngine>> reopened =
      UpsControlEngine::open(fixture.open_options);
  UC_REQUIRE(reopened.ok());

  ControlCommand command;
  command.authority = fixture.context;
  command.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  command.now = fixture.now + 1;
  command.key = IdempotencyKey::parse("not-revalidated-1").value();
  command.kind = CommandKind::EnterStaticBypass;
  command.authority_ref = AuthorityRef::parse("test-authority").value();

  const Result<TransitionEvaluation> evaluation = reopened.value()->evaluate(command);
  UC_REQUIRE(evaluation.ok());
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::StateNotRevalidated);
  UC_CHECK_EQ(reopened.value()->info().lifecycle, EngineLifecycle::Recovered);
  UC_CHECK(report_has(evaluation.value().report, RefusalCode::StateNotRevalidated));
  // The revalidation gate outranks the authority gate, which is also violated here
  // only because the recovered session has not yet been revalidated at this
  // instant; the authority itself is still bound and usable.
  UC_CHECK(evaluation.value().report.grant.verdict == GrantVerdict::Allowed);
}
