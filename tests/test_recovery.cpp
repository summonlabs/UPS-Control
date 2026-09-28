// Recovery proof obligations: recovery must never make anything fresh again.
//
// Each test closes a real store, reopens it through the real open path, and
// proves that what comes back is the committed fact and not a refreshed one:
// the engine is Recovered, the evidence keeps its original instant and ages, an
// unresolved attempt is never reissued, and a pure read-only recovery changes
// nothing at all.

#include "test_harness.hpp"

#include "fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "ups_control/engine.hpp"
#include "ups_control/state.hpp"
#include "ups_control/store.hpp"

using namespace ups_control;

namespace {

std::vector<std::byte> read_all(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> data((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes;
  bytes.reserve(data.size());
  for (const char value : data) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }
  return bytes;
}

std::string hex16(std::uint64_t value) {
  static const char* digits = "0123456789abcdef";
  std::string text(16, '0');
  for (int index = 0; index < 16; ++index) {
    const unsigned shift = static_cast<unsigned>(60 - 4 * index);
    text[static_cast<std::size_t>(index)] =
        digits[static_cast<std::size_t>((value >> shift) & 0xFull)];
  }
  return text;
}

std::filesystem::path payload_path_for(const std::filesystem::path& store,
                                       StoreGeneration generation) {
  std::filesystem::path result = store;
  std::string name = path_to_utf8(store.filename());
  name += ".g" + hex16(generation.value());
  result.replace_filename(std::filesystem::path(std::u8string(name.begin(), name.end())));
  return result;
}

/// Reopens the fixture's store through the engine, exactly as a restarted
/// process would.
Result<std::shared_ptr<UpsControlEngine>> reopen_engine(const uc_test::Fixture& fixture) {
  EngineOpenOptions options;
  options.store.path = fixture.store_options.path;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = false;
  options.adapter = fixture.adapter;
  return UpsControlEngine::open(options);
}

Status revalidate_at(const std::shared_ptr<UpsControlEngine>& engine, const ControlContext& context,
                     Tick now) {
  RevalidateRequest request;
  request.authority = context;
  request.now = now;
  return engine->revalidate(request).status();
}

/// True when the findings mention a recovered operating state that no
/// post-recovery telemetry has confirmed.
bool has_unconfirmed_recovery_warning(const EvaluationReport& report) {
  for (const EvaluationFinding& finding : report.warnings) {
    if (finding.detail.find("recovered from the store") != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

UC_TEST(recovery, freshly_opened_engine_is_recovered_and_refuses_every_evaluation) {
  uc_test::Fixture fixture("recovery-lifecycle");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));
  UC_CHECK_EQ(fixture.engine->info().lifecycle, EngineLifecycle::Revalidated);
  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE_OK(unit);
  const UpsId ups = unit.value().id;
  // Built while the fixture engine is still open: the helper reads the current
  // unit revision through that engine.
  const ControlCommand command = fixture.command(CommandKind::EnterStaticBypass, "recovery-key",
                                                 Tick{1100}, "test-authority");
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<UpsControlEngine>> engine = reopen_engine(fixture);
  UC_REQUIRE_OK(engine);
  UC_CHECK_EQ(engine.value()->info().lifecycle, EngineLifecycle::Recovered);

  // Every evaluation is refused while the recovered state is unconfirmed.
  const Result<TransitionEvaluation> evaluated = engine.value()->evaluate(command);
  UC_REQUIRE_OK(evaluated);
  UC_CHECK(!evaluated.value().report.allowed);
  UC_CHECK_EQ(evaluated.value().report.primary, RefusalCode::StateNotRevalidated);
  UC_CHECK_EQ(evaluated.value().report.revalidated, false);
  UC_CHECK_MSG(evaluated.value().report.primary_detail.find("not revalidated") != std::string::npos,
               "the refusal must say the store was not revalidated: " +
                   evaluated.value().report.primary_detail);

  // A submit records the refusal instead of issuing anything.
  const Result<AttemptRecord> refused = engine.value()->submit(command);
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_CHECK(refused.value().refusal.has_value());
  if (refused.value().refusal.has_value()) {
    UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::StateNotRevalidated);
  }
  UC_CHECK_EQ(engine.value()->info().lifecycle, EngineLifecycle::Recovered);

  // Revalidation moves the engine forward and reports what it found.
  RevalidateRequest request;
  request.authority = fixture.context;
  request.now = Tick{1200};
  const Result<RevalidationReport> report = engine.value()->revalidate(request);
  UC_REQUIRE_OK(report);
  UC_CHECK_EQ(report.value().now, Tick{1200});
  UC_CHECK_EQ(report.value().unit_count, std::size_t{1});
  UC_CHECK_EQ(report.value().units.size(), std::size_t{1});
  UC_CHECK_EQ(report.value().unresolved_attempts, std::size_t{0});
  UC_CHECK_EQ(report.value().units_with_fresh_reserve, std::size_t{1});
  UC_CHECK_EQ(report.value().units_with_stale_reserve, std::size_t{0});
  UC_CHECK_EQ(report.value().units_with_unknown_reserve, std::size_t{0});
  if (!report.value().units.empty()) {
    UC_CHECK_EQ(report.value().units.front().ups, ups);
    UC_CHECK_EQ(report.value().units.front().reserve_freshness, FreshnessVerdict::Fresh);
    UC_CHECK_EQ(report.value().units.front().reserve_outcome, ReserveOutcome::Sufficient);
  }
  UC_CHECK_EQ(engine.value()->info().lifecycle, EngineLifecycle::Revalidated);
  UC_CHECK_EQ(engine.value()->info().revalidated_at, Tick{1200});

  // The same request is now decided on its merits rather than by the recovery
  // barrier.
  const Result<TransitionEvaluation> after = engine.value()->evaluate(command);
  UC_REQUIRE_OK(after);
  UC_CHECK(after.value().report.revalidated);
  UC_CHECK_MSG(after.value().report.primary != RefusalCode::StateNotRevalidated,
               "a revalidated store must not be refused for being unrevalidated");
  UC_CHECK(after.value().report.allowed);
  UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);
}

UC_TEST(recovery, forces_every_unit_basis_to_recovered) {
  uc_test::Fixture fixture("recovery-basis");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));

  // Establish a verified control effect, so the basis before the close is
  // anything but Recovered.
  AttemptId attempt;
  {
    const Result<AttemptRecord> submitted =
        fixture.submit(CommandKind::EnterStaticBypass, "basis-key", Tick{1100}, "test-authority");
    UC_REQUIRE_OK(submitted);
    attempt = submitted.value().id;
  }
  {
    const Result<UpsRecord> unit = fixture.current();
    UC_REQUIRE_OK(unit);
    TelemetryReport report = uc_test::healthy_report(unit.value().id, unit.value().hardware,
                                                     Tick{1200}, SourceRevision{2}, "ev-2");
    report.operating = OperatingState::StaticBypass;
    UC_REQUIRE_STATUS(fixture.observe(report, Tick{1200}), StatusCode::Ok);
  }
  {
    const Result<AttemptRecord> verified = fixture.verify(attempt, Tick{1300});
    UC_REQUIRE_OK(verified);
    UC_CHECK_EQ(verified.value().phase, AttemptPhase::Verified);
  }
  {
    const Result<UpsRecord> unit = fixture.current();
    UC_REQUIRE_OK(unit);
    UC_CHECK_EQ(unit.value().basis, StateBasis::Verified);
    UC_CHECK_EQ(unit.value().operating, OperatingState::StaticBypass);
  }
  const UpsId ups = fixture.current().value().id;
  const ControlCommand command = fixture.command(CommandKind::LeaveStaticBypass, "basis-warn",
                                                 Tick{1400}, "test-authority");
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<UpsControlEngine>> engine = reopen_engine(fixture);
  UC_REQUIRE_OK(engine);
  UC_REQUIRE_STATUS(revalidate_at(engine.value(), fixture.context, Tick{1400}), StatusCode::Ok);

  // Every view the session can report must present the basis as Recovered: the
  // pre-restart claim is not carried across the reopen.
  {
    const Result<std::vector<UpsRecord>> units = engine.value()->units();
    UC_REQUIRE_OK(units);
    if (units.value().size() != 1) {
      UC_CHECK_MSG(false, "recovery must return exactly the one registered unit");
      return;
    }
    UC_CHECK_MSG(units.value().front().basis == StateBasis::Recovered,
                 "recovery must report every unit basis as Recovered, got " +
                     std::string(to_string(units.value().front().basis)));
    UC_CHECK_EQ(units.value().front().basis, StateBasis::Recovered);
  }
  const UpsQuery query{ups, Tick{1400}};
  const Result<StatusReport> status = engine.value()->status(query);
  UC_REQUIRE_OK(status);
  UC_CHECK_MSG(status.value().basis == StateBasis::Recovered,
               "a recovered status view must report the basis as Recovered, got " +
                   std::string(to_string(status.value().basis)));
  const Result<ReadinessReport> readiness = engine.value()->readiness(query);
  UC_REQUIRE_OK(readiness);
  UC_CHECK_MSG(readiness.value().basis == StateBasis::Recovered,
               "a recovered readiness view must report the basis as Recovered, got " +
                   std::string(to_string(readiness.value().basis)));

  // The evaluation must say that the recovered operating state is not confirmed
  // by anything after the reopen.
  const Result<TransitionEvaluation> evaluated = engine.value()->evaluate(command);
  UC_REQUIRE_OK(evaluated);
  UC_CHECK_EQ(evaluated.value().report.basis, StateBasis::Recovered);
  UC_CHECK_MSG(has_unconfirmed_recovery_warning(evaluated.value().report),
               "an evaluation of a recovered unit must warn that the state is unconfirmed");

  // The downgrade lasts only until this session establishes the basis itself:
  // telemetry recorded after the reopen confirms the unit again.
  {
    const Result<std::vector<UpsRecord>> units = engine.value()->units();
    UC_REQUIRE_OK(units);
    if (units.value().size() != 1) {
      UC_CHECK_MSG(false, "recovery must return exactly the one registered unit");
      return;
    }
    TelemetryReport report = uc_test::healthy_report(ups, units.value().front().hardware, Tick{1450},
                                                     SourceRevision{3}, "ev-post-recovery");
    report.operating = OperatingState::StaticBypass;
    RecordTelemetryRequest request;
    request.authority = fixture.context;
    request.ref = UpsRef{ups, units.value().front().hardware, units.value().front().revision};
    request.now = Tick{1450};
    request.report = report;
    UC_REQUIRE_STATUS(engine.value()->record_telemetry(request), StatusCode::Ok);
  }
  const Result<StatusReport> confirmed = engine.value()->status(UpsQuery{ups, Tick{1450}});
  UC_REQUIRE_OK(confirmed);
  UC_CHECK_EQ(confirmed.value().basis, StateBasis::Observed);
  const Result<TransitionEvaluation> reconfirmed = engine.value()->evaluate(
      ControlCommand{fixture.context,
                     UpsRef{ups, confirmed.value().hardware, confirmed.value().revision},
                     Tick{1450},
                     IdempotencyKey::parse("basis-warn-2").value(),
                     CommandKind::LeaveStaticBypass,
                     CommandParameters{},
                     AuthorityRef::parse("test-authority").value()});
  UC_REQUIRE_OK(reconfirmed);
  UC_CHECK_EQ(reconfirmed.value().report.basis, StateBasis::Observed);
  UC_CHECK_MSG(!has_unconfirmed_recovery_warning(reconfirmed.value().report),
               "a basis established in this session must not be reported as unconfirmed");
  UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);
}

