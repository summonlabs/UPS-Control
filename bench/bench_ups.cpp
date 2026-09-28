// Benchmark of completed operations.
//
// Every measurement below times a whole operation, including all mandatory work:
// request validation, canonical encoding, staging write, the required device
// flush, read-back verification, atomic generation publish, head-marker commit,
// and residue cleanup. Nothing here times enqueue or submission latency and calls
// it completion.
//
// Evidence labelling:
//   REAL       - the durable store path: real files, real flushes, real atomic
//                renames on the host filesystem.
//   SYNTHETIC  - the device interaction: the deterministic synthetic adapter.
//                No hardware is involved and no hardware result is claimed.
//   UNSUPPORTED- not measured here.
//
// The benchmark prints the workload scale, the host and toolchain context, the
// repetition count and statistic, and the cleanup state, and it removes every
// store it created before returning.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ups_control/engine.hpp"
#include "ups_control/version.hpp"

namespace {

using namespace ups_control;

constexpr int kDefaultRepetitions = 300;

struct Measurement {
  std::string name;
  std::string evidence;
  std::uint64_t operations = 0;
  double total_seconds = 0.0;
  double nanoseconds_per_operation = 0.0;
  double operations_per_second = 0.0;
};

std::uint64_t now_nanos() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

TelemetryReport healthy_report(const UpsId& ups, HardwareGeneration hardware, Tick at,
                               SourceRevision revision, const EvidenceId& evidence) {
  TelemetryReport report;
  report.evidence = evidence;
  report.ups = ups;
  report.hardware = hardware;
  report.source = SourceId::parse("bench-source").value();
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
  report.capability.recharge_enabled = Bool3::True;
  report.capability.discharge_enabled = Bool3::True;
  report.battery.reserve = known_reserve(ReserveQuantity{ReserveUnit::Seconds, 1800}, EvidenceQuality::Measured);
  report.battery.activity = BatteryActivity::Idle;
  return report;
}

void report(const std::vector<Measurement>& measurements) {
  std::cout << "\n";
  for (const Measurement& measurement : measurements) {
    std::cout << "operation=" << measurement.name << "\n";
    std::cout << "  evidence=" << measurement.evidence << "\n";
    std::cout << "  completed_operations=" << measurement.operations << "\n";
    std::cout << "  total_seconds=" << measurement.total_seconds << "\n";
    std::cout << "  nanoseconds_per_operation=" << measurement.nanoseconds_per_operation << "\n";
    std::cout << "  operations_per_second=" << measurement.operations_per_second << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  int repetitions = kDefaultRepetitions;
  if (argc > 1) {
    repetitions = std::atoi(argv[1]);
  }
  if (repetitions <= 0) {
    std::cerr << "usage: ups_control_bench [repetitions]\n";
    return 1;
  }

  const std::filesystem::path root = std::filesystem::temp_directory_path() / "ups-control-bench";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  if (error) {
    std::cerr << "error: could not create the benchmark directory: " << error.message() << "\n";
    return 1;
  }

  std::cout << "ups-control benchmark\n";
  std::cout << "library_version=" << Version::string << "\n";
  std::cout << "toolchain="
#if defined(_MSC_VER)
            << "MSVC " << _MSC_VER
#elif defined(__clang__)
            << "Clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
            << "GCC " << __GNUC__ << "." << __GNUC_MINOR__
#else
            << "unknown"
#endif
            << "\n";
  std::cout << "build_type="
#if defined(NDEBUG)
            << "Release"
#else
            << "Debug"
#endif
            << "\n";
  std::cout << "os="
#if defined(_WIN32)
            << "Windows"
#elif defined(__linux__)
            << "Linux"
#elif defined(__APPLE__)
            << "macOS"
#else
            << "unknown"
#endif
            << "\n";
  std::cout << "hardware_threads=" << std::thread::hardware_concurrency() << "\n";
  std::cout << "repetitions=" << repetitions << "\n";
  std::cout << "workload_scale=one_registered_unit_with_one_obligation_and_one_grant\n";
  std::cout << "statistic=arithmetic_mean_over_all_completed_operations\n";

  std::vector<Measurement> measurements;

  // ---- 1. Engine open: full store read, decode, structural validation ---------
  {
    const std::filesystem::path path = root / "open.upsstore";
    const Tick now{100000};
    const ControlContext context{ControlEpoch{1}, Incarnation{1}};
    SyntheticAdapter::Script script;
    EngineOpenOptions open_options;
    open_options.store.path = path;
    open_options.store.access = StoreAccess::ReadWrite;
    open_options.store.create_if_missing = true;
    open_options.store.created_at = now;
    open_options.store.epoch = context.epoch;
    open_options.store.incarnation = context.incarnation;
    open_options.adapter = std::make_shared<SyntheticAdapter>(script);

    Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
    if (!opened.ok()) {
      std::cerr << "error: " << opened.status().to_string() << "\n";
      return 1;
    }
    std::shared_ptr<UpsControlEngine> engine = opened.value();
    RevalidateRequest revalidate;
    revalidate.authority = context;
    revalidate.now = now;
    if (!engine->revalidate(revalidate).ok()) {
      std::cerr << "error: could not revalidate the benchmark store\n";
      return 1;
    }
    RegisterUpsRequest registration;
    registration.authority = context;
    registration.now = now;
    registration.id = UpsId::parse("bench-ups").value();
    registration.hardware = HardwareGeneration{1};
    registration.lifecycle = LifecycleState::InService;
    registration.operating = OperatingState::OnlineNormal;
    registration.policy.max_evidence_age = TickSpan{100000};
    registration.policy.discharge_floor = ReserveQuantity{ReserveUnit::Seconds, 600};
    const Result<UpsRecord> registered = engine->register_ups(registration);
    if (!registered.ok()) {
      std::cerr << "error: " << registered.status().to_string() << "\n";
      return 1;
    }
    TelemetryReport report_value =
        healthy_report(registration.id, registration.hardware, now, SourceRevision{1},
                       EvidenceId::parse("bench-ev-1").value());
    RecordTelemetryRequest telemetry{context,
                                     UpsRef{registered.value().id, registered.value().hardware,
                                            registered.value().revision},
                                     now, report_value};
    if (!engine->record_telemetry(telemetry).ok()) {
      std::cerr << "error: could not record the benchmark observation\n";
      return 1;
    }
    IssueGrantRequest grant_request;
    grant_request.authority = context;
    const UpsRecord ready = (*engine->units()).front();
    grant_request.ref = UpsRef{ready.id, ready.hardware, ready.revision};
    grant_request.now = now;
    grant_request.grant.ref = AuthorityRef::parse("bench-authority").value();
    grant_request.grant.scope = GrantScope::BypassTransfer;
    grant_request.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    grant_request.grant.issued_at = now;
    grant_request.grant.expires_at = Tick{1000000000};
    if (!engine->issue_grant(grant_request).ok()) {
      std::cerr << "error: could not issue the benchmark grant\n";
      return 1;
    }
    engine->close();

    const std::uint64_t start = now_nanos();
    for (int index = 0; index < repetitions; ++index) {
      Result<std::shared_ptr<UpsControlEngine>> reopened = UpsControlEngine::open(open_options);
      if (!reopened.ok()) {
        std::cerr << "error: reopen failed: " << reopened.status().to_string() << "\n";
        return 1;
      }
      reopened.value()->close();
    }
    const std::uint64_t end = now_nanos();

    Measurement measurement;
    measurement.name = "engine_open_with_full_store_verification";
    measurement.evidence = "REAL";
    measurement.operations = static_cast<std::uint64_t>(repetitions);
    measurement.total_seconds =
        static_cast<double>(end - start) / 1'000'000'000.0;
    measurement.nanoseconds_per_operation =
        static_cast<double>(end - start) / static_cast<double>(repetitions);
    measurement.operations_per_second =
        static_cast<double>(repetitions) * 1'000'000'000.0 / static_cast<double>(end - start);
    measurements.push_back(measurement);
  }

  // ---- 2. Durable telemetry commit -------------------------------------------
  {
    const std::filesystem::path path = root / "telemetry.upsstore";
    const Tick now{200000};
    const ControlContext context{ControlEpoch{2}, Incarnation{1}};
    SyntheticAdapter::Script script;
    EngineOpenOptions open_options;
    open_options.store.path = path;
    open_options.store.access = StoreAccess::ReadWrite;
    open_options.store.create_if_missing = true;
    open_options.store.created_at = now;
    open_options.store.epoch = context.epoch;
    open_options.store.incarnation = context.incarnation;
    open_options.adapter = std::make_shared<SyntheticAdapter>(script);
    Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
    if (!opened.ok()) {
      std::cerr << "error: " << opened.status().to_string() << "\n";
      return 1;
    }
    std::shared_ptr<UpsControlEngine> engine = opened.value();
    RevalidateRequest revalidate;
    revalidate.authority = context;
    revalidate.now = now;
    if (!engine->revalidate(revalidate).ok()) {
      std::cerr << "error: could not revalidate the benchmark store\n";
      return 1;
    }
    RegisterUpsRequest registration;
    registration.authority = context;
    registration.now = now;
    registration.id = UpsId::parse("bench-ups").value();
    registration.hardware = HardwareGeneration{1};
    registration.lifecycle = LifecycleState::InService;
    registration.operating = OperatingState::OnlineNormal;
    registration.policy.max_evidence_age = TickSpan{100000000};
    if (!engine->register_ups(registration).ok()) {
      std::cerr << "error: could not register the benchmark unit\n";
      return 1;
    }

    const std::uint64_t start = now_nanos();
    Tick instant = now;
    for (int index = 0; index < repetitions; ++index) {
      instant = Tick{instant.value() + 1};
      const UpsRecord unit = (*engine->units()).front();
      TelemetryReport value;
      value.evidence = EvidenceId::parse("bench-ev-" + std::to_string(index + 1)).value();
      value.ups = unit.id;
      value.hardware = unit.hardware;
      value.source = SourceId::parse("bench-source").value();
      value.provenance = Provenance::SyntheticAdapter;
      value.revision = SourceRevision{static_cast<std::uint64_t>(index + 1)};
      value.observed_at = instant;
      value.received_at = instant;
      value.operating = OperatingState::OnlineNormal;
      value.transfer.bypass_kind = BypassKind::Static;
      value.transfer.bypass_available = Bool3::True;
      value.transfer.transfer_ready = Bool3::True;
      value.battery.reserve = known_reserve(ReserveQuantity{ReserveUnit::Seconds, 1800}, EvidenceQuality::Measured);
      RecordTelemetryRequest telemetry{context, UpsRef{unit.id, unit.hardware, unit.revision},
                                       instant, value};
      const Result<ObservationRecord> recorded = engine->record_telemetry(telemetry);
      if (!recorded.ok()) {
        std::cerr << "error: telemetry commit failed at repetition " << index << ": "
                  << recorded.status().to_string() << "\n";
        return 1;
      }
    }
    const std::uint64_t end = now_nanos();
    engine->close();

    Measurement measurement;
    measurement.name = "durable_telemetry_commit";
    measurement.evidence = "REAL";
    measurement.operations = static_cast<std::uint64_t>(repetitions);
    measurement.total_seconds = static_cast<double>(end - start) / 1'000'000'000.0;
    measurement.nanoseconds_per_operation =
        static_cast<double>(end - start) / static_cast<double>(repetitions);
    measurement.operations_per_second =
        static_cast<double>(repetitions) * 1'000'000'000.0 / static_cast<double>(end - start);
    measurements.push_back(measurement);
  }

  // ---- 3. Completed control attempt: plan commit, issue, acknowledgement ------
  {
    const std::filesystem::path path = root / "attempt.upsstore";
    const Tick now{300000};
    const ControlContext context{ControlEpoch{3}, Incarnation{1}};
    SyntheticAdapter::Script script;
    script.acknowledgements[CommandKind::EnterStaticBypass] = AckOutcome::NoResponse;
    script.acknowledgements[CommandKind::LeaveStaticBypass] = AckOutcome::NoResponse;
    EngineOpenOptions open_options;
    open_options.store.path = path;
    open_options.store.access = StoreAccess::ReadWrite;
    open_options.store.create_if_missing = true;
    open_options.store.created_at = now;
    open_options.store.epoch = context.epoch;
    open_options.store.incarnation = context.incarnation;
    open_options.adapter = std::make_shared<SyntheticAdapter>(script);
    Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
    if (!opened.ok()) {
      std::cerr << "error: " << opened.status().to_string() << "\n";
      return 1;
    }
    std::shared_ptr<UpsControlEngine> engine = opened.value();
    RevalidateRequest revalidate;
    revalidate.authority = context;
    revalidate.now = now;
    if (!engine->revalidate(revalidate).ok()) {
      std::cerr << "error: could not revalidate the benchmark store\n";
      return 1;
    }
    RegisterUpsRequest registration;
    registration.authority = context;
    registration.now = now;
    registration.id = UpsId::parse("bench-ups").value();
    registration.hardware = HardwareGeneration{1};
    registration.lifecycle = LifecycleState::InService;
    registration.operating = OperatingState::OnlineNormal;
    registration.policy.max_evidence_age = TickSpan{100000000};
    if (!engine->register_ups(registration).ok()) {
      std::cerr << "error: could not register the benchmark unit\n";
      return 1;
    }
    TelemetryReport first = healthy_report(registration.id, registration.hardware, now,
                                           SourceRevision{1}, EvidenceId::parse("bench-ev-1").value());
    const UpsRecord initial = (*engine->units()).front();
    if (!engine->record_telemetry(RecordTelemetryRequest{
            context, UpsRef{initial.id, initial.hardware, initial.revision}, now, first})
             .ok()) {
      std::cerr << "error: could not record the benchmark observation\n";
      return 1;
    }
    IssueGrantRequest grant_request;
    grant_request.authority = context;
    const UpsRecord ready = (*engine->units()).front();
    grant_request.ref = UpsRef{ready.id, ready.hardware, ready.revision};
    grant_request.now = now;
    grant_request.grant.ref = AuthorityRef::parse("bench-authority").value();
    grant_request.grant.scope = GrantScope::BypassTransfer;
    grant_request.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    grant_request.grant.issued_at = now;
    grant_request.grant.expires_at = Tick{1000000000};
    if (!engine->issue_grant(grant_request).ok()) {
      std::cerr << "error: could not issue the benchmark grant\n";
      return 1;
    }

    Tick instant = now;
    std::uint64_t submitted_nanos = 0;
    std::uint64_t scaffolding_nanos = 0;
    for (int index = 0; index < repetitions; ++index) {
      const std::uint64_t scaffolding_start = now_nanos();
      instant = Tick{instant.value() + 1};
      const UpsRecord unit = (*engine->units()).front();
      first.revision = SourceRevision{static_cast<std::uint64_t>(index + 2)};
      first.observed_at = instant;
      first.received_at = instant;
      first.transfer = TransferStatus{};
      first.transfer.bypass_kind = BypassKind::Static;
      first.transfer.bypass_available = Bool3::True;
      first.transfer.bypass_qualified = Bool3::True;
      first.transfer.output_synchronized = Bool3::True;
      first.transfer.transfer_ready = Bool3::True;
      first.transfer.battery_ready = Bool3::True;
      first.evidence = EvidenceId::parse("bench-live-" + std::to_string(index + 1)).value();
      if (!engine->record_telemetry(RecordTelemetryRequest{
              context, UpsRef{unit.id, unit.hardware, unit.revision}, instant, first})
               .ok()) {
        std::cerr << "error: could not refresh the benchmark observation\n";
        return 1;
      }
      const UpsRecord fresh = (*engine->units()).front();
      ControlCommand command;
      command.authority = context;
      command.ref = UpsRef{fresh.id, fresh.hardware, fresh.revision};
      command.now = instant;
      command.key = IdempotencyKey::parse("bench-key-" + std::to_string(index + 1)).value();
      command.kind = CommandKind::EnterStaticBypass;
      command.authority_ref = AuthorityRef::parse("bench-authority").value();
      // Everything before this point restores the identical starting position and
      // is measured separately. The timed region is exactly the completed control
      // attempt: validation, deterministic evaluation, canonical plan encoding, the
      // durable plan commit with its staging write, device flush, read-back
      // verification and head-marker commit, the adapter issuance, and the durable
      // acknowledgement commit.
      const std::uint64_t submitted_start = now_nanos();
      const Result<AttemptRecord> attempt = engine->submit(command);
      const std::uint64_t submitted_end = now_nanos();
      submitted_nanos += submitted_end - submitted_start;
      if (!attempt.ok()) {
        std::cerr << "error: submit failed at repetition " << index << ": "
                  << attempt.status().to_string() << "\n";
        return 1;
      }
      if (attempt.value().phase != AttemptPhase::Issued) {
        std::cerr << "error: unexpected attempt phase "
                  << to_string(attempt.value().phase) << " at repetition " << index << "\n";
        return 1;
      }
      // Return the unit to a state from which the bypass can be entered again, so
      // that every repetition performs the same amount of work.
      AdoptOperatingStateRequest reset;
      reset.authority = context;
      const UpsRecord after = (*engine->units()).front();
      reset.ref = UpsRef{after.id, after.hardware, after.revision};
      reset.now = instant;
      reset.operating = OperatingState::OnlineNormal;
      reset.reason = "benchmark reset";
      if (!engine->adopt_operating_state(reset).ok()) {
        std::cerr << "error: could not reset the benchmark unit\n";
        return 1;
      }
      AbandonRequest abandon;
      abandon.authority = context;
      abandon.now = instant;
      abandon.attempt = attempt.value().id;
      abandon.reason = "benchmark reset";
      if (!engine->abandon(abandon).ok()) {
        std::cerr << "error: could not abandon the benchmark attempt\n";
        return 1;
      }
      scaffolding_nanos += now_nanos() - scaffolding_start;
    }
    engine->close();

    // The timed region is the completed control attempt only. The scaffolding that
    // restores the identical starting position between repetitions is reported
    // separately so that the attempt number is neither inflated nor silently
    // absorbed into a total.
    Measurement measurement;
    measurement.name = "completed_control_attempt";
    measurement.evidence = "REAL(store) + SYNTHETIC(device)";
    measurement.operations = static_cast<std::uint64_t>(repetitions);
    measurement.total_seconds = static_cast<double>(submitted_nanos) / 1'000'000'000.0;
    measurement.nanoseconds_per_operation =
        static_cast<double>(submitted_nanos) / static_cast<double>(repetitions);
    measurement.operations_per_second =
        static_cast<double>(repetitions) * 1'000'000'000.0 / static_cast<double>(submitted_nanos);
    measurements.push_back(measurement);

    Measurement scaffolding;
    scaffolding.name = "scaffolding_between_control_attempts";
    scaffolding.evidence = "REAL (not part of the attempt operation; reported for transparency)";
    scaffolding.operations = static_cast<std::uint64_t>(repetitions);
    scaffolding.total_seconds = static_cast<double>(scaffolding_nanos) / 1'000'000'000.0;
    scaffolding.nanoseconds_per_operation =
        static_cast<double>(scaffolding_nanos) / static_cast<double>(repetitions);
    scaffolding.operations_per_second = static_cast<double>(repetitions) * 1'000'000'000.0 /
                                       static_cast<double>(scaffolding_nanos);
    measurements.push_back(scaffolding);
  }

  report(measurements);

  const std::uint64_t residue_before = static_cast<std::uint64_t>(
      std::distance(std::filesystem::recursive_directory_iterator(root),
                    std::filesystem::recursive_directory_iterator()));
  std::filesystem::remove_all(root, error);
  std::cout << "\ncleanup=removed_" << residue_before << "_files\n";
  std::cout << "cleanup_state=" << (std::filesystem::exists(root) ? "residue" : "clean") << "\n";
  std::cout << "device_evidence=SYNTHETIC (deterministic adapter; no hardware was used)\n";
  std::cout << "hardware_validation=UNSUPPORTED (no UPS hardware is attached to this host)\n";
  return 0;
}
