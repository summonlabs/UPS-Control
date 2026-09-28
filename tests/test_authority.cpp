// Proof obligations for the authority model.
//
// Permission is separate from everything else: it comes from a grant issued
// under the current epoch and incarnation, it never comes from telemetry, a
// neighbouring scope is never substituted, and a revocation, expiry, or
// authority handoff fences the very next command.

#include "test_harness.hpp"

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

ControlContext make_context(std::uint64_t epoch, std::uint64_t incarnation) {
  return ControlContext{ControlEpoch{epoch}, Incarnation{incarnation}};
}

AuthorityGrant make_grant(const char* ref, GrantScope scope, Tick issued_at,
                          std::optional<Tick> expires_at, const ControlContext& issued_under) {
  AuthorityGrant grant;
  grant.ref = ref_of(ref);
  grant.scope = scope;
  grant.granted_by = ref_of("facility-ops");
  grant.epoch = issued_under.epoch;
  grant.incarnation = issued_under.incarnation;
  grant.issued_at = issued_at;
  grant.expires_at = expires_at;
  return grant;
}

/// A control command citing an explicit authority context and grant.
ControlCommand command_in(uc_test::Fixture& fixture, const ControlContext& context,
                          CommandKind kind, const std::string& key, Tick at,
                          const std::optional<AuthorityRef>& authority) {
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  ControlCommand control;
  control.authority = context;
  control.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  control.now = at;
  control.key = IdempotencyKey::parse(key).value();
  control.kind = kind;
  control.authority_ref = authority;
  return control;
}

Result<AuthorityGrant> issue_through_engine(uc_test::Fixture& fixture,
                                            const ControlContext& context,
                                            const AuthorityGrant& grant, Tick at) {
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  IssueGrantRequest request;
  request.authority = context;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = at;
  request.grant = grant;
  return fixture.engine->issue_grant(request);
}

Result<AuthorityGrant> revoke_through_engine(uc_test::Fixture& fixture, const AuthorityRef& grant,
                                             Tick at) {
  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  RevokeGrantRequest request;
  request.authority = fixture.context;
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = at;
  request.grant = grant;
  request.revocation_authority = ref_of("facility-ops");
  request.reason = "authority withdrawn by facility operations";
  return fixture.engine->revoke_grant(request);
}

}  // namespace

// ---------------------------------------------------------------------------
// assess_grant, one rule at a time
// ---------------------------------------------------------------------------

UC_TEST(authority, assess_grant_without_a_cited_reference_is_missing) {
  const ControlContext context = make_context(1, 1);
  const std::vector<AuthorityGrant> grants{
      make_grant("g-present", GrantScope::TestExecution, Tick{900}, std::nullopt, context)};

  const GrantAssessment uncited =
      assess_grant(grants, std::nullopt, GrantScope::TestExecution, context, Tick{1000});
  UC_CHECK_EQ(uncited.verdict, GrantVerdict::Missing);
  UC_CHECK(!uncited.grant.has_value());

  const GrantAssessment unknown =
      assess_grant(grants, ref_of("g-absent"), GrantScope::TestExecution, context, Tick{1000});
  UC_CHECK_EQ(unknown.verdict, GrantVerdict::Missing);
  UC_CHECK_EQ(unknown.grant.value(), ref_of("g-absent"));
}

UC_TEST(authority, assess_grant_neighbouring_scope_is_never_substituted) {
  const ControlContext context = make_context(1, 1);
  const std::vector<AuthorityGrant> grants{
      make_grant("g-bypass", GrantScope::BypassTransfer, Tick{900}, std::nullopt, context),
      make_grant("g-maintenance", GrantScope::MaintenanceEntry, Tick{900}, std::nullopt, context)};

  const GrantAssessment outer = assess_grant(grants, ref_of("g-bypass"),
                                             GrantScope::MaintenanceEntry, context, Tick{1000});
  UC_CHECK_EQ(outer.verdict, GrantVerdict::OutOfScope);
  UC_CHECK(outer.detail.find("maintenance_entry") != std::string::npos);

  const GrantAssessment inner = assess_grant(grants, ref_of("g-maintenance"),
                                             GrantScope::BypassTransfer, context, Tick{1000});
  UC_CHECK_EQ(inner.verdict, GrantVerdict::OutOfScope);
  UC_CHECK(inner.detail.find("bypass_transfer") != std::string::npos);

  UC_CHECK_EQ(assess_grant(grants, ref_of("g-bypass"), GrantScope::BypassTransfer, context,
                           Tick{1000})
                  .verdict,
              GrantVerdict::Allowed);
}

