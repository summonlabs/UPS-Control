// Proof obligations for the control attempt lifecycle.
//
// The claims proven here:
//   1. an acknowledgement is not an effect, and an observed state is not a verified
//      effect: the three stages are separately recorded and separately gated;
//   2. a retry of an already committed attempt returns the prior committed result
//      before any staleness check can reject it;
//   3. idempotency retention is bounded and its eviction semantics are explicit;
//   4. at most one unresolved attempt exists per unit at a time.

#include <string>
#include <vector>

#include "fixture.hpp"
#include "test_harness.hpp"

#include "ups_control/engine.hpp"

using namespace ups_control;

namespace {

TelemetryReport effect_report(const UpsRecord& unit, Tick at, SourceRevision revision,
                              const std::string& evidence, OperatingState operating) {
  TelemetryReport report = uc_test::healthy_report(unit.id, unit.hardware, at, revision, evidence);
  report.operating = operating;
  return report;
}

}  // namespace

UC_TEST(attempts, submit_records_a_durable_attempt_and_never_claims_an_effect) {
  uc_test::Fixture fixture("attempts-submit");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  const UpsRecord before = UC_REQUIRE_OK(fixture.current());

  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnterStaticBypass, "submit-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Acknowledged);
  UC_CHECK_EQ(attempt.value().ack, AckOutcome::Accepted);
  UC_CHECK_EQ(attempt.value().verification, VerificationVerdict::NotEvaluated);
  UC_CHECK_EQ(attempt.value().evidence, EvidenceClass::Synthetic);
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{1});

  // The operating state did not move: an acknowledgement is not an effect.
  const UpsRecord after = UC_REQUIRE_OK(fixture.current());
  UC_CHECK_EQ(after.operating, before.operating);
  UC_CHECK(after.operating != OperatingState::StaticBypass);

  // The attempt is durable: it survives a reopen with the same identity, plan
  // digest and phase.
  const std::uint64_t digest = attempt.value().plan_digest;
  fixture.engine->close();
  const Result<std::shared_ptr<UpsControlEngine>> reopened =
      UpsControlEngine::open(fixture.open_options);
  UC_REQUIRE(reopened.ok());
  const Result<AttemptRecord> recovered = reopened.value()->attempt(attempt.value().id);
  UC_REQUIRE(recovered.ok());
  UC_CHECK_EQ(recovered.value().id.value(), attempt.value().id.value());
  UC_CHECK_EQ(recovered.value().phase, AttemptPhase::Acknowledged);
  UC_CHECK_EQ(recovered.value().plan_digest, digest);
  UC_CHECK_EQ(recovered.value().key.value(), std::string("submit-1"));
}