UC_TEST(recovery, recovered_evidence_ages_and_keeps_its_original_instant) {
  uc_test::Fixture fixture("recovery-aging");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));

  const Result<std::vector<UpsRecord>> before = fixture.engine->units();
  UC_REQUIRE_OK(before);
  if (before.value().size() != 1 || !before.value().front().observation.has_value()) {
    UC_CHECK_MSG(false, "the fixture must expose exactly one observed unit");
    return;
  }
  const ObservationRecord original = before.value().front().observation.value();
  UC_CHECK_EQ(original.observed_at, Tick{1000});
  UC_CHECK_EQ(original.revision, SourceRevision{1});
  const UpsId ups = before.value().front().id;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<UpsControlEngine>> engine = reopen_engine(fixture);
  UC_REQUIRE_OK(engine);

  // The freshness window is 600 ticks from the instant the evidence was
  // observed, not from the instant the store was reopened.
  const Tick late{1601};
  RevalidateRequest request;
  request.authority = fixture.context;
  request.now = late;
  const Result<RevalidationReport> report = engine.value()->revalidate(request);
  UC_REQUIRE_OK(report);
  if (report.value().units.size() != 1) {
    UC_CHECK_MSG(false, "revalidation must report exactly one unit");
    return;
  }
  UC_CHECK_EQ(report.value().units.front().reserve_freshness, FreshnessVerdict::Stale);
  UC_CHECK_EQ(report.value().units.front().reserve_outcome, ReserveOutcome::Indeterminate);
  UC_CHECK_MSG(report.value().units.front().detail.find("1000") != std::string::npos,
               "the revalidation detail must carry the original observation instant: " +
                   report.value().units.front().detail);

  const UpsQuery query{ups, late};
  const Result<ReserveAssessment> assessment = engine.value()->battery(query);
  UC_REQUIRE_OK(assessment);
  UC_CHECK_EQ(assessment.value().outcome, ReserveOutcome::Indeterminate);
  UC_CHECK_EQ(assessment.value().reason, ReserveIndeterminacy::EvidenceStale);
  UC_CHECK_EQ(assessment.value().freshness, FreshnessVerdict::Stale);
  UC_CHECK_EQ(assessment.value().observed_at, Tick{1000});
  UC_CHECK_EQ(assessment.value().age, TickSpan{601});
  UC_CHECK_MSG(assessment.value().detail.find("1000") != std::string::npos,
               "the refusal detail must carry the original observation instant so an auditor can "
               "see why the evidence is stale: " +
                   assessment.value().detail);

  // The recovered record still holds the original instant byte for byte.
  const Result<std::vector<UpsRecord>> units = engine.value()->units();
  UC_REQUIRE_OK(units);
  if (units.value().size() != 1 || !units.value().front().observation.has_value()) {
    UC_CHECK_MSG(false, "the recovered unit must still carry its observation");
    return;
  }
  UC_CHECK_MSG(units.value().front().observation.value() == original,
               "recovery must not rewrite the recorded observation");
  UC_CHECK_EQ(units.value().front().observation->observed_at, original.observed_at);
  UC_CHECK_EQ(units.value().front().observation->received_at, original.received_at);
  UC_CHECK_EQ(units.value().front().observation->evidence, original.evidence);
  UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);
}

