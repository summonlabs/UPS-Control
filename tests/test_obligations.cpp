// Proof obligations for the protected-load obligation model.
//
// The central invariant under test is that a protected-load binding is never
// silently dropped: a protection-removing transition is refused while a
// protective obligation still binds, a lapsed binding is unresolved rather than
// void, and only an explicit authority-bound release removes the requirement.

#include "test_harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "ups_control/engine.hpp"

using namespace ups_control;

namespace {

const ResourceLimits kLimits = ResourceLimits::defaults();

AuthorityRef ref_of(const char* text) { return AuthorityRef::parse(text).value(); }

ProtectedLoadObligation make_obligation(const std::string& ref, ProtectionRequirement requirement,
                                        Tick asserted_at, std::optional<Tick> expires_at) {
  ProtectedLoadObligation obligation;
  obligation.ref = ObligationRef::parse(ref).value();
  obligation.load = LoadId::parse("load." + ref).value();
  obligation.tier = ObligationTier::LifeSafety;
  obligation.protection = requirement;
  obligation.asserted_by = ref_of("facility-safety");
  obligation.asserted_at = asserted_at;
  obligation.expires_at = expires_at;
  obligation.state = ObligationState::Active;
  return obligation;
}

ProtectedLoadObligation with_release(const ProtectedLoadObligation& obligation,
                                     ObligationState outcome, Tick released_at) {
  ProtectedLoadObligation copy = obligation;
  copy.state = outcome;
  copy.released_by = ref_of("facility-safety");
  copy.released_at = released_at;
  copy.release_reason = "released for planned maintenance";
  return copy;
}

bool contains_ref(const std::vector<ObligationRef>& refs, const ObligationRef& ref) {
  return std::find(refs.begin(), refs.end(), ref) != refs.end();
}

/// Binds one obligation through the engine, citing the unit's current revision.
Result<ProtectedLoadObligation> bind_through_engine(uc_test::Fixture& fixture,
                                                    const ProtectedLoadObligation& obligation,
                                                    Tick at) {
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  BindObligationRequest request;
  request.authority = fixture.context;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = at;
  request.obligation = obligation;
  return fixture.engine->bind_obligation(request);
}

/// Releases one obligation through the engine, citing the given revision.
Result<ProtectedLoadObligation> release_through_engine(uc_test::Fixture& fixture,
                                                       const ObligationRef& obligation,
                                                       ObligationRevision revision, Tick at) {
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  ReleaseObligationRequest request;
  request.authority = fixture.context;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = at;
  request.obligation = obligation;
  request.obligation_revision = revision;
  request.release_authority = ref_of("facility-safety");
  request.reason = "facility safety released the protected load";
  request.outcome = ObligationState::Released;
  return fixture.engine->release_obligation(request);
}

/// Issues one grant of the given scope through the engine.
Result<AuthorityGrant> issue_through_engine_grant(uc_test::Fixture& fixture,
                                                  const char* authority, GrantScope scope, Tick at) {
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  IssueGrantRequest request;
  request.authority = fixture.context;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = at;
  request.grant.ref = ref_of(authority);
  request.grant.scope = scope;
  request.grant.granted_by = ref_of("facility-ops");
  request.grant.epoch = fixture.context.epoch;
  request.grant.incarnation = fixture.context.incarnation;
  request.grant.issued_at = at;
  return fixture.engine->issue_grant(request);
}

}  // namespace

// ---------------------------------------------------------------------------
// Binding, lapse, and protection class
// ---------------------------------------------------------------------------

UC_TEST(obligations, obligation_binds_only_for_active_unexpired_bindings) {
  const ProtectedLoadObligation open =
      make_obligation("ob-open", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);
  UC_CHECK(obligation_binds(open, Tick{100}));
  UC_CHECK(obligation_binds(open, Tick{1000000}));

  const ProtectedLoadObligation windowed = make_obligation(
      "ob-window", ProtectionRequirement::MustRemainProtected, Tick{100}, Tick{200});
  UC_CHECK(obligation_binds(windowed, Tick{100}));
  UC_CHECK(obligation_binds(windowed, Tick{199}));
  UC_CHECK(!obligation_binds(windowed, Tick{200}));
  UC_CHECK(!obligation_binds(windowed, Tick{201}));

  UC_CHECK(!obligation_binds(with_release(open, ObligationState::Released, Tick{150}), Tick{150}));
  UC_CHECK(!obligation_binds(with_release(open, ObligationState::Suspended, Tick{150}), Tick{150}));
}