UC_TEST(authority, assess_grant_revoked_is_never_allowed) {
  const ControlContext context = make_context(1, 1);
  AuthorityGrant grant =
      make_grant("g-revoked", GrantScope::TestExecution, Tick{900}, std::nullopt, context);
  grant.revoked_by = ref_of("facility-ops");
  grant.revoked_at = Tick{950};

  const GrantAssessment assessment =
      assess_grant({grant}, ref_of("g-revoked"), GrantScope::TestExecution, context, Tick{1000});
  UC_CHECK_EQ(assessment.verdict, GrantVerdict::Revoked);
  UC_CHECK(assessment.detail.find("revoked") != std::string::npos);
}

UC_TEST(authority, assess_grant_fenced_by_epoch_or_incarnation) {
  const std::vector<AuthorityGrant> grants{
      make_grant("g-old-epoch", GrantScope::TestExecution, Tick{900}, std::nullopt,
                 make_context(1, 1)),
      make_grant("g-old-incarnation", GrantScope::TestExecution, Tick{900}, std::nullopt,
                 make_context(2, 1))};

  UC_CHECK_EQ(assess_grant(grants, ref_of("g-old-epoch"), GrantScope::TestExecution,
                           make_context(2, 1), Tick{1000})
                  .verdict,
              GrantVerdict::Fenced);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-old-incarnation"), GrantScope::TestExecution,
                           make_context(2, 2), Tick{1000})
                  .verdict,
              GrantVerdict::Fenced);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-old-epoch"), GrantScope::TestExecution,
                           make_context(1, 1), Tick{1000})
                  .verdict,
              GrantVerdict::Allowed);
}

UC_TEST(authority, assess_grant_validity_window_is_half_open) {
  const ControlContext context = make_context(1, 1);
  const std::vector<AuthorityGrant> windowed{
      make_grant("g-window", GrantScope::TestExecution, Tick{1000}, Tick{1500}, context)};
  const std::vector<AuthorityGrant> open{
      make_grant("g-open", GrantScope::TestExecution, Tick{1000}, std::nullopt, context)};

  UC_CHECK_EQ(assess_grant(windowed, ref_of("g-window"), GrantScope::TestExecution, context,
                           Tick{1000})
                  .verdict,
              GrantVerdict::Allowed);
  UC_CHECK_EQ(assess_grant(windowed, ref_of("g-window"), GrantScope::TestExecution, context,
                           Tick{1499})
                  .verdict,
              GrantVerdict::Allowed);
  UC_CHECK_EQ(assess_grant(windowed, ref_of("g-window"), GrantScope::TestExecution, context,
                           Tick{1500})
                  .verdict,
              GrantVerdict::Expired);
  UC_CHECK_EQ(assess_grant(windowed, ref_of("g-window"), GrantScope::TestExecution, context,
                           Tick{1501})
                  .verdict,
              GrantVerdict::Expired);
  UC_CHECK_EQ(assess_grant(open, ref_of("g-open"), GrantScope::TestExecution, context, Tick{999999})
                  .verdict,
              GrantVerdict::Allowed);
}

UC_TEST(authority, assess_grant_explicit_denial_is_distinct_from_missing) {
  const ControlContext context = make_context(1, 1);
  AuthorityGrant grant =
      make_grant("g-denied", GrantScope::TestExecution, Tick{900}, std::nullopt, context);
  grant.allowed = false;

  const GrantAssessment denied =
      assess_grant({grant}, ref_of("g-denied"), GrantScope::TestExecution, context, Tick{1000});
  UC_CHECK_EQ(denied.verdict, GrantVerdict::Denied);

  const GrantAssessment missing =
      assess_grant({grant}, ref_of("g-never-issued"), GrantScope::TestExecution, context, Tick{1000});
  UC_CHECK_EQ(missing.verdict, GrantVerdict::Missing);
}