UC_TEST(attempts, acknowledgement_observation_and_verification_are_three_stages) {
  uc_test::Fixture fixture("attempts-stages");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const Result<AttemptRecord> submitted =
      fixture.submit(CommandKind::EnterStaticBypass, "stages-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(submitted.ok());
  UC_CHECK_EQ(submitted.value().phase, AttemptPhase::Acknowledged);
  UC_CHECK_EQ(submitted.value().observed, ObservedOutcome::None);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(fixture
                 .observe(effect_report(unit, fixture.now + 2, SourceRevision{2}, "ev-effect",
                                        OperatingState::StaticBypass),
                          fixture.now + 2)
                 .ok());

  const Result<AttemptRecord> observed = fixture.engine->attempt(submitted.value().id);
  UC_REQUIRE(observed.ok());
  UC_CHECK_EQ(observed.value().phase, AttemptPhase::Observed);
  UC_CHECK_EQ(observed.value().observed, ObservedOutcome::MatchesTarget);
  UC_CHECK_EQ(observed.value().observed_state, OperatingState::StaticBypass);
  UC_CHECK_EQ(observed.value().verification, VerificationVerdict::NotEvaluated);

  const Result<AttemptRecord> verified = fixture.verify(submitted.value().id, fixture.now + 2);
  UC_REQUIRE(verified.ok());
  UC_CHECK_EQ(verified.value().phase, AttemptPhase::Verified);
  UC_CHECK_EQ(verified.value().verification, VerificationVerdict::Verified);
  UC_REQUIRE(verified.value().verification_evidence.has_value());
  UC_CHECK_EQ(verified.value().verification_evidence->value(), std::string("ev-effect"));

  const UpsRecord moved = UC_REQUIRE_OK(fixture.current());
  UC_CHECK_EQ(moved.operating, OperatingState::StaticBypass);
  UC_CHECK_EQ(moved.basis, StateBasis::Verified);
  UC_CHECK(!moved.in_flight.has_value());
  UC_CHECK(!UC_REQUIRE_OK(fixture.engine->status(UpsQuery{moved.id, fixture.now + 2})).in_flight.has_value());
}

UC_TEST(attempts, verification_is_pure_and_refuses_every_incomplete_observation) {
  const UpsRecord unit = [] {
    UpsRecord record;
    record.id = UpsId::parse("ups-under-test").value();
    record.hardware = HardwareGeneration{1};
    record.revision = StateRevision{1};
    return record;
  }();

  AttemptRecord attempt;
  attempt.id = AttemptId{1};
  attempt.ups = unit.id;
  attempt.hardware = unit.hardware;
  attempt.command = CommandKind::EnterStaticBypass;
  attempt.target = OperatingState::StaticBypass;
  attempt.submitted_at = Tick{100};
  attempt.phase = AttemptPhase::Acknowledged;
  attempt.ack = AckOutcome::Accepted;
  attempt.acknowledged_at = Tick{101};

  ObservationRecord observation;
  observation.evidence = EvidenceId::parse("ev-1").value();
  observation.source = SourceId::parse("test-source").value();
  observation.provenance = Provenance::SyntheticAdapter;
  observation.observed_at = Tick{102};
  observation.received_at = Tick{102};
  observation.operating = OperatingState::StaticBypass;

  const FreshnessPolicy freshness = default_verification_freshness();
  std::string detail;

  // A planned attempt cannot be verified at all.
  AttemptRecord planned = attempt;
  planned.phase = AttemptPhase::Planned;
  UC_CHECK_EQ(verify_attempt(planned, observation, Tick{103}, freshness, detail),
              VerificationVerdict::Indeterminate);

  // An observation that is not newer than the acknowledgement decides nothing.
  ObservationRecord same_instant = observation;
  same_instant.observed_at = Tick{101};
  same_instant.received_at = Tick{101};
  UC_CHECK_EQ(verify_attempt(attempt, same_instant, Tick{103}, freshness, detail),
              VerificationVerdict::Indeterminate);

  // A fresh observation of the documented effect verifies.
  UC_CHECK_EQ(verify_attempt(attempt, observation, Tick{103}, freshness, detail),
              VerificationVerdict::Verified);

  // The same observation, evaluated much later, is stale and refuses.
  UC_CHECK_EQ(verify_attempt(attempt, observation, Tick{100'000}, freshness, detail),
              VerificationVerdict::Indeterminate);

  // A contradictory observation is contradicted, never verified.
  ObservationRecord contradictory = observation;
  contradictory.contradictory = true;
  contradictory.contradiction_detail = "fault asserted while online";
  UC_CHECK_EQ(verify_attempt(attempt, contradictory, Tick{103}, freshness, detail),
              VerificationVerdict::Contradicted);

  // A concrete state the command cannot produce is contradicted.
  ObservationRecord wrong = observation;
  wrong.operating = OperatingState::OnlineBattery;
  UC_CHECK_EQ(verify_attempt(attempt, wrong, Tick{103}, freshness, detail),
              VerificationVerdict::Contradicted);

  // No established state decides nothing.
  ObservationRecord unknown_state = observation;
  unknown_state.operating = OperatingState::Unknown;
  UC_CHECK_EQ(verify_attempt(attempt, unknown_state, Tick{103}, freshness, detail),
              VerificationVerdict::Indeterminate);

  // The dwell is honoured: unverified before it elapses, verified after.
  AttemptRecord dwell = attempt;
  dwell.parameters.verification_dwell = TickSpan{10};
  UC_CHECK_EQ(verify_attempt(dwell, observation, Tick{103}, freshness, detail),
              VerificationVerdict::Unverified);
  ObservationRecord later = observation;
  later.observed_at = Tick{111};
  later.received_at = Tick{111};
  UC_CHECK_EQ(verify_attempt(dwell, later, Tick{112}, freshness, detail),
              VerificationVerdict::Verified);

  // A capability command verifies against the observed capability.
  AttemptRecord capability = attempt;
  capability.command = CommandKind::EnableRecharge;
  capability.target = OperatingState::Unknown;
  ObservationRecord capability_observation = observation;
  capability_observation.capability.recharge_enabled = Bool3::True;
  UC_CHECK_EQ(verify_attempt(capability, capability_observation, Tick{103}, freshness, detail),
              VerificationVerdict::Verified);
  capability_observation.capability.recharge_enabled = Bool3::False;
  UC_CHECK_EQ(verify_attempt(capability, capability_observation, Tick{103}, freshness, detail),
              VerificationVerdict::Contradicted);
  capability_observation.capability.recharge_enabled = Bool3::Unknown;
  UC_CHECK_EQ(verify_attempt(capability, capability_observation, Tick{103}, freshness, detail),
              VerificationVerdict::Indeterminate);
}

UC_TEST(attempts, a_lost_acknowledgement_leaves_the_attempt_unresolved_and_ordered) {
  uc_test::Fixture fixture("attempts-noresponse");
  fixture.script.acknowledgements[CommandKind::EnterStaticBypass] = AckOutcome::NoResponse;
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnterStaticBypass, "lost-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Issued);
  UC_CHECK_EQ(attempt.value().ack, AckOutcome::NoResponse);
  UC_CHECK(is_unresolved(attempt.value().phase));
  UC_CHECK(!attempt.value().terminal_detail.empty());

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(unit.in_flight.has_value());
  UC_CHECK_EQ(unit.in_flight->value(), attempt.value().id.value());

  // A different key is refused while the gate is held, with the ordering refusal.
  const Result<AttemptRecord> second =
      fixture.submit(CommandKind::EnterStaticBypass, "lost-2", fixture.now + 2, "test-authority");
  UC_REQUIRE(second.ok());
  UC_CHECK_EQ(second.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(second.value().refusal.has_value());
  UC_CHECK_EQ(second.value().refusal->code, RefusalCode::TransitionInProgress);
  // The refused request never reached the adapter.
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{1});

  // An observation of the effect can still verify the attempt that lost its
  // acknowledgement, because the device may have acted.
  UC_REQUIRE(fixture
                 .observe(effect_report(unit, fixture.now + 3, SourceRevision{2}, "ev-lost",
                                        OperatingState::StaticBypass),
                          fixture.now + 3)
                 .ok());
  const Result<AttemptRecord> verified = fixture.verify(attempt.value().id, fixture.now + 3);
  UC_REQUIRE(verified.ok());
  UC_CHECK_EQ(verified.value().phase, AttemptPhase::Verified);
  const UpsRecord released = UC_REQUIRE_OK(fixture.current());
  UC_CHECK(!released.in_flight.has_value());
}

UC_TEST(attempts, a_retry_of_the_same_key_returns_the_prior_result_before_any_staleness_check) {
  uc_test::Fixture fixture("attempts-replay");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const Result<AttemptRecord> first =
      fixture.submit(CommandKind::EnterStaticBypass, "replay-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(first.ok());
  UC_CHECK_EQ(first.value().phase, AttemptPhase::Acknowledged);

  // Move the store on: a new observation bumps the revision, and time moves on.
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(fixture
                 .observe(effect_report(unit, fixture.now + 2, SourceRevision{2}, "ev-move",
                                        OperatingState::StaticBypass),
                          fixture.now + 2)
                 .ok());
  const UpsRecord moved = UC_REQUIRE_OK(fixture.current());
  UC_CHECK(moved.revision.value() > unit.revision.value());

  // The retry cites the ORIGINAL revision and a much later instant: both fences are
  // stale, and it must still return the prior accepted result.
  ControlCommand retry;
  retry.authority = fixture.context;
  retry.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  retry.now = fixture.now + 100'000;
  retry.key = IdempotencyKey::parse("replay-1").value();
  retry.kind = CommandKind::EnterStaticBypass;
  retry.authority_ref = AuthorityRef::parse("test-authority").value();

  // The prior record has itself advanced to Observed because the effect was
  // recorded; the replay must return exactly that record and not a new one.
  const Result<AttemptRecord> prior = fixture.engine->attempt(first.value().id);
  UC_REQUIRE(prior.ok());
  UC_CHECK_EQ(prior.value().phase, AttemptPhase::Observed);

  const Result<AttemptRecord> replayed = fixture.engine->submit(retry);
  UC_REQUIRE(replayed.ok());
  UC_CHECK_EQ(replayed.value().id.value(), first.value().id.value());
  UC_CHECK_EQ(replayed.value().phase, prior.value().phase);
  UC_CHECK_EQ(replayed.value().plan_digest, first.value().plan_digest);
  // No second attempt was created and the adapter was not addressed again.
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{1});
  const Result<std::vector<AttemptRecord>> history = fixture.engine->history(HistoryQuery{});
  UC_REQUIRE(history.ok());
  UC_CHECK_EQ(history.value().size(), std::size_t{1});

  // The same key with a different intent is a conflict, not a replay.
  ControlCommand different = retry;
  different.kind = CommandKind::StartSelfTest;
  UC_REQUIRE_STATUS(fixture.engine->submit(different), StatusCode::IdempotencyConflict);

  // Replay through the read path returns the same record.
  const Result<AttemptRecord> direct = fixture.engine->replay(retry.key);
  UC_REQUIRE(direct.ok());
  UC_CHECK_EQ(direct.value().id.value(), first.value().id.value());
}

UC_TEST(attempts, idempotency_retention_is_bounded_and_eviction_is_explicit) {
  uc_test::Fixture fixture("attempts-retention");
  fixture.store_options.limits.max_idempotency_records = 3;
  fixture.store_options.limits.max_attempt_journal = 3;
  UC_REQUIRE(fixture.build(GrantScope::TestExecution));

  for (int index = 0; index < 5; ++index) {
    const Tick at = fixture.now + 1 + index;
    const Result<AttemptRecord> attempt = fixture.submit(
        CommandKind::StartSelfTest, "retain-" + std::to_string(index), at, "test-authority");
    UC_REQUIRE(attempt.ok());
    UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Acknowledged);

    // Clear the gate and the test state so the next attempt is independent.
    const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
    UC_REQUIRE(fixture.abandon(attempt.value().id, at, "retention test").ok());
    AdoptOperatingStateRequest reset;
    reset.authority = fixture.context;
    const UpsRecord after = UC_REQUIRE_OK(fixture.current());
    reset.ref = UpsRef{after.id, after.hardware, after.revision};
    reset.now = at;
    reset.operating = OperatingState::OnlineNormal;
    reset.reason = "retention reset";
    UC_REQUIRE(fixture.engine->adopt_operating_state(reset).ok());
    (void)unit;
  }

  const Result<StoreAuditReport> audit = fixture.engine->store_audit();
  UC_REQUIRE(audit.ok());
  UC_CHECK_EQ(audit.value().idempotency_count, std::size_t{3});
  UC_CHECK_EQ(audit.value().attempt_count, std::size_t{3});

  // The two oldest bindings were evicted: replay of them is refused with NotFound.
  UC_REQUIRE_STATUS(fixture.engine->replay(IdempotencyKey::parse("retain-0").value()),
                    StatusCode::NotFound);
  UC_REQUIRE_STATUS(fixture.engine->replay(IdempotencyKey::parse("retain-1").value()),
                    StatusCode::NotFound);
  // The three most recent are still recognized.
  UC_REQUIRE(fixture.engine->replay(IdempotencyKey::parse("retain-4").value()).ok());

  // A retry of an evicted key is a new request, and is then refused by the
  // ordinary staleness checks rather than re-applied.
  ControlCommand stale = fixture.command(CommandKind::StartSelfTest, "retain-0", fixture.now + 100,
                                         "test-authority");
  stale.ref.revision = StateRevision{1};
  const Result<AttemptRecord> refused = fixture.engine->submit(stale);
  UC_REQUIRE(refused.ok());
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::StaleRevision);
}

UC_TEST(attempts, abandoning_an_attempt_leaves_the_basis_unverified_until_fresh_telemetry) {
  uc_test::Fixture fixture("attempts-abandon");
  fixture.script.acknowledgements[CommandKind::EnterStaticBypass] = AckOutcome::NoResponse;
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnterStaticBypass, "abandon-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Issued);

  const Result<AttemptRecord> abandoned = fixture.abandon(attempt.value().id, fixture.now + 2);
  UC_REQUIRE(abandoned.ok());
  UC_CHECK_EQ(abandoned.value().phase, AttemptPhase::Failed);
  UC_CHECK(is_terminal(abandoned.value().phase));

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_CHECK(!unit.in_flight.has_value());
  UC_CHECK_EQ(unit.basis, StateBasis::CommandedUnverified);

  // The next command is refused because the current state is no longer trustworthy.
  const Result<AttemptRecord> next =
      fixture.submit(CommandKind::EnterStaticBypass, "abandon-2", fixture.now + 3, "test-authority");
  UC_REQUIRE(next.ok());
  UC_CHECK_EQ(next.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(next.value().refusal.has_value());
  UC_CHECK_EQ(next.value().refusal->code, RefusalCode::StateBasisUnverified);

  // Fresh telemetry restores the basis and the command becomes possible again.
  UC_REQUIRE(fixture
                 .observe(effect_report(unit, fixture.now + 4, SourceRevision{2}, "ev-recover",
                                        OperatingState::OnlineNormal),
                          fixture.now + 4)
                 .ok());
  const UpsRecord restored = UC_REQUIRE_OK(fixture.current());
  UC_CHECK_EQ(restored.basis, StateBasis::Observed);
  const Result<AttemptRecord> allowed =
      fixture.submit(CommandKind::EnterStaticBypass, "abandon-3", fixture.now + 5, "test-authority");
  UC_REQUIRE(allowed.ok());
  // This fixture's adapter never answers for this command, so the new attempt is
  // Issued rather than Acknowledged; what matters is that it was accepted at all.
  UC_CHECK(is_unresolved(allowed.value().phase));
  UC_CHECK_EQ(allowed.value().ack, AckOutcome::NoResponse);
  UC_CHECK(!allowed.value().refusal.has_value());
}

UC_TEST(attempts, stage_operations_on_wrong_or_terminal_attempts_are_refused) {
  uc_test::Fixture fixture("attempts-stage-errors");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  UC_REQUIRE_STATUS(fixture.verify(AttemptId{999}, fixture.now + 1), StatusCode::AttemptNotFound);
  UC_REQUIRE_STATUS(fixture.abandon(AttemptId{999}, fixture.now + 1), StatusCode::AttemptNotFound);
  AcknowledgeRequest acknowledge;
  acknowledge.authority = fixture.context;
  acknowledge.now = fixture.now + 1;
  acknowledge.attempt = AttemptId{999};
  UC_REQUIRE_STATUS(fixture.engine->acknowledge(acknowledge), StatusCode::AttemptNotFound);

  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnterStaticBypass, "terminal-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(attempt.ok());
  UC_REQUIRE(fixture.abandon(attempt.value().id, fixture.now + 2).ok());

  UC_REQUIRE_STATUS(fixture.verify(attempt.value().id, fixture.now + 3), StatusCode::AttemptStateConflict);
  UC_REQUIRE_STATUS(fixture.abandon(attempt.value().id, fixture.now + 3), StatusCode::AttemptStateConflict);
  acknowledge.attempt = attempt.value().id;
  acknowledge.now = fixture.now + 3;
  UC_REQUIRE_STATUS(fixture.engine->acknowledge(acknowledge), StatusCode::AttemptStateConflict);
}

UC_TEST(attempts, a_refused_request_is_recorded_and_never_reaches_the_adapter) {
  uc_test::Fixture fixture("attempts-refused");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnterStaticBypass, "refused-1", fixture.now + 1, "no-such-authority");
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Refused);
  UC_CHECK_EQ(attempt.value().ack, AckOutcome::None);
  UC_REQUIRE(attempt.value().refusal.has_value());
  UC_CHECK_EQ(attempt.value().refusal->code, RefusalCode::AuthorityMissing);
  UC_CHECK_EQ(fixture.adapter->issue_count(), std::uint64_t{0});

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_CHECK(!unit.in_flight.has_value());

  const Result<AttemptRecord> recovered = fixture.engine->attempt(attempt.value().id);
  UC_REQUIRE(recovered.ok());
  UC_CHECK_EQ(recovered.value().phase, AttemptPhase::Refused);
  UC_CHECK_EQ(recovered.value().refusal->code, RefusalCode::AuthorityMissing);
}

