// Stale and unknown reserve evidence, and a fenced authority, all refuse rather
// than being promoted to permission.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "ups_control/engine.hpp"

namespace {

using namespace ups_control;

int fail(const Status& status) {
  std::cerr << "error: " << status.to_string() << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path store_path =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("example-stale.upsstore");
  std::error_code error;
  std::filesystem::remove(store_path, error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".g1"), error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".lock"), error);

  const Tick now{3000};
  const ControlContext context{ControlEpoch{11}, Incarnation{2}};

  EngineOpenOptions open_options;
  open_options.store.path = store_path;
  open_options.store.access = StoreAccess::ReadWrite;
  open_options.store.create_if_missing = true;
  open_options.store.created_at = now;
  open_options.store.epoch = context.epoch;
  open_options.store.incarnation = context.incarnation;
  open_options.adapter = std::make_shared<SyntheticAdapter>(SyntheticAdapter::Script{});

  Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
  if (!opened.ok()) {
    return fail(opened.status());
  }
  std::shared_ptr<UpsControlEngine> engine = opened.value();

  RevalidateRequest revalidate;
  revalidate.authority = context;
  revalidate.now = now;
  if (!engine->revalidate(revalidate).ok()) {
    return fail(engine->revalidate(revalidate).status());
  }

  RegisterUpsRequest registration;
  registration.authority = context;
  registration.now = now;
  registration.id = UpsId::parse("ups-c3").value();
  registration.hardware = HardwareGeneration{2};
  registration.lifecycle = LifecycleState::InService;
  registration.operating = OperatingState::OnlineNormal;
  registration.policy.max_evidence_age = TickSpan{60};
  registration.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
  const Result<UpsRecord> registered = engine->register_ups(registration);
  if (!registered.ok()) {
    return fail(registered.status());
  }

  // Unknown reserve: an explicit unknown, never zero.
  TelemetryReport unknown = TelemetryReport{};
  unknown.evidence = EvidenceId::parse("ev-unknown").value();
  unknown.ups = registered.value().id;
  unknown.hardware = registered.value().hardware;
  unknown.source = SourceId::parse("device-poll").value();
  unknown.provenance = Provenance::SyntheticAdapter;
  unknown.revision = SourceRevision{1};
  unknown.observed_at = now;
  unknown.received_at = now;
  unknown.operating = OperatingState::OnlineNormal;
  unknown.battery.reserve = unknown_reserve(UnknownReason::SensorFault);

  RecordTelemetryRequest unknown_request{context,
                                         UpsRef{registered.value().id, registered.value().hardware,
                                                registered.value().revision},
                                         now, unknown};
  if (!engine->record_telemetry(unknown_request).ok()) {
    return fail(engine->record_telemetry(unknown_request).status());
  }
  const Result<UpsRecord> after_unknown = engine->units().ok()
                                              ? Result<UpsRecord>((*engine->units()).front())
                                              : Result<UpsRecord>(engine->units().status());

  IssueGrantRequest test_grant;
  test_grant.authority = context;
  test_grant.ref = UpsRef{after_unknown.value().id, after_unknown.value().hardware,
                          after_unknown.value().revision};
  test_grant.now = now;
  test_grant.grant.ref = AuthorityRef::parse("authority-test").value();
  test_grant.grant.scope = GrantScope::TestExecution;
  test_grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
  test_grant.grant.issued_at = now;
  test_grant.grant.expires_at = Tick{90000};
  if (!engine->issue_grant(test_grant).ok()) {
    return fail(engine->issue_grant(test_grant).status());
  }

  const UpsRecord after_grant = (*engine->units()).front();
  ControlCommand test_command;
  test_command.authority = context;
  test_command.ref = UpsRef{after_grant.id, after_grant.hardware, after_grant.revision};
  test_command.now = now + 1;
  test_command.key = IdempotencyKey::parse("battery-test-1").value();
  test_command.kind = CommandKind::StartBatteryTest;
  test_command.authority_ref = AuthorityRef::parse("authority-test").value();

  const Result<TransitionEvaluation> unknown_evaluation = engine->evaluate(test_command);
  if (!unknown_evaluation.ok()) {
    return fail(unknown_evaluation.status());
  }
  std::cout << "unknown_reserve verdict="
            << (unknown_evaluation.value().report.allowed ? "allowed" : "refused")
            << " primary=" << to_string(unknown_evaluation.value().report.primary) << "\n";
  if (unknown_evaluation.value().report.reserve.has_value()) {
    std::cout << "reserve_reason=" << to_string(unknown_evaluation.value().report.reserve->reason)
              << "\n";
    std::cout << "reserve_detail=" << unknown_evaluation.value().report.reserve->detail << "\n";
  }

  // A fresh, known reserve below the floor is insufficient, not indeterminate.
  TelemetryReport low = unknown;
  low.evidence = EvidenceId::parse("ev-low").value();
  low.revision = SourceRevision{2};
  low.observed_at = now + 2;
  low.received_at = now + 2;
  low.battery.reserve = known_reserve(ReserveQuantity{ReserveUnit::Seconds, 30}, EvidenceQuality::Measured);
  const UpsRecord before_low = (*engine->units()).front();
  RecordTelemetryRequest low_request{context,
                                     UpsRef{before_low.id, before_low.hardware, before_low.revision},
                                     now + 2, low};
  if (!engine->record_telemetry(low_request).ok()) {
    return fail(engine->record_telemetry(low_request).status());
  }
  const UpsRecord after_low = (*engine->units()).front();
  ControlCommand low_command = test_command;
  low_command.ref = UpsRef{after_low.id, after_low.hardware, after_low.revision};
  low_command.now = now + 3;
  low_command.key = IdempotencyKey::parse("battery-test-2").value();
  const Result<TransitionEvaluation> low_evaluation = engine->evaluate(low_command);
  if (!low_evaluation.ok()) {
    return fail(low_evaluation.status());
  }
  std::cout << "low_reserve verdict="
            << (low_evaluation.value().report.allowed ? "allowed" : "refused")
            << " primary=" << to_string(low_evaluation.value().report.primary) << "\n";

  // The same evidence, evaluated much later, is stale rather than insufficient.
  ControlCommand late = low_command;
  late.now = now + 10000;
  late.key = IdempotencyKey::parse("battery-test-3").value();
  const Result<TransitionEvaluation> stale_evaluation = engine->evaluate(late);
  if (!stale_evaluation.ok()) {
    return fail(stale_evaluation.status());
  }
  std::cout << "stale_evidence verdict="
            << (stale_evaluation.value().report.allowed ? "allowed" : "refused")
            << " primary=" << to_string(stale_evaluation.value().report.primary) << "\n";

  // A request planned against a superseded epoch is fenced.
  ControlCommand fenced = late;
  fenced.now = now + 4;
  fenced.key = IdempotencyKey::parse("battery-test-4").value();
  fenced.authority.epoch = ControlEpoch{10};
  const Result<TransitionEvaluation> fenced_evaluation = engine->evaluate(fenced);
  if (!fenced_evaluation.ok()) {
    return fail(fenced_evaluation.status());
  }
  std::cout << "fenced_epoch verdict="
            << (fenced_evaluation.value().report.allowed ? "allowed" : "refused")
            << " primary=" << to_string(fenced_evaluation.value().report.primary) << "\n";

  // An unsupported command name is refused; it is never mapped to a neighbour.
  const Result<CommandKind> unsupported = parse_command_kind("go_to_bypass");
  std::cout << "unsupported_command=" << to_string(unsupported.status().code()) << "\n";
  return 0;
}