UC_TEST(authority, assess_grant_precedence_is_fixed_when_several_rules_are_violated) {
  const ControlContext context = make_context(2, 1);
  const Tick now{1000};

  AuthorityGrant out_of_scope =
      make_grant("g-scope", GrantScope::BypassTransfer, Tick{900}, Tick{950}, context);
  out_of_scope.revoked_by = ref_of("facility-ops");
  out_of_scope.revoked_at = Tick{960};
  out_of_scope.allowed = false;

  AuthorityGrant revoked =
      make_grant("g-revoked", GrantScope::TestExecution, Tick{900}, Tick{950}, context);
  revoked.revoked_by = ref_of("facility-ops");
  revoked.revoked_at = Tick{960};
  revoked.allowed = false;

  AuthorityGrant fenced =
      make_grant("g-fenced", GrantScope::TestExecution, Tick{900}, Tick{950}, make_context(1, 1));
  fenced.allowed = false;

  AuthorityGrant expired =
      make_grant("g-expired", GrantScope::TestExecution, Tick{900}, Tick{950}, context);
  expired.allowed = false;

  AuthorityGrant denied =
      make_grant("g-denied", GrantScope::TestExecution, Tick{900}, std::nullopt, context);
  denied.allowed = false;

  const AuthorityGrant allowed =
      make_grant("g-allowed", GrantScope::TestExecution, Tick{900}, std::nullopt, context);

  const std::vector<AuthorityGrant> grants{out_of_scope, revoked, fenced, expired, denied, allowed};

  UC_CHECK_EQ(assess_grant(grants, ref_of("g-scope"), GrantScope::TestExecution, context, now)
                  .verdict,
              GrantVerdict::OutOfScope);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-revoked"), GrantScope::TestExecution, context, now)
                  .verdict,
              GrantVerdict::Revoked);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-fenced"), GrantScope::TestExecution, context, now)
                  .verdict,
              GrantVerdict::Fenced);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-expired"), GrantScope::TestExecution, context, now)
                  .verdict,
              GrantVerdict::Expired);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-denied"), GrantScope::TestExecution, context, now)
                  .verdict,
              GrantVerdict::Denied);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-allowed"), GrantScope::TestExecution, context, now)
                  .verdict,
              GrantVerdict::Allowed);
  UC_CHECK_EQ(assess_grant(grants, ref_of("g-allowed"), GrantScope::TestExecution,
                           make_context(1, 1), now)
                  .verdict,
              GrantVerdict::Fenced);
}

// ---------------------------------------------------------------------------
// validate_grant
// ---------------------------------------------------------------------------