UC_TEST(attempts, the_commit_log_records_one_entry_per_durable_step) {
  uc_test::Fixture fixture("attempts-commit-log");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));

  const StoreAuditReport before = UC_REQUIRE_OK(fixture.engine->store_audit());
  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnterStaticBypass, "log-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(attempt.ok());
  const StoreAuditReport after_submit = UC_REQUIRE_OK(fixture.engine->store_audit());
  UC_CHECK(after_submit.generation.value() > before.generation.value());
  UC_CHECK_EQ(after_submit.commit_log.back().operation, OperationKind::AttemptAcknowledged);
  UC_CHECK_EQ(after_submit.commit_log.back().attempt.value(), attempt.value().id.value());
  UC_CHECK_EQ(after_submit.commit_log.back().key.value(), std::string("log-1"));

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(fixture
                 .observe(effect_report(unit, fixture.now + 2, SourceRevision{2}, "ev-log",
                                        OperatingState::StaticBypass),
                          fixture.now + 2)
                 .ok());
  UC_REQUIRE(fixture.verify(attempt.value().id, fixture.now + 2).ok());
  const StoreAuditReport after_verify = UC_REQUIRE_OK(fixture.engine->store_audit());
  UC_CHECK_EQ(after_verify.commit_log.back().operation, OperationKind::AttemptVerified);
  UC_CHECK_EQ(after_verify.terminal_attempt_count, std::size_t{1});
  UC_CHECK_EQ(after_verify.unresolved_attempt_count, std::size_t{0});

  // The generation advances by exactly one per durable step.
  for (std::size_t index = 1; index < after_verify.commit_log.size(); ++index) {
    UC_CHECK_EQ(after_verify.commit_log[index].generation.value(),
                after_verify.commit_log[index - 1].generation.value() + 1);
  }
}