UC_TEST(recovery, revalidation_does_not_resurrect_stale_reserve_evidence) {
  uc_test::Fixture fixture("recovery-reserve");
  UC_REQUIRE(fixture.build(GrantScope::TestExecution, ReserveUnit::Seconds, 600));

  // While the evidence is fresh the reserve dependent command is allowed.
  const ControlCommand fresh =
      fixture.command(CommandKind::StartBatteryTest, "battery-test-key", Tick{1100}, "test-authority");
  const ControlCommand stale =
      fixture.command(CommandKind::StartBatteryTest, "battery-test-key", Tick{1601}, "test-authority");
  const Result<TransitionEvaluation> before = fixture.engine->evaluate(fresh);
  UC_REQUIRE_OK(before);
  UC_CHECK_MSG(before.value().report.allowed,
               "a fresh reserve must satisfy the command before the restart: " +
                   before.value().report.primary_detail);
  UC_CHECK(before.value().report.reserve.has_value());
  if (before.value().report.reserve.has_value()) {
    UC_CHECK_EQ(before.value().report.reserve->outcome, ReserveOutcome::Sufficient);
  }
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<UpsControlEngine>> engine = reopen_engine(fixture);
  UC_REQUIRE_OK(engine);

  const Tick late{1601};
  RevalidateRequest request;
  request.authority = fixture.context;
  request.now = late;
  const Result<RevalidationReport> report = engine.value()->revalidate(request);
  UC_REQUIRE_OK(report);

  // Revalidation reports the unit as stale; it does not renew the evidence.
  if (report.value().units.size() != 1) {
    UC_CHECK_MSG(false, "revalidation must report exactly one unit");
    return;
  }
  UC_CHECK_EQ(report.value().units.front().reserve_freshness, FreshnessVerdict::Stale);
  UC_CHECK_EQ(report.value().units.front().reserve_outcome, ReserveOutcome::Indeterminate);
  // Staleness is a statement about the evidence, not about the reserve, so the
  // unit is counted as stale rather than as unknown, and it is certainly not
  // counted as fresh.
  UC_CHECK_EQ(report.value().units_with_stale_reserve, std::size_t{1});
  UC_CHECK_EQ(report.value().units_with_unknown_reserve, std::size_t{0});
  UC_CHECK_EQ(report.value().units_with_insufficient_reserve, std::size_t{0});
  UC_CHECK_EQ(report.value().units_with_fresh_reserve, std::size_t{0});

  const Result<TransitionEvaluation> after = engine.value()->evaluate(stale);
  UC_REQUIRE_OK(after);
  UC_CHECK_MSG(!after.value().report.allowed,
               "revalidation must not resurrect reserve evidence that has aged out");
  UC_CHECK_EQ(after.value().report.primary, RefusalCode::ReserveEvidenceStale);
  UC_REQUIRE(after.value().report.reserve.has_value());
  UC_CHECK_EQ(after.value().report.reserve->reason, ReserveIndeterminacy::EvidenceStale);

  // Submitting it leaves a refused attempt and issues nothing.
  const Result<AttemptRecord> submitted = engine.value()->submit(stale);
  UC_REQUIRE_OK(submitted);
  UC_CHECK_EQ(submitted.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(submitted.value().refusal.has_value());
  UC_CHECK_EQ(submitted.value().refusal->code, RefusalCode::ReserveEvidenceStale);
  UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);
}