UC_TEST(authority, validate_grant_refuses_expiry_not_strictly_after_issue) {
  const AuthorityGrant base =
      make_grant("g-valid", GrantScope::TestExecution, Tick{100}, Tick{200}, make_context(1, 1));
  UC_REQUIRE_STATUS(validate_grant(base, kLimits), StatusCode::Ok);

  AuthorityGrant equal = base;
  equal.expires_at = Tick{100};
  UC_REQUIRE_STATUS(validate_grant(equal, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant before = base;
  before.expires_at = Tick{99};
  UC_REQUIRE_STATUS(validate_grant(before, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant origin = base;
  origin.expires_at = Tick{0};
  UC_REQUIRE_STATUS(validate_grant(origin, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant no_issue = base;
  no_issue.issued_at = Tick{0};
  UC_REQUIRE_STATUS(validate_grant(no_issue, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant negative_issue = base;
  negative_issue.issued_at = Tick{-3};
  UC_REQUIRE_STATUS(validate_grant(negative_issue, kLimits), StatusCode::InvalidArgument);
}

UC_TEST(authority, validate_grant_refuses_incomplete_revocation_record) {
  const AuthorityGrant base =
      make_grant("g-revoke", GrantScope::TestExecution, Tick{100}, std::nullopt, make_context(1, 1));
  UC_REQUIRE_STATUS(validate_grant(base, kLimits), StatusCode::Ok);

  AuthorityGrant no_authority = base;
  no_authority.revoked_at = Tick{150};
  UC_REQUIRE_STATUS(validate_grant(no_authority, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant origin_instant = base;
  origin_instant.revoked_by = ref_of("facility-ops");
  origin_instant.revoked_at = Tick{0};
  UC_REQUIRE_STATUS(validate_grant(origin_instant, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant empty_authority = base;
  empty_authority.revoked_by = AuthorityRef{};
  empty_authority.revoked_at = Tick{150};
  UC_REQUIRE_STATUS(validate_grant(empty_authority, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant complete = base;
  complete.revoked_by = ref_of("facility-ops");
  complete.revoked_at = Tick{150};
  UC_REQUIRE_STATUS(validate_grant(complete, kLimits), StatusCode::Ok);
}

UC_TEST(authority, validate_grant_refuses_empty_reference_and_granting_authority) {
  const AuthorityGrant base =
      make_grant("g-valid", GrantScope::TestExecution, Tick{100}, std::nullopt, make_context(1, 1));

  AuthorityGrant empty_ref = base;
  empty_ref.ref = AuthorityRef{};
  UC_REQUIRE_STATUS(validate_grant(empty_ref, kLimits), StatusCode::InvalidArgument);

  AuthorityGrant empty_granter = base;
  empty_granter.granted_by = AuthorityRef{};
  UC_REQUIRE_STATUS(validate_grant(empty_granter, kLimits), StatusCode::InvalidArgument);
}

UC_TEST(authority, validate_grant_refuses_negative_and_oversized_reserve_floor) {
  AuthorityGrant floor =
      make_grant("g-floor", GrantScope::DischargeEnable, Tick{100}, std::nullopt, make_context(1, 1));
  floor.reserve_floor = ReserveQuantity{ReserveUnit::Seconds, -1};
  UC_REQUIRE_STATUS(validate_grant(floor, kLimits), StatusCode::InvalidArgument);

  floor.reserve_floor = ReserveQuantity{ReserveUnit::Seconds, kLimits.max_reserve_seconds + 1};
  UC_REQUIRE_STATUS(validate_grant(floor, kLimits), StatusCode::LimitExceeded);

  floor.reserve_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
  UC_REQUIRE_STATUS(validate_grant(floor, kLimits), StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Through the engine
// ---------------------------------------------------------------------------

UC_TEST(authority, engine_refuses_command_without_cited_authority) {
  uc_test::Fixture fixture("auth-missing");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  ControlCommand control = fixture.command(CommandKind::EnterStaticBypass, "key-no-authority",
                                           Tick{1010}, "test-authority");
  control.authority_ref.reset();

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(control);
  UC_REQUIRE_OK(evaluation);
  UC_CHECK(!evaluation.value().report.allowed);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::AuthorityMissing);
  UC_CHECK_EQ(evaluation.value().report.grant.verdict, GrantVerdict::Missing);
  UC_CHECK(!evaluation.value().report.grant.grant.has_value());

  const Result<AttemptRecord> refused = fixture.engine->submit(control);
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityMissing);

  // The same request with the grant cited is authorized, so the refusal was
  // about the missing citation and nothing else.
  const Result<AttemptRecord> allowed = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-with-authority", Tick{1020},
                                                       "test-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(authority, engine_refuses_command_citing_an_unissued_authority) {
  uc_test::Fixture fixture("auth-unissued");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-unissued", Tick{1010},
                                                       "never-issued-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityMissing);

  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, fixture.context,
                           make_grant("never-issued-authority", GrantScope::BypassTransfer,
                                      Tick{1020}, std::nullopt, fixture.context),
                           Tick{1020}),
      StatusCode::Ok);

  const Result<AttemptRecord> allowed = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-issued-now", Tick{1030},
                                                       "never-issued-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(authority, engine_neighbouring_scope_does_not_authorize_maintenance_entry) {
  uc_test::Fixture fixture("auth-scope");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "key-scope-evaluate", Tick{1010},
                      "test-authority"));
  UC_REQUIRE_OK(evaluation);
  UC_CHECK(!evaluation.value().report.allowed);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::AuthorityMissing);
  UC_CHECK_EQ(evaluation.value().report.grant.verdict, GrantVerdict::OutOfScope);
  UC_CHECK(evaluation.value().report.primary_detail.find("maintenance_entry") != std::string::npos);

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterMaintenanceBypass,
                                                       "key-scope-refused", Tick{1011},
                                                       "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityMissing);
  UC_CHECK(refused.value().refusal->detail.find("maintenance_entry") != std::string::npos);

  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, fixture.context,
                           make_grant("maintenance-authority", GrantScope::MaintenanceEntry,
                                      Tick{1020}, std::nullopt, fixture.context),
                           Tick{1020}),
      StatusCode::Ok);

  const Result<AttemptRecord> allowed = fixture.submit(
      CommandKind::EnterMaintenanceBypass, "key-scope-allowed", Tick{1030},
      "maintenance-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(authority, engine_refuses_next_command_after_grant_revocation) {
  uc_test::Fixture fixture("auth-revoked");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<AuthorityGrant> revoked =
      revoke_through_engine(fixture, ref_of("test-authority"), Tick{1010});
  UC_REQUIRE_STATUS(revoked, StatusCode::Ok);
  UC_REQUIRE(revoked.value().revoked_at.has_value());
  UC_CHECK_EQ(revoked.value().revoked_at.value(), Tick{1010});
  UC_CHECK_EQ(revoked.value().revoked_by.value(), ref_of("facility-ops"));

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-after-revocation", Tick{1020},
                                                       "test-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityRevoked);
}

UC_TEST(authority, engine_refuses_expired_grant) {
  uc_test::Fixture fixture("auth-expired");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, fixture.context,
                           make_grant("expiring-authority", GrantScope::BypassTransfer, Tick{1000},
                                      Tick{1001}, fixture.context),
                           Tick{1000}),
      StatusCode::Ok);

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterStaticBypass, "key-expired-evaluate", Tick{1005},
                      "expiring-authority"));
  UC_REQUIRE_OK(evaluation);
  UC_CHECK_EQ(evaluation.value().report.grant.verdict, GrantVerdict::Expired);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::AuthorityExpired);

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-expired", Tick{1005},
                                                       "expiring-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityExpired);
}

UC_TEST(authority, engine_refuses_second_live_grant_with_same_reference) {
  uc_test::Fixture fixture("auth-duplicate");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, fixture.context,
                           make_grant("test-authority", GrantScope::BypassTransfer, Tick{1010},
                                      std::nullopt, fixture.context),
                           Tick{1010}),
      StatusCode::AlreadyExists);

  // The collision is on the reference, not on the scope.
  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, fixture.context,
                           make_grant("test-authority", GrantScope::MaintenanceEntry, Tick{1020},
                                      std::nullopt, fixture.context),
                           Tick{1020}),
      StatusCode::AlreadyExists);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  UC_REQUIRE(unit.grants.size() == std::size_t{1});
  UC_CHECK_EQ(unit.grants.front().scope, GrantScope::BypassTransfer);
}