UC_TEST(attempts, capability_commands_verify_against_the_observed_capability) {
  uc_test::Fixture fixture("attempts-capability");
  UC_REQUIRE(fixture.build(GrantScope::RechargeEnable));

  const Result<AttemptRecord> attempt =
      fixture.submit(CommandKind::EnableRecharge, "capability-1", fixture.now + 1, "test-authority");
  UC_REQUIRE(attempt.ok());
  UC_CHECK_EQ(attempt.value().phase, AttemptPhase::Acknowledged);
  UC_CHECK_EQ(attempt.value().target, OperatingState::Unknown);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  TelemetryReport observed = effect_report(unit, fixture.now + 2, SourceRevision{2}, "ev-capability",
                                           OperatingState::OnlineNormal);
  observed.capability.recharge_enabled = Bool3::True;
  UC_REQUIRE(fixture.observe(observed, fixture.now + 2).ok());

  const Result<AttemptRecord> verified = fixture.verify(attempt.value().id, fixture.now + 2);
  UC_REQUIRE(verified.ok());
  UC_CHECK_EQ(verified.value().verification, VerificationVerdict::Verified);
  // A capability change does not move the operating state.
  const UpsRecord after = UC_REQUIRE_OK(fixture.current());
  UC_CHECK_EQ(after.operating, OperatingState::OnlineNormal);
}
