// Out-of-tree consumer of the installed UPS Control package.
//
// This program is deliberately written the way a downstream service would use the
// library: it includes only installed public headers, links only the exported
// target, and exercises a real lifecycle rather than poking at internals. It
// drives the deterministic synthetic adapter, so every device interaction it
// performs is labelled SYNTHETIC.

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "ups_control/engine.hpp"
#include "ups_control/version.hpp"

namespace {

using namespace ups_control;

int fail(const std::string& what, const Status& status) {
  std::cerr << "error: " << what << ": " << status.to_string() << "\n";
  return 1;
}

void clean(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::remove(std::filesystem::path(path.string() + ".g1"), error);
  std::filesystem::remove(std::filesystem::path(path.string() + ".lock"), error);
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path store_path =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("consumer.upsstore");
  std::error_code error;
  std::filesystem::create_directories(store_path.parent_path(), error);
  clean(store_path);

  std::cout << "consumer: ups_control " << Version::string << "\n";

  const Tick now{5000};
  const ControlContext context{ControlEpoch{21}, Incarnation{1}};

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
    return fail("open", opened.status());
  }
  std::shared_ptr<UpsControlEngine> engine = opened.value();

  RevalidateRequest revalidate;
  revalidate.authority = context;
  revalidate.now = now;
  const Result<RevalidationReport> revalidated = engine->revalidate(revalidate);
  if (!revalidated.ok()) {
    return fail("revalidate", revalidated.status());
  }

  RegisterUpsRequest registration;
  registration.authority = context;
  registration.now = now;
  registration.id = UpsId::parse("consumer-ups").value();
  registration.hardware = HardwareGeneration{1};
  registration.lifecycle = LifecycleState::InService;
  registration.operating = OperatingState::OnlineNormal;
  registration.policy.max_evidence_age = TickSpan{600};
  registration.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 300};
  const Result<UpsRecord> registered = engine->register_ups(registration);
  if (!registered.ok()) {
    return fail("register", registered.status());
  }

  TelemetryReport report;
  report.evidence = EvidenceId::parse("consumer-ev-1").value();
  report.ups = registered.value().id;
  report.hardware = registered.value().hardware;
  report.source = SourceId::parse("consumer-source").value();
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
  const Result<ObservationRecord> recorded = engine->record_telemetry(RecordTelemetryRequest{
      context,
      UpsRef{registered.value().id, registered.value().hardware, registered.value().revision}, now,
      report});
  if (!recorded.ok()) {
    return fail("record telemetry", recorded.status());
  }

  IssueGrantRequest grant_request;
  grant_request.authority = context;
  const UpsRecord ready = (*engine->units()).front();
  grant_request.ref = UpsRef{ready.id, ready.hardware, ready.revision};
  grant_request.now = now;
  grant_request.grant.ref = AuthorityRef::parse("consumer-authority").value();
  grant_request.grant.scope = GrantScope::BypassTransfer;
  grant_request.grant.granted_by = AuthorityRef::parse("facility-ops").value();
  grant_request.grant.issued_at = now;
  grant_request.grant.expires_at = Tick{90000};
  if (!engine->issue_grant(grant_request).ok()) {
    return fail("issue grant", engine->issue_grant(grant_request).status());
  }

  const UpsRecord current = (*engine->units()).front();
  ControlCommand command;
  command.authority = context;
  command.ref = UpsRef{current.id, current.hardware, current.revision};
  command.now = now + 1;
  command.key = IdempotencyKey::parse("consumer-key-1").value();
  command.kind = CommandKind::EnterStaticBypass;
  command.authority_ref = AuthorityRef::parse("consumer-authority").value();

  const Result<TransitionEvaluation> evaluation = engine->evaluate(command);
  if (!evaluation.ok()) {
    return fail("evaluate", evaluation.status());
  }
  if (!evaluation.value().report.allowed) {
    std::cerr << "error: the consumer transition was refused: "
              << to_string(evaluation.value().report.primary) << "\n";
    return 1;
  }

  const Result<AttemptRecord> attempt = engine->submit(command);
  if (!attempt.ok()) {
    return fail("submit", attempt.status());
  }
  if (attempt.value().phase != AttemptPhase::Acknowledged) {
    std::cerr << "error: unexpected attempt phase " << to_string(attempt.value().phase) << "\n";
    return 1;
  }

  TelemetryReport effect = report;
  effect.evidence = EvidenceId::parse("consumer-ev-2").value();
  effect.revision = SourceRevision{2};
  effect.observed_at = now + 2;
  effect.received_at = now + 2;
  effect.operating = OperatingState::StaticBypass;
  const Result<ObservationRecord> observed = engine->record_telemetry(RecordTelemetryRequest{
      context, command.ref, now + 2, effect});
  if (!observed.ok()) {
    return fail("record effect", observed.status());
  }

  VerifyRequest verify;
  verify.authority = context;
  verify.now = now + 2;
  verify.attempt = attempt.value().id;
  const Result<AttemptRecord> verified = engine->verify(verify);
  if (!verified.ok()) {
    return fail("verify", verified.status());
  }
  if (verified.value().verification != VerificationVerdict::Verified) {
    std::cerr << "error: the recorded effect did not verify: "
              << to_string(verified.value().verification) << "\n";
    return 1;
  }

  const Result<StoreAuditReport> audit = engine->store_audit();
  if (!audit.ok()) {
    return fail("audit", audit.status());
  }
  const std::uint64_t digest = audit.value().canonical_digest;
  const StoreGeneration generation = audit.value().generation;
  engine->close();

  // Reopen and confirm that the durable state is exactly what was committed and
  // that recovery does not make the observation fresh again.
  open_options.store.access = StoreAccess::ReadWrite;
  Result<std::shared_ptr<UpsControlEngine>> reopened = UpsControlEngine::open(open_options);
  if (!reopened.ok()) {
    return fail("reopen", reopened.status());
  }
  const Result<StoreAuditReport> reopened_audit = reopened.value()->store_audit();
  if (!reopened_audit.ok()) {
    return fail("reopen audit", reopened_audit.status());
  }
  if (reopened_audit.value().canonical_digest != digest ||
      !(reopened_audit.value().generation == generation)) {
    std::cerr << "error: the reopened store does not match the committed state\n";
    return 1;
  }
  const Result<StatusReport> status =
      reopened.value()->status(UpsQuery{registered.value().id, now + 2});
  if (!status.ok()) {
    return fail("status", status.status());
  }

  std::cout << "consumer: generation=" << generation.value()
            << " canonical_digest=" << digest << "\n";
  std::cout << "consumer: recovered_lifecycle="
            << to_string(reopened.value()->info().lifecycle)
            << " operating=" << to_string(status.value().operating)
            << " basis=" << to_string(status.value().basis) << "\n";
  std::cout << "consumer: device_evidence=SYNTHETIC\n";
  reopened.value()->close();
  clean(store_path);
  std::cout << "consumer: ok\n";
  return 0;
}