UC_TEST(authority, engine_reissue_after_revocation_bumps_grant_revision) {
  uc_test::Fixture fixture("auth-reissue");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const Result<AuthorityGrant> revoked =
      revoke_through_engine(fixture, ref_of("test-authority"), Tick{1010});
  UC_REQUIRE_STATUS(revoked, StatusCode::Ok);
  UC_CHECK_EQ(revoked.value().revision, GrantRevision{2});

  const Result<AuthorityGrant> reissued =
      issue_through_engine(fixture, fixture.context,
                           make_grant("test-authority", GrantScope::BypassTransfer, Tick{1020},
                                      std::nullopt, fixture.context),
                           Tick{1020});
  UC_REQUIRE_STATUS(reissued, StatusCode::Ok);
  UC_CHECK_EQ(reissued.value().revision, GrantRevision{3});
  UC_CHECK(!reissued.value().revoked_at.has_value());
  UC_CHECK(!reissued.value().revoked_by.has_value());

  UC_CHECK_EQ(assess_grant({reissued.value()}, ref_of("test-authority"), GrantScope::BypassTransfer,
                           fixture.context, Tick{1030})
                  .verdict,
              GrantVerdict::Allowed);

  const Result<AttemptRecord> allowed = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-reissued", Tick{1030},
                                                       "test-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(authority, engine_authority_handoff_moves_strictly_forward) {
  uc_test::Fixture fixture("auth-handoff");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const auto adopt = [&fixture](const ControlContext& from, std::uint64_t epoch,
                                std::uint64_t incarnation, Tick at) {
    AdoptAuthorityRequest request;
    request.authority = from;
    request.now = at;
    request.epoch = ControlEpoch{epoch};
    request.incarnation = Incarnation{incarnation};
    return fixture.engine->adopt_authority(request);
  };

  // An equal pair is not a handoff.
  UC_REQUIRE_STATUS(adopt(fixture.context, 1, 1, Tick{1010}), StatusCode::StaleAuthority);
  // A zero epoch or incarnation is not an authority context at all.
  UC_REQUIRE_STATUS(adopt(fixture.context, 0, 2, Tick{1011}), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(adopt(fixture.context, 2, 0, Tick{1012}), StatusCode::InvalidArgument);

  // A greater epoch is forward.
  const Result<ControlContext> forward = adopt(fixture.context, 2, 1, Tick{1020});
  UC_REQUIRE_STATUS(forward, StatusCode::Ok);
  UC_CHECK_EQ(forward.value().epoch, ControlEpoch{2});
  UC_CHECK_EQ(forward.value().incarnation, Incarnation{1});

  // An older epoch is a rollback, even with a much greater incarnation.
  UC_REQUIRE_STATUS(adopt(forward.value(), 1, 9, Tick{1030}), StatusCode::StaleAuthority);

  // The same epoch with a greater incarnation is forward.
  const Result<ControlContext> bumped = adopt(forward.value(), 2, 3, Tick{1040});
  UC_REQUIRE_STATUS(bumped, StatusCode::Ok);
  UC_CHECK_EQ(bumped.value().epoch, ControlEpoch{2});
  UC_CHECK_EQ(bumped.value().incarnation, Incarnation{3});

  // The same epoch with an older incarnation is a rollback.
  UC_REQUIRE_STATUS(adopt(bumped.value(), 2, 2, Tick{1050}), StatusCode::StaleAuthority);
  UC_REQUIRE_STATUS(adopt(bumped.value(), 2, 3, Tick{1060}), StatusCode::StaleAuthority);

  const EngineInfo info = fixture.engine->info();
  UC_CHECK_EQ(info.epoch, ControlEpoch{2});
  UC_CHECK_EQ(info.incarnation, Incarnation{3});
}

UC_TEST(authority, engine_refuses_old_epoch_incarnation_and_grant_after_handoff) {
  uc_test::Fixture fixture("auth-after-handoff");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  AdoptAuthorityRequest handoff;
  handoff.authority = fixture.context;
  handoff.now = Tick{1010};
  handoff.epoch = ControlEpoch{2};
  handoff.incarnation = Incarnation{3};
  UC_REQUIRE_STATUS(fixture.engine->adopt_authority(handoff), StatusCode::Ok);
  const ControlContext current = make_context(2, 3);

  const Result<AttemptRecord> old_epoch =
      fixture.engine->submit(command_in(fixture, fixture.context, CommandKind::EnterStaticBypass,
                                        "key-old-epoch", Tick{1020}, ref_of("test-authority")));
  UC_REQUIRE_OK(old_epoch);
  UC_CHECK_EQ(old_epoch.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(old_epoch.value().refusal.has_value());
  UC_CHECK_EQ(old_epoch.value().refusal->code, RefusalCode::StaleEpoch);

  const Result<AttemptRecord> old_incarnation =
      fixture.engine->submit(command_in(fixture, make_context(2, 1), CommandKind::EnterStaticBypass,
                                        "key-old-incarnation", Tick{1030},
                                        ref_of("test-authority")));
  UC_REQUIRE_OK(old_incarnation);
  UC_CHECK_EQ(old_incarnation.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(old_incarnation.value().refusal.has_value());
  UC_CHECK_EQ(old_incarnation.value().refusal->code, RefusalCode::StaleIncarnation);

  const Result<TransitionEvaluation> fenced_evaluation =
      fixture.engine->evaluate(command_in(fixture, current, CommandKind::EnterStaticBypass,
                                          "key-fenced-evaluate", Tick{1040},
                                          ref_of("test-authority")));
  UC_REQUIRE_OK(fenced_evaluation);
  UC_CHECK_EQ(fenced_evaluation.value().report.grant.verdict, GrantVerdict::Fenced);
  UC_CHECK_EQ(fenced_evaluation.value().report.primary, RefusalCode::AuthorityFenced);

  const Result<AttemptRecord> fenced = fixture.engine->submit(
      command_in(fixture, current, CommandKind::EnterStaticBypass, "key-fenced-grant", Tick{1041},
                 ref_of("test-authority")));
  UC_REQUIRE_OK(fenced);
  UC_CHECK_EQ(fenced.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(fenced.value().refusal.has_value());
  UC_CHECK_EQ(fenced.value().refusal->code, RefusalCode::AuthorityFenced);

  // A grant issued under the old epoch cannot be recorded at all.
  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, current,
                           make_grant("stale-authority", GrantScope::BypassTransfer, Tick{1050},
                                      std::nullopt, fixture.context),
                           Tick{1050}),
      StatusCode::StaleAuthority);

  // A grant issued under the current context authorizes the very same command.
  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, current,
                           make_grant("current-authority", GrantScope::BypassTransfer, Tick{1060},
                                      std::nullopt, current),
                           Tick{1060}),
      StatusCode::Ok);
  const Result<AttemptRecord> allowed = fixture.engine->submit(
      command_in(fixture, current, CommandKind::EnterStaticBypass, "key-current-authority",
                 Tick{1070}, ref_of("current-authority")));
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}