UC_TEST(recovery, restart_does_not_reissue_a_prior_control_attempt) {
  uc_test::Fixture fixture("recovery-attempt");
  fixture.script.effects_log = fixture.scratch->file("effects.log");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));

  const Result<AttemptRecord> submitted =
      fixture.submit(CommandKind::EnterStaticBypass, "restart-key", Tick{1100}, "test-authority");
  UC_REQUIRE_OK(submitted);
  const AttemptRecord original = submitted.value();
  UC_CHECK_EQ(original.phase, AttemptPhase::Acknowledged);
  UC_CHECK_EQ(original.ack, AckOutcome::Accepted);
  UC_CHECK_EQ(uc_test::count_lines(fixture.script.effects_log), std::size_t{1});

  const Result<UpsRecord> before_close = fixture.current();
  UC_REQUIRE_OK(before_close);
  UC_REQUIRE(before_close.value().in_flight.has_value());
  UC_CHECK_EQ(before_close.value().in_flight.value(), original.id);
  const UpsId ups = before_close.value().id;
  // Built while the fixture engine is still open.
  const ControlCommand second_command = fixture.command(CommandKind::EnterStaticBypass, "restart-key-2",
                                                        Tick{1300}, "test-authority");
  const ControlCommand retry_command = fixture.command(CommandKind::EnterStaticBypass, "restart-key",
                                                       Tick{1300}, "test-authority");

  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<UpsControlEngine>> engine = reopen_engine(fixture);
  UC_REQUIRE_OK(engine);
  UC_CHECK_EQ(engine.value()->info().lifecycle, EngineLifecycle::Recovered);
  UC_CHECK_EQ(uc_test::count_lines(fixture.script.effects_log), std::size_t{1});

  UC_REQUIRE_STATUS(revalidate_at(engine.value(), fixture.context, Tick{1200}), StatusCode::Ok);

  // (a) the attempt survived byte for byte and was not re-verified.
  const Result<AttemptRecord> recovered = engine.value()->attempt(original.id);
  UC_REQUIRE_OK(recovered);
  UC_CHECK_MSG(recovered.value() == original, "recovery must not rewrite the attempt record");
  UC_CHECK_EQ(recovered.value().id, original.id);
  UC_CHECK_EQ(recovered.value().phase, original.phase);
  UC_CHECK_EQ(recovered.value().ack, original.ack);
  UC_CHECK_EQ(recovered.value().plan_digest, original.plan_digest);
  UC_CHECK_EQ(recovered.value().key, original.key);

  // (b) the single-device ordering gate is still held, and (c) the engine
  // reports one unresolved attempt.
  const Result<std::vector<UpsRecord>> units = engine.value()->units();
  UC_REQUIRE_OK(units);
  if (units.value().size() != 1 || !units.value().front().in_flight.has_value()) {
    UC_CHECK_MSG(false, "the recovered unit must still hold the in-flight gate");
    return;
  }
  UC_CHECK_EQ(units.value().front().in_flight.value(), original.id);
  UC_CHECK_EQ(engine.value()->info().unresolved_attempt_count, std::size_t{1});

  // (d) a new key is refused while the gate is held.
  const Result<AttemptRecord> second = engine.value()->submit(second_command);
  UC_REQUIRE_OK(second);
  UC_CHECK_EQ(second.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(second.value().refusal.has_value());
  UC_CHECK_EQ(second.value().refusal->code, RefusalCode::TransitionInProgress);
  UC_CHECK_EQ(uc_test::count_lines(fixture.script.effects_log), std::size_t{1});

  // (e) a retry of the original key replays the same attempt and issues nothing.
  const Result<AttemptRecord> retried = engine.value()->submit(retry_command);
  UC_REQUIRE_OK(retried);
  UC_CHECK_EQ(retried.value().id, original.id);
  UC_CHECK_EQ(retried.value().phase, original.phase);
  UC_CHECK_EQ(retried.value().plan_digest, original.plan_digest);
  UC_CHECK_MSG(retried.value() == original, "the replay must return the recorded attempt unchanged");
  UC_CHECK_EQ(uc_test::count_lines(fixture.script.effects_log), std::size_t{1});
  UC_CHECK_EQ(engine.value()->info().unresolved_attempt_count, std::size_t{1});
  // The refused request is itself a durable fact, so the journal holds two
  // attempts while only the original one is unresolved and only the original one
  // ever reached the adapter.
  const Result<std::vector<AttemptRecord>> history = engine.value()->history(HistoryQuery{});
  UC_REQUIRE_OK(history);
  UC_CHECK_EQ(history.value().size(), std::size_t{2});
  UC_CHECK_EQ(engine.value()->info().attempt_count, std::size_t{2});
  if (history.value().size() == 2) {
    // History is newest first.
    UC_CHECK_EQ(history.value().front().phase, AttemptPhase::Refused);
    UC_CHECK_EQ(history.value().back().id, original.id);
    UC_CHECK_EQ(history.value().back().phase, original.phase);
  }
  UC_CHECK_EQ(ups, recovered.value().ups);
  UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);
}

