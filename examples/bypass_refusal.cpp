// A protected-load obligation refuses a maintenance bypass transfer, and the
// refusal is lifted only by an authority-bound release.

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

TelemetryReport healthy_report(const UpsId& ups, HardwareGeneration hardware, Tick at,
                               SourceRevision revision, const EvidenceId& evidence) {
  TelemetryReport report;
  report.evidence = evidence;
  report.ups = ups;
  report.hardware = hardware;
  report.source = SourceId::parse("device-poll").value();
  report.provenance = Provenance::SyntheticAdapter;
  report.revision = revision;
  report.observed_at = at;
  report.received_at = at;
  report.operating = OperatingState::OnlineNormal;
  report.transfer.bypass_kind = BypassKind::Static;
  report.transfer.bypass_available = Bool3::True;
  report.transfer.bypass_qualified = Bool3::True;
  report.transfer.output_synchronized = Bool3::True;
  report.transfer.transfer_ready = Bool3::True;
  report.transfer.battery_ready = Bool3::True;
  report.battery.reserve = known_reserve(ReserveQuantity{ReserveUnit::Seconds, 1200}, EvidenceQuality::Measured);
  report.battery.activity = BatteryActivity::Idle;
  return report;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path store_path =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("example-bypass.upsstore");
  std::error_code error;
  std::filesystem::remove(store_path, error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".g1"), error);
  std::filesystem::remove(std::filesystem::path(store_path.string() + ".lock"), error);

  const Tick now{2000};
  const ControlContext context{ControlEpoch{9}, Incarnation{4}};

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
  registration.id = UpsId::parse("ups-b2").value();
  registration.hardware = HardwareGeneration{4};
  registration.lifecycle = LifecycleState::InService;
  registration.operating = OperatingState::OnlineNormal;
  registration.policy.max_evidence_age = TickSpan{600};
  const Result<UpsRecord> registered = engine->register_ups(registration);
  if (!registered.ok()) {
    return fail(registered.status());
  }

  const Result<ObservationRecord> observed = engine->record_telemetry(RecordTelemetryRequest{
      context,
      UpsRef{registered.value().id, registered.value().hardware, registered.value().revision},
      now + 1,
      healthy_report(registered.value().id, registered.value().hardware, now + 1, SourceRevision{1},
                     EvidenceId::parse("ev-1").value())});
  if (!observed.ok()) {
    return fail(observed.status());
  }

  const UpsRecord first = (*engine->units()).front();

  BindObligationRequest binding;
  binding.authority = context;
  binding.ref = UpsRef{first.id, first.hardware, first.revision};
  binding.now = now + 2;
  binding.obligation.ref = ObligationRef::parse("obl-life-safety-1").value();
  binding.obligation.load = LoadId::parse("load-or-3").value();
  binding.obligation.tier = ObligationTier::LifeSafety;
  binding.obligation.protection = ProtectionRequirement::MustRemainProtected;
  binding.obligation.asserted_by = AuthorityRef::parse("feed-authority").value();
  binding.obligation.asserted_at = now + 2;
  const Result<ProtectedLoadObligation> bound = engine->bind_obligation(binding);
  if (!bound.ok()) {
    return fail(bound.status());
  }

  IssueGrantRequest maintenance_grant;
  maintenance_grant.authority = context;
  maintenance_grant.ref = UpsRef{first.id, first.hardware, first.revision + 1};
  maintenance_grant.now = now + 3;
  maintenance_grant.grant.ref = AuthorityRef::parse("authority-maintenance").value();
  maintenance_grant.grant.scope = GrantScope::MaintenanceEntry;
  maintenance_grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
  maintenance_grant.grant.issued_at = now + 3;
  maintenance_grant.grant.expires_at = Tick{9000};
  if (!engine->issue_grant(maintenance_grant).ok()) {
    return fail(engine->issue_grant(maintenance_grant).status());
  }

  const UpsRecord second = (*engine->units()).front();
  ControlCommand bypass;
  bypass.authority = context;
  bypass.ref = UpsRef{second.id, second.hardware, second.revision};
  bypass.now = now + 4;
  bypass.key = IdempotencyKey::parse("bypass-1").value();
  bypass.kind = CommandKind::EnterMaintenanceBypass;
  bypass.authority_ref = AuthorityRef::parse("authority-maintenance").value();

  const Result<TransitionEvaluation> evaluation = engine->evaluate(bypass);
  if (!evaluation.ok()) {
    return fail(evaluation.status());
  }
  std::cout << "first_evaluation verdict="
            << (evaluation.value().report.allowed ? "allowed" : "refused")
            << " primary=" << to_string(evaluation.value().report.primary) << "\n";
  std::cout << "protection_impact=" << to_string(evaluation.value().report.protection.impact)
            << " at_risk=" << evaluation.value().report.protection.at_risk.size() << "\n";
  if (evaluation.value().report.allowed) {
    std::cerr << "the obligation did not block the protection-dropping transition\n";
    return 1;
  }

  const UpsRecord third = (*engine->units()).front();
  ReleaseObligationRequest release;
  release.authority = context;
  release.ref = UpsRef{third.id, third.hardware, third.revision};
  release.now = now + 5;
  release.obligation = bound.value().ref;
  release.obligation_revision = bound.value().revision;
  release.release_authority = AuthorityRef::parse("feed-authority").value();
  release.reason = "load transferred to the redundant feed";
  const Result<ProtectedLoadObligation> released = engine->release_obligation(release);
  if (!released.ok()) {
    return fail(released.status());
  }

  const UpsRecord fourth = (*engine->units()).front();
  ControlCommand retry = bypass;
  retry.ref = UpsRef{fourth.id, fourth.hardware, fourth.revision};
  retry.now = now + 6;
  retry.key = IdempotencyKey::parse("bypass-2").value();
  const Result<TransitionEvaluation> second_evaluation = engine->evaluate(retry);
  if (!second_evaluation.ok()) {
    return fail(second_evaluation.status());
  }
  std::cout << "second_evaluation verdict="
            << (second_evaluation.value().report.allowed ? "allowed" : "refused")
            << " primary=" << to_string(second_evaluation.value().report.primary) << "\n";

  const Result<AttemptRecord> issued = engine->submit(retry);
  if (!issued.ok()) {
    return fail(issued.status());
  }
  std::cout << "attempt_phase=" << to_string(issued.value().phase)
            << " ack=" << to_string(issued.value().ack)
            << " verified=" << to_string(issued.value().verification) << "\n";
  std::cout << "note: an acknowledgement is not an effect; the transfer is verified "
               "only against fresh telemetry\n";
  return 0;
}