UC_TEST(authority, engine_refuses_explicitly_denied_scope_with_authority_denied) {
  uc_test::Fixture fixture("auth-denied");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  AuthorityGrant denial = make_grant("denial-authority", GrantScope::BypassTransfer, Tick{1010},
                                     std::nullopt, fixture.context);
  denial.allowed = false;
  UC_REQUIRE_STATUS(issue_through_engine(fixture, fixture.context, denial, Tick{1010}),
                    StatusCode::Ok);

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterStaticBypass, "key-denied-evaluate", Tick{1020},
                      "denial-authority"));
  UC_REQUIRE_OK(evaluation);
  UC_CHECK_EQ(evaluation.value().report.grant.verdict, GrantVerdict::Denied);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::AuthorityDenied);

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnterStaticBypass,
                                                       "key-denied", Tick{1020},
                                                       "denial-authority");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityDenied);

  // A denial covers exactly its scope: the neighbouring maintenance scope is not
  // denied by it, it is uncovered.
  const Result<TransitionEvaluation> other = fixture.engine->evaluate(
      fixture.command(CommandKind::EnterMaintenanceBypass, "key-denied-neighbour", Tick{1030},
                      "denial-authority"));
  UC_REQUIRE_OK(other);
  UC_CHECK_EQ(other.value().report.grant.verdict, GrantVerdict::OutOfScope);
  UC_CHECK_EQ(other.value().report.primary, RefusalCode::AuthorityMissing);
}