UC_TEST(obligations, obligation_lapsed_only_for_active_bindings_past_expiry) {
  const ProtectedLoadObligation windowed = make_obligation(
      "ob-window", ProtectionRequirement::MustRemainProtected, Tick{100}, Tick{200});
  UC_CHECK(!obligation_lapsed(windowed, Tick{100}));
  UC_CHECK(!obligation_lapsed(windowed, Tick{199}));
  UC_CHECK(obligation_lapsed(windowed, Tick{200}));
  UC_CHECK(obligation_lapsed(windowed, Tick{5000}));

  const ProtectedLoadObligation open =
      make_obligation("ob-open", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);
  UC_CHECK(!obligation_lapsed(open, Tick{1000000}));

  UC_CHECK(!obligation_lapsed(with_release(windowed, ObligationState::Released, Tick{150}),
                              Tick{5000}));
  UC_CHECK(!obligation_lapsed(with_release(windowed, ObligationState::Suspended, Tick{150}),
                              Tick{5000}));
}

UC_TEST(obligations, obligation_is_protective_excludes_only_informational) {
  UC_CHECK(obligation_is_protective(make_obligation(
      "ob-must", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt)));
  UC_CHECK(obligation_is_protective(make_obligation(
      "ob-may", ProtectionRequirement::MayBeInterruptedWithAuthority, Tick{100}, std::nullopt)));
  UC_CHECK(!obligation_is_protective(make_obligation(
      "ob-info", ProtectionRequirement::Informational, Tick{100}, std::nullopt)));
}

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------