UC_TEST(recovery, generation_epoch_incarnation_obligations_and_grants_survive_verbatim) {
  uc_test::Fixture fixture("recovery-verbatim");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));

  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE_OK(unit);
  BindObligationRequest bind;
  bind.authority = fixture.context;
  bind.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
  bind.now = Tick{1100};
  bind.obligation.ref = ObligationRef::parse("obl-verbatim").value();
  bind.obligation.load = LoadId::parse("load-verbatim").value();
  bind.obligation.tier = ObligationTier::Critical;
  bind.obligation.protection = ProtectionRequirement::MustRemainProtected;
  bind.obligation.asserted_by = AuthorityRef::parse("facility-ops").value();
  bind.obligation.asserted_at = Tick{1100};
  bind.obligation.expires_at = Tick{900000};
  UC_REQUIRE_STATUS(fixture.engine->bind_obligation(bind), StatusCode::Ok);

  const Result<std::vector<ProtectedLoadObligation>> obligations =
      fixture.engine->obligations(UpsQuery{unit.value().id, Tick{1100}});
  UC_REQUIRE_OK(obligations);
  UC_REQUIRE(obligations.value().size() == 1);

  const Result<std::vector<UpsRecord>> before = fixture.engine->units();
  UC_REQUIRE_OK(before);
  if (before.value().size() != 1) {
    UC_CHECK_MSG(false, "the fixture must expose exactly one unit");
    return;
  }
  const std::vector<ProtectedLoadObligation> obligation_bindings = before.value().front().obligations;
  const std::vector<AuthorityGrant> grant_bindings = before.value().front().grants;
  UC_CHECK_EQ(obligation_bindings.size(), std::size_t{1});
  UC_CHECK_EQ(grant_bindings.size(), std::size_t{1});

  const EngineInfo before_info = fixture.engine->info();
  const Result<std::shared_ptr<const UpsState>> before_state =
      UpsStore::read_file(fixture.store_options.path, StoreReadOptions{});
  UC_REQUIRE_OK(before_state);
  const std::uint64_t digest = canonical_state_digest(*before_state.value());

  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<UpsControlEngine>> engine = reopen_engine(fixture);
  UC_REQUIRE_OK(engine);

  // Recovery itself changes nothing: the engine comes back at exactly the
  // committed generation, epoch and incarnation.
  const EngineInfo recovered_info = engine.value()->info();
  UC_CHECK_EQ(recovered_info.generation, before_info.generation);
  UC_CHECK_EQ(recovered_info.epoch, before_info.epoch);
  UC_CHECK_EQ(recovered_info.incarnation, before_info.incarnation);
  UC_CHECK_EQ(recovered_info.created_at, before_info.created_at);
  UC_CHECK_EQ(recovered_info.store_identity, before_info.store_identity);
  {
    const Result<std::shared_ptr<const UpsState>> recovered_state =
        UpsStore::read_file(fixture.store_options.path, StoreReadOptions{});
    UC_REQUIRE_OK(recovered_state);
    UC_CHECK_EQ(canonical_state_digest(*recovered_state.value()), digest);
  }

  UC_REQUIRE_STATUS(revalidate_at(engine.value(), fixture.context, Tick{1200}), StatusCode::Ok);

  const EngineInfo after_info = engine.value()->info();
  // Revalidation is itself a durable commit, so it advances the generation by
  // exactly one and moves nothing else.
  UC_CHECK_EQ(after_info.generation, StoreGeneration{before_info.generation.value() + 1});
  UC_CHECK_EQ(after_info.epoch, before_info.epoch);
  UC_CHECK_EQ(after_info.incarnation, before_info.incarnation);
  UC_CHECK_EQ(after_info.store_identity, before_info.store_identity);

  const Result<std::vector<UpsRecord>> after = engine.value()->units();
  UC_REQUIRE_OK(after);
  if (after.value().size() != 1) {
    UC_CHECK_MSG(false, "the recovered store must expose exactly one unit");
    return;
  }
  UC_CHECK_EQ(after.value().front().obligations, obligation_bindings);
  UC_CHECK_EQ(after.value().front().grants, grant_bindings);
  UC_CHECK_MSG(after.value().front().obligations == obligation_bindings,
               "committed protected-load obligations must survive verbatim");
  UC_CHECK_MSG(after.value().front().grants == grant_bindings,
               "committed authority grants must survive verbatim");
  UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);

  const Result<std::shared_ptr<const UpsState>> after_state =
      UpsStore::read_file(fixture.store_options.path, StoreReadOptions{});
  UC_REQUIRE_OK(after_state);
  UC_CHECK_NE(canonical_state_digest(*after_state.value()), digest);
  // Only the revalidation commit moved: the unit bindings are identical.
  UC_REQUIRE(after_state.value()->units.size() == 1);
  if (after_state.value()->units.size() == 1) {
    UC_CHECK_EQ(after_state.value()->units.front(), before.value().front());
    UC_CHECK_EQ(after_state.value()->attempts, before_state.value()->attempts);
    UC_CHECK_EQ(after_state.value()->idempotency, before_state.value()->idempotency);
  }
  UC_CHECK_EQ(after_state.value()->revalidated_at, Tick{1200});
}