UC_TEST(authority, engine_discharge_grant_floor_is_used_as_an_authority_floor) {
  uc_test::Fixture fixture("auth-discharge-floor");
  UC_REQUIRE(fixture.build(GrantScope::DischargeEnable));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  // Remove the unit's own floor: the only floor left has to come from the grant.
  const UpsRecord before = UC_REQUIRE_OK(fixture.current());
  SetReservePolicyRequest policy_request;
  policy_request.authority = fixture.context;
  policy_request.ref = UpsRef{before.id, before.hardware, before.revision};
  policy_request.now = Tick{1005};
  policy_request.policy.max_evidence_age = TickSpan{600};
  UC_REQUIRE_STATUS(fixture.engine->set_reserve_policy(policy_request), StatusCode::Ok);
  const UpsRecord policied = UC_REQUIRE_OK(fixture.current());
  UC_CHECK(!policied.reserve_policy.discharge_floor.has_value());

  // Without a floor anywhere the runtime refuses instead of assuming safety.
  const Result<AttemptRecord> unfloored = fixture.submit(CommandKind::EnableDischarge,
                                                         "key-discharge-unfloored", Tick{1010},
                                                         "test-authority");
  UC_REQUIRE_OK(unfloored);
  UC_CHECK_EQ(unfloored.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(unfloored.value().refusal.has_value());
  UC_CHECK_EQ(unfloored.value().refusal->code, RefusalCode::ReserveRequirementMissing);

  // The grant declares a floor above the observed 900 s reserve.
  AuthorityGrant floored = make_grant("discharge-floor", GrantScope::DischargeEnable, Tick{1020},
                                      std::nullopt, fixture.context);
  floored.reserve_floor = ReserveQuantity{ReserveUnit::Seconds, 1000};
  UC_REQUIRE_STATUS(issue_through_engine(fixture, fixture.context, floored, Tick{1020}),
                    StatusCode::Ok);

  const Result<TransitionEvaluation> evaluation = fixture.engine->evaluate(
      fixture.command(CommandKind::EnableDischarge, "key-discharge-floored-evaluate", Tick{1030},
                      "discharge-floor"));
  UC_REQUIRE_OK(evaluation);
  UC_CHECK_EQ(evaluation.value().report.primary, RefusalCode::ReserveInsufficient);
  UC_REQUIRE(evaluation.value().report.reserve.has_value());
  UC_CHECK_EQ(evaluation.value().report.reserve->outcome, ReserveOutcome::Insufficient);
  UC_CHECK_EQ(evaluation.value().report.reserve->observed.value(),
              (ReserveQuantity{ReserveUnit::Seconds, 900}));
  UC_CHECK_EQ(evaluation.value().report.reserve->required.value(),
              (ReserveQuantity{ReserveUnit::Seconds, 1000}));

  const Result<AttemptRecord> refused = fixture.submit(CommandKind::EnableDischarge,
                                                       "key-discharge-floored", Tick{1030},
                                                       "discharge-floor");
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::ReserveInsufficient);
}

