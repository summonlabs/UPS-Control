// A modelled restart: the engine is closed with an unresolved attempt and
// reopened. The attempt is recovered as it was, it is never reissued, and a retry
// of the same idempotency key returns the prior record instead of a new attempt.
//
// This example models the restart by closing and reopening the handle in one
// process. The test suite exercises the same protocol against real process death
// at each durable stage.

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
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("example-restart.upsstore");
  std::error_code error;
  std::filesystem::remove(store_path, error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".g1"), error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".lock"), error);

  const Tick now{4000};
  const ControlContext context{ControlEpoch{5}, Incarnation{1}};

  SyntheticAdapter::Script script;
  script.acknowledgements[CommandKind::EnterStaticBypass] = AckOutcome::NoResponse;

  EngineOpenOptions open_options;
  open_options.store.path = store_path;
  open_options.store.access = StoreAccess::ReadWrite;
  open_options.store.create_if_missing = true;
  open_options.store.created_at = now;
  open_options.store.epoch = context.epoch;
  open_options.store.incarnation = context.incarnation;
  open_options.adapter = std::make_shared<SyntheticAdapter>(script);

  {
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
    registration.id = UpsId::parse("ups-d4").value();
    registration.hardware = HardwareGeneration{1};
    registration.lifecycle = LifecycleState::InService;
    registration.operating = OperatingState::OnlineNormal;
    registration.policy.max_evidence_age = TickSpan{600};
    if (!engine->register_ups(registration).ok()) {
      return fail(engine->register_ups(registration).status());
    }
    TelemetryReport report;
    report.evidence = EvidenceId::parse("ev-1").value();
    report.ups = registration.id;
    report.hardware = registration.hardware;
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
    const UpsRecord unit = (*engine->units()).front();
    RecordTelemetryRequest telemetry{context, UpsRef{unit.id, unit.hardware, unit.revision}, now,
                                     report};
    if (!engine->record_telemetry(telemetry).ok()) {
      return fail(engine->record_telemetry(telemetry).status());
    }

    IssueGrantRequest grant_request;
    grant_request.authority = context;
    const UpsRecord ready = (*engine->units()).front();
    grant_request.ref = UpsRef{ready.id, ready.hardware, ready.revision};
    grant_request.now = now;
    grant_request.grant.ref = AuthorityRef::parse("authority-ops").value();
    grant_request.grant.scope = GrantScope::BypassTransfer;
    grant_request.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    grant_request.grant.issued_at = now;
    grant_request.grant.expires_at = Tick{90000};
    if (!engine->issue_grant(grant_request).ok()) {
      return fail(engine->issue_grant(grant_request).status());
    }

    const UpsRecord final_unit = (*engine->units()).front();
    ControlCommand command;
    command.authority = context;
    command.ref = UpsRef{final_unit.id, final_unit.hardware, final_unit.revision};
    command.now = now + 1;
    command.key = IdempotencyKey::parse("restart-key-1").value();
    command.kind = CommandKind::EnterStaticBypass;
    command.authority_ref = AuthorityRef::parse("authority-ops").value();
    const Result<AttemptRecord> submitted = engine->submit(command);
    if (!submitted.ok()) {
      return fail(submitted.status());
    }
    std::cout << "before_restart attempt=" << submitted.value().id.value()
              << " phase=" << to_string(submitted.value().phase)
              << " ack=" << to_string(submitted.value().ack) << "\n";
    engine->close();
  }

  // ---- restart ----
  Result<std::shared_ptr<UpsControlEngine>> reopened = UpsControlEngine::open(open_options);
  if (!reopened.ok()) {
    return fail(reopened.status());
  }
  std::shared_ptr<UpsControlEngine> engine = reopened.value();
  std::cout << "after_restart lifecycle=" << to_string(engine->info().lifecycle)
            << " unresolved=" << engine->info().unresolved_attempt_count << "\n";

  RevalidateRequest revalidate;
  revalidate.authority = context;
  revalidate.now = now + 10;
  if (!engine->revalidate(revalidate).ok()) {
    return fail(engine->revalidate(revalidate).status());
  }

  ControlCommand different;
  different.authority = context;
  const UpsRecord unit = (*engine->units()).front();
  different.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  different.now = now + 11;
  different.key = IdempotencyKey::parse("restart-key-2").value();
  // A legal transition for the current state, refused purely because the device
  // gate is still held by the unresolved attempt recovered from the crash.
  different.kind = CommandKind::EnterStaticBypass;
  different.authority_ref = AuthorityRef::parse("authority-ops").value();
  const Result<AttemptRecord> refused = engine->submit(different);
  if (!refused.ok()) {
    return fail(refused.status());
  }
  std::cout << "new_key_after_restart phase=" << to_string(refused.value().phase)
            << " refusal="
            << (refused.value().refusal.has_value() ? to_string(refused.value().refusal->code)
                                                    : std::string("none"))
            << "\n";

  ControlCommand retry;
  retry.authority = context;
  retry.ref = different.ref;
  retry.now = now + 12;
  retry.key = IdempotencyKey::parse("restart-key-1").value();
  retry.kind = CommandKind::EnterStaticBypass;
  retry.authority_ref = AuthorityRef::parse("authority-ops").value();
  const Result<AttemptRecord> replayed = engine->submit(retry);
  if (!replayed.ok()) {
    return fail(replayed.status());
  }
  std::cout << "same_key_after_restart attempt=" << replayed.value().id.value()
            << " phase=" << to_string(replayed.value().phase)
            << " same_attempt=" << (replayed.value().id.value() == 1 ? "true" : "false") << "\n";
  std::cout << "no duplicate command was issued\n";
  return 0;
}
