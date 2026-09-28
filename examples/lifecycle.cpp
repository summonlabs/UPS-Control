// A complete UPS control lifecycle, driven entirely through the public API.
//
// This example uses the deterministic synthetic adapter. No hardware is
// involved, and every attempt it produces is labelled SYNTHETIC.

#include <cstdio>
#include <cstdlib>
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
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("example-lifecycle.upsstore");
  std::error_code error;
  std::filesystem::remove(store_path, error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".g1"), error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".lock"), error);

  const Tick now{1000};

  SyntheticAdapter::Script script;
  script.supported_commands = all_command_kinds_mask();

  EngineOpenOptions open_options;
  open_options.store.path = store_path;
  open_options.store.access = StoreAccess::ReadWrite;
  open_options.store.create_if_missing = true;
  open_options.store.created_at = now;
  open_options.store.epoch = ControlEpoch{7};
  open_options.store.incarnation = Incarnation{1};
  open_options.adapter = std::make_shared<SyntheticAdapter>(script);

  Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
  if (!opened.ok()) {
    return fail(opened.status());
  }
  std::shared_ptr<UpsControlEngine> engine = opened.value();
  const ControlContext context{ControlEpoch{7}, Incarnation{1}};

  // A freshly recovered store is never revalidated until it is revalidated at an
  // explicit instant.
  std::cout << "lifecycle=" << to_string(engine->info().lifecycle) << "\n";
  RevalidateRequest revalidate;
  revalidate.authority = context;
  revalidate.now = now;
  const Result<RevalidationReport> revalidated = engine->revalidate(revalidate);
  if (!revalidated.ok()) {
    return fail(revalidated.status());
  }

  RegisterUpsRequest registration;
  registration.authority = context;
  registration.now = now;
  registration.id = UpsId::parse("ups-a1").value();
  registration.label = "Row A UPS 1";
  registration.hardware = HardwareGeneration{3};
  registration.lifecycle = LifecycleState::InService;
  registration.operating = OperatingState::OnlineNormal;
  registration.policy.max_evidence_age = TickSpan{300};
  const Result<UpsRecord> registered = engine->register_ups(registration);
  if (!registered.ok()) {
    return fail(registered.status());
  }
  std::cout << "registered=" << registered.value().id.value()
            << " revision=" << registered.value().revision.value() << "\n";

  IssueGrantRequest grant_request;
  grant_request.authority = context;
  grant_request.ref = UpsRef{registered.value().id, registered.value().hardware,
                             registered.value().revision};
  grant_request.now = now;
  grant_request.grant.ref = AuthorityRef::parse("authority-ops").value();
  grant_request.grant.scope = GrantScope::BypassTransfer;
  grant_request.grant.granted_by = AuthorityRef::parse("feed-authority").value();
  grant_request.grant.issued_at = now;
  grant_request.grant.expires_at = Tick{5000};
  const Result<AuthorityGrant> grant = engine->issue_grant(grant_request);
  if (!grant.ok()) {
    return fail(grant.status());
  }

  const Result<UpsRecord> after_grant = engine->units().ok()
                                            ? Result<UpsRecord>((*engine->units()).front())
                                            : Result<UpsRecord>(engine->units().status());

  TelemetryReport report;
  report.evidence = EvidenceId::parse("ev-1").value();
  report.ups = registered.value().id;
  report.hardware = registered.value().hardware;
  report.source = SourceId::parse("device-poll").value();
  report.provenance = Provenance::SyntheticAdapter;
  report.revision = SourceRevision{1};
  report.observed_at = now;
  report.received_at = now;
  report.operating = OperatingState::OnlineNormal;
  report.transfer.bypass_kind = BypassKind::Static;
  report.transfer.bypass_available = Bool3::True;
  report.transfer.bypass_qualified = Bool3::True;
  report.transfer.output_synchronized = Bool3::True;
  report.transfer.transfer_ready = Bool3::True;
  report.transfer.battery_ready = Bool3::True;
  report.battery.reserve = known_reserve(ReserveQuantity{ReserveUnit::Seconds, 900}, EvidenceQuality::Measured);
  report.battery.activity = BatteryActivity::Idle;

  RecordTelemetryRequest telemetry;
  telemetry.authority = context;
  telemetry.ref = UpsRef{after_grant.value().id, after_grant.value().hardware,
                         after_grant.value().revision};
  telemetry.now = now + 1;
  telemetry.report = report;
  const Result<ObservationRecord> observed = engine->record_telemetry(telemetry);
  if (!observed.ok()) {
    return fail(observed.status());
  }

  const Result<std::vector<UpsRecord>> units = engine->units();
  if (!units.ok()) {
    return fail(units.status());
  }
  const UpsRecord& unit = units.value().front();
  const UpsRef reference{unit.id, unit.hardware, unit.revision};

  ControlCommand command;
  command.authority = context;
  command.ref = reference;
  command.now = now + 2;
  command.key = IdempotencyKey::parse("lifecycle-key-1").value();
  command.kind = CommandKind::EnterStaticBypass;
  command.authority_ref = AuthorityRef::parse("authority-ops").value();

  const Result<AttemptRecord> submitted = engine->submit(command);
  if (!submitted.ok()) {
    return fail(submitted.status());
  }
  std::cout << "attempt=" << submitted.value().id.value()
            << " phase=" << to_string(submitted.value().phase)
            << " ack=" << to_string(submitted.value().ack)
            << " verified=" << to_string(submitted.value().verification)
            << " evidence_class=" << to_string(submitted.value().evidence) << "\n";

  // The device now reports the new state. The observation is a separate stage from
  // the acknowledgement, and the verification is a third stage again.
  TelemetryReport effect;
  effect.evidence = EvidenceId::parse("ev-2").value();
  effect.ups = reference.ups;
  effect.hardware = reference.hardware;
  effect.source = SourceId::parse("device-poll").value();
  effect.provenance = Provenance::SyntheticAdapter;
  effect.revision = SourceRevision{2};
  effect.observed_at = now + 3;
  effect.received_at = now + 3;
  effect.operating = OperatingState::StaticBypass;
  effect.transfer = report.transfer;
  effect.battery = report.battery;

  RecordTelemetryRequest effect_request;
  effect_request.authority = context;
  effect_request.ref = reference;
  effect_request.now = now + 3;
  effect_request.report = effect;
  const Result<ObservationRecord> effect_record = engine->record_telemetry(effect_request);
  if (!effect_record.ok()) {
    return fail(effect_record.status());
  }

  VerifyRequest verify;
  verify.authority = context;
  verify.now = now + 3;
  verify.attempt = submitted.value().id;
  const Result<AttemptRecord> verified = engine->verify(verify);
  if (!verified.ok()) {
    return fail(verified.status());
  }
  std::cout << "verified=" << to_string(verified.value().verification)
            << " phase=" << to_string(verified.value().phase) << "\n";
  std::cout << "operating=" << to_string((*engine->units()).front().operating)
            << " basis=" << to_string((*engine->units()).front().basis) << "\n";

  // A retry of the same request returns the prior committed result, even though the
  // store has advanced several generations since.
  ControlCommand retry = command;
  retry.now = now + 50;
  const Result<AttemptRecord> replayed = engine->submit(retry);
  if (!replayed.ok()) {
    return fail(replayed.status());
  }
  std::cout << "replay_attempt=" << replayed.value().id.value()
            << " phase=" << to_string(replayed.value().phase) << "\n";

  const Result<StoreAuditReport> audit = engine->store_audit();
  if (!audit.ok()) {
    return fail(audit.status());
  }
  std::cout << "generation=" << audit.value().generation.value()
            << " operations=" << audit.value().operation_count
            << " attempts=" << audit.value().attempt_count << "\n";
  (void)grant;
  std::cout << "example completed\n";
  return 0;
}