UC_TEST(recovery, ten_read_only_recovery_cycles_change_nothing) {
  uc_test::Fixture fixture("recovery-read-only");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  const Result<std::shared_ptr<const UpsState>> initial =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_OK(initial);
  const std::uint64_t digest = canonical_state_digest(*initial.value());
  const StoreGeneration generation = initial.value()->generation;
  const std::vector<std::byte> head_bytes = read_all(store_path);
  const std::vector<std::byte> payload_bytes = read_all(payload_path_for(store_path, generation));

  // The read-write fixture created a lock file; remove it so that the read-only
  // cycles can prove they never create one.
  UC_REQUIRE(std::filesystem::remove(store_path.string() + ".lock"));
  UC_CHECK(!std::filesystem::exists(store_path.string() + ".lock"));

  StoreOpenOptions options;
  options.path = store_path;
  options.access = StoreAccess::ReadOnly;
  options.create_if_missing = false;

  for (int cycle = 0; cycle < 10; ++cycle) {
    const Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options);
    UC_REQUIRE_OK(store);
    UC_CHECK_EQ(store.value()->info().generation, generation);
    UC_CHECK_EQ(store.value()->info().epoch, initial.value()->epoch);
    UC_CHECK_EQ(store.value()->info().incarnation, initial.value()->incarnation);
    if (store.value()->opened_state() == nullptr) {
      UC_CHECK_MSG(false, "a read-only open must expose the state it verified");
      return;
    }
    UC_CHECK_EQ(canonical_state_digest(*store.value()->opened_state()), digest);
    UC_CHECK_EQ(*store.value()->opened_state(), *initial.value());
    UC_REQUIRE_STATUS(store.value()->close(), StatusCode::Ok);
  }

  // A pure read-only recovery writes nothing at all, not even a lock file.
  UC_CHECK_EQ(read_all(store_path), head_bytes);
  UC_CHECK_EQ(read_all(payload_path_for(store_path, generation)), payload_bytes);
  UC_CHECK(!std::filesystem::exists(store_path.string() + ".lock"));

  const Result<std::shared_ptr<const UpsState>> final_state =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_OK(final_state);
  UC_CHECK_EQ(canonical_state_digest(*final_state.value()), digest);
  UC_CHECK_EQ(final_state.value()->generation, generation);
}