UC_TEST(obligations, validate_obligation_refuses_empty_reference_load_and_asserting_authority) {
  const ProtectedLoadObligation good =
      make_obligation("ob-good", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);
  UC_REQUIRE_STATUS(validate_obligation(good, kLimits), StatusCode::Ok);

  ProtectedLoadObligation empty_ref = good;
  empty_ref.ref = ObligationRef{};
  UC_REQUIRE_STATUS(validate_obligation(empty_ref, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation empty_load = good;
  empty_load.load = LoadId{};
  UC_REQUIRE_STATUS(validate_obligation(empty_load, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation empty_authority = good;
  empty_authority.asserted_by = AuthorityRef{};
  UC_REQUIRE_STATUS(validate_obligation(empty_authority, kLimits), StatusCode::InvalidArgument);
}

UC_TEST(obligations, validate_obligation_refuses_non_positive_assertion_instant) {
  ProtectedLoadObligation zero = make_obligation(
      "ob-zero", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);
  zero.asserted_at = Tick{0};
  UC_REQUIRE_STATUS(validate_obligation(zero, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation negative = zero;
  negative.asserted_at = Tick{-7};
  UC_REQUIRE_STATUS(validate_obligation(negative, kLimits), StatusCode::InvalidArgument);
}

UC_TEST(obligations, validate_obligation_refuses_expiry_not_strictly_after_assertion) {
  const ProtectedLoadObligation base =
      make_obligation("ob-exp", ProtectionRequirement::MustRemainProtected, Tick{100}, Tick{200});

  ProtectedLoadObligation equal = base;
  equal.expires_at = Tick{100};
  UC_REQUIRE_STATUS(validate_obligation(equal, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation before = base;
  before.expires_at = Tick{99};
  UC_REQUIRE_STATUS(validate_obligation(before, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation origin = base;
  origin.expires_at = Tick{0};
  UC_REQUIRE_STATUS(validate_obligation(origin, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation next = base;
  next.expires_at = Tick{101};
  UC_REQUIRE_STATUS(validate_obligation(next, kLimits), StatusCode::Ok);

  UC_REQUIRE_STATUS(validate_obligation(base, kLimits), StatusCode::Ok);
}

UC_TEST(obligations, validate_obligation_refuses_incomplete_release_record) {
  const ProtectedLoadObligation base =
      make_obligation("ob-rel", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);

  ProtectedLoadObligation no_authority = base;
  no_authority.state = ObligationState::Released;
  no_authority.released_at = Tick{150};
  UC_REQUIRE_STATUS(validate_obligation(no_authority, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation no_instant = base;
  no_instant.state = ObligationState::Released;
  no_instant.released_by = ref_of("facility-safety");
  UC_REQUIRE_STATUS(validate_obligation(no_instant, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation origin_instant = no_instant;
  origin_instant.released_at = Tick{0};
  UC_REQUIRE_STATUS(validate_obligation(origin_instant, kLimits), StatusCode::InvalidArgument);

  ProtectedLoadObligation empty_authority = no_instant;
  empty_authority.released_by = AuthorityRef{};
  empty_authority.released_at = Tick{150};
  UC_REQUIRE_STATUS(validate_obligation(empty_authority, kLimits), StatusCode::InvalidArgument);

  const ProtectedLoadObligation complete =
      with_release(base, ObligationState::Released, Tick{150});
  UC_REQUIRE_STATUS(validate_obligation(complete, kLimits), StatusCode::Ok);
}

UC_TEST(obligations, validate_obligation_refuses_overlong_release_reason) {
  ProtectedLoadObligation base =
      make_obligation("ob-reason", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);

  base.release_reason = std::string(kLimits.max_detail_bytes, 'x');
  UC_REQUIRE_STATUS(validate_obligation(base, kLimits), StatusCode::Ok);

  base.release_reason = std::string(kLimits.max_detail_bytes + 1, 'x');
  UC_REQUIRE_STATUS(validate_obligation(base, kLimits), StatusCode::LimitExceeded);
}

// ---------------------------------------------------------------------------
// assess_protected_impact
// ---------------------------------------------------------------------------

UC_TEST(obligations, assess_protected_impact_preserves_when_nothing_is_bound) {
  const ProtectedImpactReport report =
      assess_protected_impact({}, std::nullopt, Tick{500}, kLimits);
  UC_CHECK_EQ(report.impact, ProtectionImpact::Preserves);
  UC_CHECK(report.binding.empty());
  UC_CHECK(report.at_risk.empty());
  UC_CHECK(report.unreleased.empty());
  UC_CHECK(report.lapsed.empty());

  const ProtectedImpactReport cited =
      assess_protected_impact({}, ref_of("facility-safety"), Tick{500}, kLimits);
  UC_CHECK_EQ(cited.impact, ProtectionImpact::Preserves);
}

UC_TEST(obligations, assess_protected_impact_flags_unreleased_must_remain_protected) {
  const ProtectedLoadObligation obligation = make_obligation(
      "ob-must", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);
  const ProtectedImpactReport report =
      assess_protected_impact({obligation}, std::nullopt, Tick{500}, kLimits);

  UC_CHECK_EQ(report.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(report.binding, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK_EQ(report.at_risk, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK_EQ(report.unreleased, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK(report.lapsed.empty());
}

UC_TEST(obligations, citing_asserting_authority_does_not_release_must_remain_protected) {
  const ProtectedLoadObligation obligation = make_obligation(
      "ob-must", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);
  const ProtectedImpactReport report = assess_protected_impact(
      {obligation}, ref_of("facility-safety"), Tick{500}, kLimits);

  UC_CHECK_EQ(report.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(report.unreleased, std::vector<ObligationRef>{obligation.ref});
}

UC_TEST(obligations, citing_asserting_authority_covers_may_be_interrupted) {
  const ProtectedLoadObligation obligation = make_obligation(
      "ob-may", ProtectionRequirement::MayBeInterruptedWithAuthority, Tick{100}, std::nullopt);
  const ProtectedImpactReport report = assess_protected_impact(
      {obligation}, ref_of("facility-safety"), Tick{500}, kLimits);

  UC_CHECK_EQ(report.impact, ProtectionImpact::ReducedWithAuthority);
  UC_CHECK_EQ(report.binding, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK_EQ(report.at_risk, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK(report.unreleased.empty());
}

UC_TEST(obligations, may_be_interrupted_needs_the_asserting_authority_exactly) {
  const ProtectedLoadObligation obligation = make_obligation(
      "ob-may", ProtectionRequirement::MayBeInterruptedWithAuthority, Tick{100}, std::nullopt);
  const ProtectedImpactReport report = assess_protected_impact(
      {obligation}, ref_of("some-other-authority"), Tick{500}, kLimits);

  UC_CHECK_EQ(report.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(report.unreleased, std::vector<ObligationRef>{obligation.ref});

  const ProtectedImpactReport uncited =
      assess_protected_impact({obligation}, std::nullopt, Tick{500}, kLimits);
  UC_CHECK_EQ(uncited.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(uncited.unreleased, std::vector<ObligationRef>{obligation.ref});
}

UC_TEST(obligations, released_or_suspended_obligation_is_not_at_risk) {
  const ProtectedLoadObligation base = make_obligation(
      "ob-must", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt);

  for (const ObligationState state : {ObligationState::Released, ObligationState::Suspended}) {
    const ProtectedLoadObligation obligation = with_release(base, state, Tick{150});
    const ProtectedImpactReport report =
        assess_protected_impact({obligation}, std::nullopt, Tick{500}, kLimits);
    UC_CHECK_EQ(report.impact, ProtectionImpact::Preserves);
    UC_CHECK(report.binding.empty());
    UC_CHECK(report.at_risk.empty());
    UC_CHECK(report.unreleased.empty());
    UC_CHECK(report.lapsed.empty());
  }
}

UC_TEST(obligations, lapsed_obligation_is_unreleased_rather_than_silently_dropped) {
  const ProtectedLoadObligation obligation = make_obligation(
      "ob-lapsed", ProtectionRequirement::MustRemainProtected, Tick{100}, Tick{150});
  const ProtectedImpactReport report =
      assess_protected_impact({obligation}, std::nullopt, Tick{500}, kLimits);

  UC_CHECK_EQ(report.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(report.lapsed, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK(report.binding.empty());
  UC_CHECK_EQ(report.at_risk, std::vector<ObligationRef>{obligation.ref});
  UC_CHECK_EQ(report.unreleased, std::vector<ObligationRef>{obligation.ref});
}

UC_TEST(obligations, informational_obligation_is_ignored_by_impact) {
  const ProtectedLoadObligation obligation = make_obligation(
      "ob-info", ProtectionRequirement::Informational, Tick{100}, std::nullopt);
  const ProtectedImpactReport report =
      assess_protected_impact({obligation}, std::nullopt, Tick{500}, kLimits);

  UC_CHECK_EQ(report.impact, ProtectionImpact::Preserves);
  UC_CHECK(report.binding.empty());
  UC_CHECK(report.at_risk.empty());
  UC_CHECK(report.unreleased.empty());
  UC_CHECK(report.lapsed.empty());

  // A lapsed informational binding is still not a protection requirement.
  const ProtectedLoadObligation lapsed = make_obligation(
      "ob-info-lapsed", ProtectionRequirement::Informational, Tick{100}, Tick{150});
  const ProtectedImpactReport lapsed_report =
      assess_protected_impact({lapsed}, std::nullopt, Tick{500}, kLimits);
  UC_CHECK_EQ(lapsed_report.impact, ProtectionImpact::Preserves);
  UC_CHECK(lapsed_report.lapsed.empty());
}

UC_TEST(obligations, assess_protected_impact_is_deterministic_in_reference_order) {
  const std::vector<ProtectedLoadObligation> obligations{
      make_obligation("ob-alpha", ProtectionRequirement::MustRemainProtected, Tick{100}, std::nullopt),
      make_obligation("ob-midway", ProtectionRequirement::MayBeInterruptedWithAuthority, Tick{100},
                      std::nullopt),
      make_obligation("ob-zulu", ProtectionRequirement::MustRemainProtected, Tick{100}, Tick{150})};

  const std::vector<ProtectedLoadObligation> same_input = obligations;
  const ProtectedImpactReport first =
      assess_protected_impact(obligations, ref_of("facility-safety"), Tick{500}, kLimits);
  const ProtectedImpactReport second =
      assess_protected_impact(same_input, ref_of("facility-safety"), Tick{500}, kLimits);
  UC_CHECK_EQ(first, second);

  UC_CHECK_EQ(first.binding,
              (std::vector<ObligationRef>{obligations[0].ref, obligations[1].ref}));
  UC_CHECK_EQ(first.at_risk,
              (std::vector<ObligationRef>{obligations[0].ref, obligations[1].ref, obligations[2].ref}));
  UC_CHECK_EQ(first.unreleased,
              (std::vector<ObligationRef>{obligations[0].ref, obligations[2].ref}));
  UC_CHECK_EQ(first.lapsed, std::vector<ObligationRef>{obligations[2].ref});
  UC_CHECK_EQ(first.impact, ProtectionImpact::DropsUndeclared);
}

// ---------------------------------------------------------------------------
// Through the engine
// ---------------------------------------------------------------------------

UC_TEST(obligations, engine_refuses_maintenance_bypass_until_obligation_released) {
  uc_test::Fixture fixture("obl-bypass");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<ProtectedLoadObligation> bound = bind_through_engine(
      fixture,
      make_obligation("ob-life-safety", ProtectionRequirement::MustRemainProtected, Tick{1000},
                      std::nullopt),
      Tick{1000});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);
  UC_CHECK_EQ(bound.value().state, ObligationState::Active);
  UC_CHECK_EQ(bound.value().revision, ObligationRevision{1});
  const ObligationRef ref = bound.value().ref;

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "key-bypass-refused", Tick{1010},
                      "test-authority"));
  UC_REQUIRE_OK(evaluation);
  UC_CHECK(!evaluation.value().report.allowed);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::ObligationUnreleased);
  UC_CHECK_EQ(evaluation.value().report.protection.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK(contains_ref(evaluation.value().report.protection.binding, ref));
  UC_CHECK(contains_ref(evaluation.value().report.protection.at_risk, ref));
  UC_CHECK(contains_ref(evaluation.value().report.protection.unreleased, ref));
  UC_CHECK(evaluation.value().report.protection.lapsed.empty());

  const Result<AttemptRecord> refused = fixture.submit(
      CommandKind::EnterMaintenanceBypass, "key-bypass-refused", Tick{1011}, "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::ObligationUnreleased);

  const Result<ProtectedLoadObligation> released =
      release_through_engine(fixture, ref, ObligationRevision{1}, Tick{1020});
  UC_REQUIRE_STATUS(released, StatusCode::Ok);
  UC_CHECK_EQ(released.value().state, ObligationState::Released);
  UC_CHECK_EQ(released.value().released_at.value(), Tick{1020});
  UC_CHECK_EQ(released.value().released_by.value(), ref_of("facility-safety"));
  UC_CHECK_EQ(released.value().revision, ObligationRevision{2});

  const Result<AttemptRecord> allowed = fixture.submit(
      CommandKind::EnterMaintenanceBypass, "key-bypass-allowed", Tick{1030}, "test-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(obligations, engine_refuses_isolate_output_until_obligation_released) {
  uc_test::Fixture fixture("obl-isolate");
  UC_REQUIRE(fixture.build(GrantScope::IsolationControl));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<ProtectedLoadObligation> bound = bind_through_engine(
      fixture,
      make_obligation("ob-critical", ProtectionRequirement::MustRemainProtected, Tick{1000},
                      std::nullopt),
      Tick{1000});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);
  const ObligationRef ref = bound.value().ref;

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::IsolateOutput,
                                                       "key-isolate-refused", Tick{1010},
                                                       "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::ObligationUnreleased);

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(
      fixture.command(CommandKind::IsolateOutput, "key-isolate-evaluate", Tick{1015},
                      "test-authority"));
  UC_REQUIRE_OK(evaluation);
  UC_CHECK_EQ(evaluation.value().report.protection.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(evaluation.value().report.protection.unreleased,
              std::vector<ObligationRef>{ref});

  const Result<ProtectedLoadObligation> released =
      release_through_engine(fixture, ref, ObligationRevision{1}, Tick{1020});
  UC_REQUIRE_STATUS(released, StatusCode::Ok);

  const Result<AttemptRecord> allowed = fixture.submit(CommandKind::IsolateOutput,
                                                       "key-isolate-allowed", Tick{1030},
                                                       "test-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(obligations, engine_refuses_lapsed_obligation_with_obligation_expired) {
  uc_test::Fixture fixture("obl-lapsed");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<ProtectedLoadObligation> bound = bind_through_engine(
      fixture,
      make_obligation("ob-window", ProtectionRequirement::MustRemainProtected, Tick{1000},
                      Tick{1100}),
      Tick{1000});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);
  const ObligationRef ref = bound.value().ref;

  const ControlCommand control = fixture.command(CommandKind::EnterMaintenanceBypass,
                                                 "key-lapsed", Tick{1200}, "test-authority");
  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(control);
  UC_REQUIRE_OK(evaluation);
  UC_CHECK(!evaluation.value().report.allowed);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::ObligationExpired);
  UC_CHECK_EQ(evaluation.value().report.protection.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(evaluation.value().report.protection.lapsed, std::vector<ObligationRef>{ref});
  UC_CHECK(evaluation.value().report.protection.binding.empty());
  UC_CHECK_EQ(evaluation.value().report.protection.unreleased, std::vector<ObligationRef>{ref});
  bool mentions_expiry_ref = false;
  for (const EvaluationFinding& finding : evaluation.value().report.findings) {
    if (finding.code == RefusalCode::ObligationExpired &&
        finding.detail.find(ref.value()) != std::string::npos) {
      mentions_expiry_ref = true;
    }
    UC_CHECK_NE(finding.code, RefusalCode::ObligationUnreleased);
  }
  UC_CHECK(mentions_expiry_ref);

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterMaintenanceBypass,
                                                       "key-lapsed", Tick{1201}, "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::ObligationExpired);
}

UC_TEST(obligations, engine_refuses_rebinding_a_released_reference_with_already_exists) {
  uc_test::Fixture fixture("obl-rebind");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const ProtectedLoadObligation obligation = make_obligation(
      "ob-audit", ProtectionRequirement::MustRemainProtected, Tick{1000}, std::nullopt);
  const Result<ProtectedLoadObligation> bound =
      bind_through_engine(fixture, obligation, Tick{1000});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);

  const Result<ProtectedLoadObligation> released =
      release_through_engine(fixture, obligation.ref, ObligationRevision{1}, Tick{1010});
  UC_REQUIRE_STATUS(released, StatusCode::Ok);

  const Result<ProtectedLoadObligation> rebound =
      bind_through_engine(fixture, obligation, Tick{1020});
  UC_REQUIRE_STATUS(rebound, StatusCode::AlreadyExists);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(unit.obligations.size() == std::size_t{1});
  UC_CHECK_EQ(unit.obligations.front().state, ObligationState::Released);
  UC_CHECK_EQ(unit.obligations.front().released_by.value(), ref_of("facility-safety"));
  UC_CHECK_EQ(unit.obligations.front().released_at.value(), Tick{1010});

  // A fresh reference for the same load is accepted; only the reference is spent.
  ProtectedLoadObligation replacement = obligation;
  replacement.ref = ObligationRef::parse("ob-audit-2").value();
  UC_REQUIRE_STATUS(bind_through_engine(fixture, replacement, Tick{1030}), StatusCode::Ok);
}

UC_TEST(obligations, engine_refuses_release_with_stale_obligation_revision) {
  uc_test::Fixture fixture("obl-stale-release");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const ProtectedLoadObligation obligation = make_obligation(
      "ob-revision", ProtectionRequirement::MustRemainProtected, Tick{1000}, std::nullopt);
  const Result<ProtectedLoadObligation> first =
      bind_through_engine(fixture, obligation, Tick{1000});
  UC_REQUIRE_STATUS(first, StatusCode::Ok);
  UC_CHECK_EQ(first.value().revision, ObligationRevision{1});

  const Result<ProtectedLoadObligation> reasserted =
      bind_through_engine(fixture, obligation, Tick{1010});
  UC_REQUIRE_STATUS(reasserted, StatusCode::Ok);
  UC_CHECK_EQ(reasserted.value().revision, ObligationRevision{2});

  const Result<ProtectedLoadObligation> stale =
      release_through_engine(fixture, obligation.ref, ObligationRevision{1}, Tick{1020});
  UC_REQUIRE_STATUS(stale, StatusCode::StaleSourceGeneration);

  const Result<ProtectedLoadObligation> current =
      release_through_engine(fixture, obligation.ref, ObligationRevision{2}, Tick{1030});
  UC_REQUIRE_STATUS(current, StatusCode::Ok);
  UC_CHECK_EQ(current.value().state, ObligationState::Released);
}

UC_TEST(obligations, engine_refuses_second_release_with_conflict) {
  uc_test::Fixture fixture("obl-double-release");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const ProtectedLoadObligation obligation = make_obligation(
      "ob-once", ProtectionRequirement::MustRemainProtected, Tick{1000}, std::nullopt);
  const Result<ProtectedLoadObligation> bound =
      bind_through_engine(fixture, obligation, Tick{1000});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);

  const Result<ProtectedLoadObligation> released =
      release_through_engine(fixture, obligation.ref, ObligationRevision{1}, Tick{1010});
  UC_REQUIRE_STATUS(released, StatusCode::Ok);
  UC_CHECK_EQ(released.value().revision, ObligationRevision{2});

  const Result<ProtectedLoadObligation> twice =
      release_through_engine(fixture, obligation.ref, ObligationRevision{2}, Tick{1020});
  UC_REQUIRE_STATUS(twice, StatusCode::Conflict);

  const Result<ProtectedLoadObligation> stale =
      release_through_engine(fixture, obligation.ref, ObligationRevision{1}, Tick{1030});
  UC_REQUIRE_STATUS(stale, StatusCode::StaleSourceGeneration);
}

UC_TEST(obligations, engine_reasserting_a_live_obligation_bumps_its_revision) {
  uc_test::Fixture fixture("obl-reassert");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const ProtectedLoadObligation obligation = make_obligation(
      "ob-live", ProtectionRequirement::MustRemainProtected, Tick{1000}, std::nullopt);
  const Result<ProtectedLoadObligation> first =
      bind_through_engine(fixture, obligation, Tick{1000});
  UC_REQUIRE_STATUS(first, StatusCode::Ok);
  UC_CHECK_EQ(first.value().revision, ObligationRevision{1});
  UC_CHECK_EQ(first.value().state, ObligationState::Active);

  const Result<ProtectedLoadObligation> second =
      bind_through_engine(fixture, obligation, Tick{1010});
  UC_REQUIRE_STATUS(second, StatusCode::Ok);
  UC_CHECK_EQ(second.value().state, ObligationState::Active);
  UC_CHECK_EQ(second.value().revision, ObligationRevision{2});
  UC_CHECK(!second.value().released_at.has_value());
  UC_CHECK(!second.value().released_by.has_value());

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(unit.obligations.size() == std::size_t{1});
  UC_CHECK_EQ(unit.obligations.front().ref, obligation.ref);
  UC_CHECK_EQ(unit.obligations.front().revision, ObligationRevision{2});
}

UC_TEST(obligations, engine_refuses_obligation_asserted_under_a_future_epoch) {
  uc_test::Fixture fixture("obl-future-epoch");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  // An omitted epoch is taken from the request authority, so the binding lands
  // under the store epoch.
  const Result<ProtectedLoadObligation> defaulted = bind_through_engine(
      fixture,
      make_obligation("ob-default-epoch", ProtectionRequirement::MustRemainProtected, Tick{1000},
                      std::nullopt),
      Tick{1000});
  UC_REQUIRE_STATUS(defaulted, StatusCode::Ok);
  UC_CHECK_EQ(defaulted.value().epoch, fixture.context.epoch);
  UC_CHECK_EQ(defaulted.value().incarnation, fixture.context.incarnation);

  ProtectedLoadObligation ahead = make_obligation(
      "ob-future-epoch", ProtectionRequirement::MustRemainProtected, Tick{1000}, std::nullopt);
  ahead.epoch = ControlEpoch{2};
  ahead.incarnation = Incarnation{2};
  UC_REQUIRE_STATUS(bind_through_engine(fixture, ahead, Tick{1010}), StatusCode::InvalidArgument);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(unit.obligations.size() == std::size_t{1});
  UC_CHECK_EQ(unit.obligations.front().ref, defaulted.value().ref);
}

UC_TEST(obligations, engine_allows_interruption_only_under_the_asserting_authority) {
  uc_test::Fixture fixture("obl-interrupt");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  // The load's asserting authority also holds a maintenance-entry grant, so the
  // citation is a real grant and not merely a matching identifier.
  UC_REQUIRE_STATUS(
      issue_through_engine_grant(fixture, "facility-safety", GrantScope::MaintenanceEntry,
                                 Tick{1005}),
      StatusCode::Ok);

  ProtectedLoadObligation obligation =
      make_obligation("ob-interruptible", ProtectionRequirement::MayBeInterruptedWithAuthority,
                      Tick{1000}, std::nullopt);
  const Result<ProtectedLoadObligation> bound =
      bind_through_engine(fixture, obligation, Tick{1010});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);
  const ObligationRef ref = bound.value().ref;

  // Citing a different authority does not cover the interruption.
  const Result<TransitionEvaluation> other = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "key-interrupt-other", Tick{1020},
                      "test-authority"));
  UC_REQUIRE_OK(other);
  UC_CHECK(!other.value().report.allowed);
  UC_CHECK_EQ(other.value().report.primary, RefusalCode::ObligationUnreleased);
  UC_CHECK_EQ(other.value().report.protection.impact, ProtectionImpact::DropsUndeclared);
  UC_CHECK_EQ(other.value().report.protection.unreleased, std::vector<ObligationRef>{ref});

  const Result<AttemptRecord> refused = fixture.submit(
      CommandKind::EnterMaintenanceBypass, "key-interrupt-other", Tick{1021}, "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::ObligationUnreleased);

  // The asserting authority itself covers the interruption.
  const Result<TransitionEvaluation> covered = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "key-interrupt-covered", Tick{1030},
                      "facility-safety"));
  UC_REQUIRE_OK(covered);
  UC_CHECK(covered.value().report.allowed);
  UC_CHECK_EQ(covered.value().report.primary, RefusalCode::None);
  UC_CHECK_EQ(covered.value().report.protection.impact, ProtectionImpact::ReducedWithAuthority);
  UC_CHECK_EQ(covered.value().report.protection.at_risk, std::vector<ObligationRef>{ref});
  UC_CHECK(covered.value().report.protection.unreleased.empty());

  const Result<AttemptRecord> allowed = fixture.submit(
      CommandKind::EnterMaintenanceBypass, "key-interrupt-covered", Tick{1031}, "facility-safety");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(obligations, engine_obligation_survives_reopen_and_still_blocks_transition) {
  uc_test::Fixture fixture("obl-reopen");
  UC_REQUIRE(fixture.build(GrantScope::MaintenanceEntry));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<ProtectedLoadObligation> bound = bind_through_engine(
      fixture,
      make_obligation("ob-durable", ProtectionRequirement::MustRemainProtected, Tick{1000},
                      std::nullopt),
      Tick{1000});
  UC_REQUIRE_STATUS(bound, StatusCode::Ok);
  const ObligationRef ref = bound.value().ref;

  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  const Result<std::shared_ptr<UpsControlEngine>> reopened =
      UpsControlEngine::open(fixture.open_options);
  UC_REQUIRE_OK(reopened);
  fixture.engine = reopened.value();
  UC_CHECK_EQ(fixture.engine->info().lifecycle, EngineLifecycle::Recovered);

  UC_REQUIRE_STATUS(fixture.revalidate(Tick{1050}), StatusCode::Ok);
  UC_CHECK_EQ(fixture.engine->info().lifecycle, EngineLifecycle::Revalidated);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(unit.obligations.size() == std::size_t{1});
  UC_CHECK_EQ(unit.obligations.front().ref, ref);
  UC_CHECK_EQ(unit.obligations.front().state, ObligationState::Active);
  UC_CHECK_EQ(unit.obligations.front().revision, ObligationRevision{1});
  UC_CHECK(!unit.obligations.front().released_at.has_value());

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterMaintenanceBypass,
                                                       "key-after-reopen", Tick{1060},
                                                       "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::ObligationUnreleased);
}