UC_TEST(authority, engine_telemetry_never_grants_recharge_authority) {
  uc_test::Fixture fixture("auth-telemetry");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer));
  UC_CHECK_MSG(fixture.last_error.empty(), fixture.last_error);

  const UpsRecord unit = UC_REQUIRE_OK(fixture.current());
  TelemetryReport report = uc_test::healthy_report(unit.id, unit.hardware, Tick{1010},
                                                   SourceRevision{2}, "ev-recharge-enabled");
  report.capability.recharge_enabled = Bool3::True;

  RecordTelemetryRequest telemetry;
  telemetry.authority = fixture.context;
  telemetry.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  telemetry.now = Tick{1010};
  telemetry.report = report;
  const Result<ObservationRecord> observed = fixture.engine->record_telemetry(telemetry);
  UC_REQUIRE_STATUS(observed, StatusCode::Ok);
  UC_CHECK_EQ(observed.value().capability.recharge_enabled, Bool3::True);

  const Result<ReadinessReport> readiness =
      fixture.engine->readiness(UpsQuery{unit.id, Tick{1020}});
  UC_REQUIRE_OK(readiness);
  UC_CHECK(!readiness.value().recharge_authorized);

  ControlCommand control = fixture.command(CommandKind::EnableRecharge, "key-recharge-telemetry",
                                           Tick{1030}, "test-authority");
  control.authority_ref.reset();
  const Result<AttemptRecord> refused = fixture.engine->submit(control);
  UC_REQUIRE_OK(refused);
  UC_CHECK_EQ(refused.value().phase, AttemptPhase::Refused);
  UC_REQUIRE(refused.value().refusal.has_value());
  UC_CHECK_EQ(refused.value().refusal->code, RefusalCode::AuthorityMissing);

  // Only a grant of the recharge scope authorizes the operation.
  UC_REQUIRE_STATUS(
      issue_through_engine(fixture, fixture.context,
                           make_grant("recharge-authority", GrantScope::RechargeEnable, Tick{1040},
                                      std::nullopt, fixture.context),
                           Tick{1040}),
      StatusCode::Ok);

  const Result<ReadinessReport> readiness_after =
      fixture.engine->readiness(UpsQuery{unit.id, Tick{1050}});
  UC_REQUIRE_OK(readiness_after);
  UC_CHECK(readiness_after.value().recharge_authorized);

  const Result<AttemptRecord> allowed = fixture.submit(CommandKind::EnableRecharge,
                                                       "key-recharge-granted", Tick{1050},
                                                       "recharge-authority");
  UC_REQUIRE_OK(allowed);
  UC_CHECK(!allowed.value().refusal.has_value());
  UC_CHECK_EQ(allowed.value().phase, AttemptPhase::Acknowledged);
}
