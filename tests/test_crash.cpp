// Process death at every durable stage of the commit protocol.
//
// Every case below spawns the real probe as an independent operating-system
// process, arms DurableCrashPoint through StoreOpenOptions::crash_at /
// crash_after_commits, and lets UpsStore::commit terminate that process at the
// named stage. Nothing here simulates process death with a thread, and the
// injected exit code is distinguished from every ordinary failure path.
//
// The probe's commit ordinals
// ---------------------------
// A control decision on recovered state is refused with
// RefusalCode::StateNotRevalidated, so the probe revalidates before it submits,
// and revalidation is itself a durable commit. With revalidate=1 the probe's
// commits are therefore:
//
//   ordinal 1   the revalidation commit
//   ordinal 2   the durable plan (AttemptPlanned), committed before the adapter
//               is invoked
//   ordinal 3   the acknowledgement commit (AttemptAcknowledged), committed
//               after the adapter was invoked
//
// Ordinal 1 is the "crash before the durable plan" case, ordinal 2 is the
// "crash after the durable plan, before the device was addressed" case, and
// ordinal 3 is the "the device was addressed exactly once and the
// acknowledgement commit died" case.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "fixture.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

#include "ups_control/engine.hpp"
#include "ups_control/store.hpp"

using namespace ups_control;

namespace {

/// The exit code the durable-stage crash injection terminates the process with
/// (src/detail/crash.hpp, kCrashExitCode). No ordinary failure path produces it,
/// so an injected crash can never be confused with an error exit.
constexpr int kCrashExitCode = 0x00C0DE01;
/// The instant the probe acts at. It must be strictly after the fixture's last
/// committed instant, or the store refuses the commit as a precondition failure,
/// and it must stay inside the freshness window of the prepared observation
/// (observed at 1000 with max_evidence_age 600), or the submission is refused
/// with BypassNotAvailable because no fresh observation establishes the transfer
/// capability, and the commit ordinals below would shift.
constexpr std::int64_t kProbeTick = 1500;
/// The instant the parent acts at after a crash.
constexpr std::int64_t kParentTick = 5000;

constexpr const char* kUpsText = "ups-under-test";
constexpr const char* kGrantText = "test-authority";

const char* const kPreHeadPoints[] = {"before_staging_write", "after_staging_write",
                                      "after_staging_flush", "after_staging_verify"};
const char* const kAllPoints[] = {"before_staging_write", "after_staging_write",
                                  "after_staging_flush", "after_staging_verify",
                                  "after_head_commit"};

std::string crash_label(const std::string& point, std::uint64_t ordinal) {
  return "crash=" + point + " nth=" + std::to_string(ordinal);
}

bool is_head_commit_point(const std::string& point) { return point == "after_head_commit"; }

/// What one directory scan of the store's directory found.
struct DirScan {
  std::size_t head = 0;
  std::size_t lock = 0;
  std::size_t payloads = 0;
  std::size_t staging = 0;
  std::size_t head_tmp = 0;
};

DirScan scan_directory(const std::filesystem::path& store_path) {
  DirScan scan;
  const std::string base = store_path.filename().string();
  std::error_code error;
  std::filesystem::directory_iterator iterator(store_path.parent_path(), error);
  if (error) {
    return scan;
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    const std::string name = entry.path().filename().string();
    if (name == base) {
      ++scan.head;
      continue;
    }
    if (name == base + ".lock") {
      ++scan.lock;
      continue;
    }
    if (name == base + ".head-tmp") {
      ++scan.head_tmp;
      continue;
    }
    if (name.size() > base.size() + 9 && name.compare(0, base.size(), base) == 0 &&
        name.compare(base.size(), 9, ".staging-") == 0) {
      ++scan.staging;
      continue;
    }
    if (name.size() > base.size() + 2 && name.compare(0, base.size(), base) == 0 &&
        name.compare(base.size(), 2, ".g") == 0) {
      ++scan.payloads;
    }
  }
  return scan;
}

/// A deterministic store prepared through the public API: one unit
/// ("ups-under-test", hardware generation 1, operating state OnlineNormal,
/// lifecycle InService), one healthy observation and one bypass-transfer grant.
/// Every prepared store is logically identical, which is what lets one crashed
/// run be compared against another as a reference.
struct PreparedStore {
  std::unique_ptr<uc_test::ScratchDirectory> scratch;
  std::filesystem::path store_path;
  std::filesystem::path effects_path;
  std::filesystem::path result_path;
  StoreGeneration generation;
  std::uint64_t digest = 0;
  StateRevision revision;
  bool ready = false;
  std::string error;
};

PreparedStore prepare(const std::string& name) {
  PreparedStore prepared;
  uc_test::Fixture fixture(name);
  if (!fixture.build()) {
    prepared.error = "the fixture store could not be built: " + fixture.last_error;
    return prepared;
  }
  const Result<StoreAuditReport> audit = fixture.engine->store_audit();
  if (!audit.ok()) {
    prepared.error = "the prepared store could not be audited: " + audit.status().to_string();
    return prepared;
  }
  const Result<UpsRecord> unit = fixture.current();
  if (!unit.ok()) {
    prepared.error = "the prepared store holds no unit: " + unit.status().to_string();
    return prepared;
  }
  prepared.scratch = std::move(fixture.scratch);
  prepared.store_path = prepared.scratch->file("unit.upsstore");
  prepared.effects_path = prepared.scratch->file("adapter-effects.log");
  prepared.result_path = prepared.scratch->file("probe-result.txt");
  prepared.generation = audit.value().generation;
  prepared.digest = audit.value().canonical_digest;
  prepared.revision = unit.value().revision;
  // Release the writer lock before the probe is started: exactly one process may
  // hold writer authority for the store.
  (void)fixture.engine->close();
  fixture.engine.reset();
  fixture.adapter.reset();
  prepared.ready = true;
  return prepared;
}

/// The probe arguments for one submission. An empty point runs the probe without
/// crash injection.
std::vector<std::string> probe_arguments(const PreparedStore& store, const std::string& key,
                                         const std::string& point, std::uint64_t ordinal) {
  std::vector<std::string> arguments;
  arguments.push_back("submit");
  arguments.push_back("store=" + store.store_path.string());
  arguments.push_back("effects=" + store.effects_path.string());
  arguments.push_back("result=" + store.result_path.string());
  arguments.push_back("tick=" + std::to_string(kProbeTick));
  arguments.push_back("revalidate=1");
  arguments.push_back("command=enter_static_bypass");
  arguments.push_back("ups=" + std::string(kUpsText));
  arguments.push_back("hardware=1");
  arguments.push_back("revision=" + std::to_string(store.revision.value()));
  arguments.push_back("key=" + key);
  arguments.push_back("authority=" + std::string(kGrantText));
  if (!point.empty()) {
    arguments.push_back("crash=" + point);
    arguments.push_back("nth=" + std::to_string(ordinal));
  }
  return arguments;
}

struct EngineHandle {
  std::shared_ptr<UpsControlEngine> engine;
  std::shared_ptr<SyntheticAdapter> adapter;
  Status status;

  bool ok() const { return engine != nullptr; }
};

/// Opens the store read-write with an adapter whose effects log is the same file
/// the probe wrote to, so a parent-side issuance would appear as an extra line
/// instead of being hidden in a different file.
EngineHandle open_read_write(const PreparedStore& store) {
  EngineHandle handle;
  EngineOpenOptions options;
  options.store.path = store.store_path;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = false;
  SyntheticAdapter::Script script;
  script.effects_log = store.effects_path;
  handle.adapter = std::make_shared<SyntheticAdapter>(std::move(script));
  options.adapter = handle.adapter;
  const Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(options);
  handle.status = opened.status();
  if (opened.ok()) {
    handle.engine = opened.value();
  }
  return handle;
}

/// Everything the parent can observe about one store.
struct StoreView {
  Status read_only_status;
  bool read_only_opened = false;
  Status verify_status;
  bool verified = false;
  bool state_valid = false;
  Status state_status;
  Status audit_status;
  bool audit_ok = false;
  Status open_status;
  bool open_ok = false;
  StoreGeneration generation;
  std::uint64_t digest = 0;
  std::size_t unit_count = 0;
  std::size_t attempt_count = 0;
  std::size_t unresolved_count = 0;
  std::vector<AttemptRecord> journal;
  std::optional<AttemptId> in_flight;
  StateRevision revision;
  OperatingState operating = OperatingState::Unknown;
  StateBasis basis = StateBasis::Recovered;
  DirScan residue_before;
  DirScan residue_after_read_only;
  DirScan residue_after_open;
  std::size_t effects_lines = 0;
};

StoreView read_view(const std::shared_ptr<UpsControlEngine>& engine) {
  StoreView view;
  const Result<StoreAuditReport> audit = engine->store_audit();
  view.audit_status = audit.status();
  view.audit_ok = audit.ok();
  if (audit.ok()) {
    view.generation = audit.value().generation;
    view.digest = audit.value().canonical_digest;
    view.unit_count = audit.value().unit_count;
    view.attempt_count = audit.value().attempt_count;
    view.unresolved_count = audit.value().unresolved_attempt_count;
  }
  const Result<std::vector<AttemptRecord>> journal = engine->history(HistoryQuery{});
  if (journal.ok()) {
    view.journal = journal.value();
  }
  const Result<UpsId> id = UpsId::parse(kUpsText);
  if (id.ok()) {
    UpsQuery query;
    query.ups = id.value();
    query.now = Tick{kParentTick};
    const Result<StatusReport> status = engine->status(query);
    if (status.ok()) {
      view.revision = status.value().revision;
      view.operating = status.value().operating;
      view.basis = status.value().basis;
      view.in_flight = status.value().in_flight;
    }
  }
  return view;
}

/// Inspects a store exactly the way recovery does: a strict read-only open (which
/// must never touch crashed-writer residue), a full verification of the artifact,
/// a structural validation of the decoded state, and then the read-write reopen
/// that retires residue.
StoreView inspect(const PreparedStore& store) {
  StoreView view;
  view.residue_before = scan_directory(store.store_path);

  StoreOpenOptions read_options;
  read_options.path = store.store_path;
  read_options.access = StoreAccess::ReadOnly;
  const Result<std::shared_ptr<UpsStore>> read_only = UpsStore::open(read_options);
  view.read_only_status = read_only.status();
  view.read_only_opened = read_only.ok();
  if (read_only.ok()) {
    const std::shared_ptr<const UpsState>& state = read_only.value()->opened_state();
    if (state != nullptr) {
      view.state_status = validate_state(*state, ResourceLimits::defaults());
      view.state_valid = view.state_status.ok();
    }
    (void)read_only.value()->close();
  }
  view.residue_after_read_only = scan_directory(store.store_path);

  view.verify_status = UpsStore::verify_file(store.store_path);
  view.verified = view.verify_status.ok();

  EngineHandle handle = open_read_write(store);
  view.open_status = handle.status;
  view.open_ok = handle.ok();
  if (handle.ok()) {
    const StoreView live = read_view(handle.engine);
    view.audit_status = live.audit_status;
    view.audit_ok = live.audit_ok;
    view.generation = live.generation;
    view.digest = live.digest;
    view.unit_count = live.unit_count;
    view.attempt_count = live.attempt_count;
    view.unresolved_count = live.unresolved_count;
    view.journal = live.journal;
    view.in_flight = live.in_flight;
    view.revision = live.revision;
    view.operating = live.operating;
    view.basis = live.basis;
    (void)handle.engine->close();
  }
  view.residue_after_open = scan_directory(store.store_path);
  view.effects_lines = uc_test::count_lines(store.effects_path);
  return view;
}

ControlContext authority_context() { return ControlContext{ControlEpoch{1}, Incarnation{1}}; }

Result<UpsRecord> single_unit(const std::shared_ptr<UpsControlEngine>& engine) {
  const Result<std::vector<UpsRecord>> units = engine->units();
  if (!units.ok()) {
    return units.status();
  }
  if (units.value().empty()) {
    return Status::error(StatusCode::NotFound, "no unit is registered");
  }
  return units.value().front();
}

ControlCommand command_for(const UpsRecord& unit, CommandKind kind, const std::string& key, Tick at) {
  ControlCommand command;
  command.authority = authority_context();
  command.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  command.now = at;
  command.kind = kind;
  command.key = IdempotencyKey::parse(key).value();
  command.authority_ref = AuthorityRef::parse(kGrantText).value();
  return command;
}

Status record_observation(const std::shared_ptr<UpsControlEngine>& engine, const UpsRecord& unit,
                          OperatingState operating, Tick at, std::uint64_t source_revision,
                          const std::string& evidence) {
  RecordTelemetryRequest request;
  request.authority = authority_context();
  request.ref = UpsRef{unit.id, unit.hardware, unit.revision};
  request.now = at;
  request.report = uc_test::healthy_report(unit.id, unit.hardware, at, SourceRevision{source_revision},
                                           evidence);
  request.report.operating = operating;
  return engine->record_telemetry(request).status();
}

/// One complete control lifecycle on a recovered store: resolve a recovered
/// unresolved attempt, take a fresh observation, submit, observe the effect and
/// verify it. A store that survives this is a working store, not merely a
/// readable one.
struct LifecycleResult {
  bool ok = false;
  std::string detail;
};

LifecycleResult complete_lifecycle(const PreparedStore& store, Tick base) {
  LifecycleResult result;
  EngineHandle handle = open_read_write(store);
  if (!handle.ok()) {
    result.detail = "the store could not be reopened: " + handle.status.to_string();
    return result;
  }
  const std::shared_ptr<UpsControlEngine> engine = handle.engine;

  RevalidateRequest revalidate;
  revalidate.authority = authority_context();
  revalidate.now = base;
  const Status revalidated = engine->revalidate(revalidate).status();
  if (!revalidated.ok()) {
    result.detail = "revalidation failed: " + revalidated.to_string();
    return result;
  }

  Result<UpsRecord> unit = single_unit(engine);
  if (!unit.ok()) {
    result.detail = "the unit vanished: " + unit.status().to_string();
    return result;
  }
  Tick tick = base;
  if (unit.value().in_flight.has_value()) {
    // The recovered attempt holds the single-device gate. It is never reissued;
    // it is resolved explicitly, and a fresh observation then restores a basis
    // that permits control again.
    AbandonRequest abandon;
    abandon.authority = authority_context();
    abandon.now = tick + 1;
    abandon.attempt = unit.value().in_flight.value();
    abandon.reason = "the recovered attempt was never verified";
    const Result<AttemptRecord> abandoned = engine->abandon(abandon);
    if (!abandoned.ok()) {
      result.detail =
          "the recovered attempt could not be abandoned: " + abandoned.status().to_string();
      return result;
    }
    tick += 1;
    unit = single_unit(engine);
    if (!unit.ok()) {
      result.detail = "the unit vanished after the abandonment";
      return result;
    }
    const Status recorded = record_observation(engine, unit.value(), OperatingState::OnlineNormal,
                                               tick + 1, 2, "recovery-observation");
    if (!recorded.ok()) {
      result.detail = "the recovery observation was refused: " + recorded.to_string();
      return result;
    }
    tick += 1;
    unit = single_unit(engine);
    if (!unit.ok()) {
      result.detail = "the unit vanished after the recovery observation";
      return result;
    }
  } else {
    // Recovery forces the state basis to Recovered and the prepared observation
    // is outside its freshness window at this instant, so a fresh observation is
    // what makes the store usable for control again.
    const Status recorded = record_observation(engine, unit.value(), OperatingState::OnlineNormal,
                                               tick + 1, 2, "recovery-observation");
    if (!recorded.ok()) {
      result.detail = "the recovery observation was refused: " + recorded.to_string();
      return result;
    }
    tick += 1;
    unit = single_unit(engine);
    if (!unit.ok()) {
      result.detail = "the unit vanished after the recovery observation";
      return result;
    }
  }

  const ControlCommand command =
      command_for(unit.value(), CommandKind::EnterStaticBypass, "lifecycle-key", tick + 1);
  const Result<AttemptRecord> submitted = engine->submit(command);
  if (!submitted.ok()) {
    result.detail = "the submission was refused: " + submitted.status().to_string();
    return result;
  }
  if (submitted.value().phase != AttemptPhase::Acknowledged) {
    result.detail = std::string("the submission reached phase ") +
                    to_string(submitted.value().phase) + " instead of acknowledged";
    return result;
  }
  tick += 1;

  unit = single_unit(engine);
  if (!unit.ok()) {
    result.detail = "the unit vanished after the submission";
    return result;
  }
  const Status recorded = record_observation(engine, unit.value(), OperatingState::StaticBypass,
                                             tick + 1, 3, "effect-observation");
  if (!recorded.ok()) {
    result.detail = "the effect observation was refused: " + recorded.to_string();
    return result;
  }
  tick += 1;

  VerifyRequest verify;
  verify.authority = authority_context();
  verify.now = tick + 1;
  verify.attempt = submitted.value().id;
  const Result<AttemptRecord> verified = engine->verify(verify);
  if (!verified.ok()) {
    result.detail = "the verification was refused: " + verified.status().to_string();
    return result;
  }
  if (verified.value().phase != AttemptPhase::Verified) {
    result.detail = std::string("the attempt ended in phase ") +
                    to_string(verified.value().phase) + " and not verified";
    return result;
  }
  unit = single_unit(engine);
  if (!unit.ok()) {
    result.detail = "the unit vanished after the verification";
    return result;
  }
  if (unit.value().operating != OperatingState::StaticBypass) {
    result.detail = std::string("the verified effect left the unit in ") +
                    to_string(unit.value().operating);
    return result;
  }
  if (unit.value().in_flight.has_value()) {
    result.detail = "the in-flight gate is still held after a verified effect";
    return result;
  }
  (void)engine->close();
  result.ok = true;
  return result;
}

}  // namespace

UC_TEST(crash, every_durable_stage_terminates_the_probe_with_the_injected_exit_code) {
  for (const char* point : kAllPoints) {
    for (std::uint64_t ordinal = 1; ordinal <= 3; ++ordinal) {
      const std::string label = crash_label(point, ordinal);
      PreparedStore store = prepare("crash-stage-" + label);
      if (!store.ready) {
        UC_CHECK_MSG(false, label + ": " + store.error);
        continue;
      }
      int exit_code = -1;
      std::string error;
      const bool started =
          uc_test::run_probe(probe_arguments(store, "stage-key", point, ordinal), exit_code, error);
      UC_CHECK_MSG(started, label + ": " + error);
      UC_CHECK_MSG(exit_code == kCrashExitCode,
                   label + ": the probe exited with " + std::to_string(exit_code) +
                       " instead of the injected crash code " + std::to_string(kCrashExitCode));

      const DirScan residue = scan_directory(store.store_path);
      // Every commit stages, including the revalidation the probe performs first,
      // so the residue at a pre-head crash point is the staging file of whichever
      // commit was running.
      const bool staging_expected =
          !is_head_commit_point(point) && point != std::string("before_staging_write");
      UC_CHECK_MSG(residue.staging == (staging_expected ? std::size_t{1} : std::size_t{0}),
                   label + ": staging residue " + std::to_string(residue.staging) +
                       " does not match the durable stage");
      const bool head_tmp_expected = point == std::string("after_staging_verify");
      UC_CHECK_MSG(residue.head_tmp == (head_tmp_expected ? std::size_t{1} : std::size_t{0}),
                   label + ": head-tmp residue " + std::to_string(residue.head_tmp) +
                       " does not match the durable stage");
      UC_CHECK_MSG(residue.payloads == (is_head_commit_point(point) ? std::size_t{2} : std::size_t{1}),
                   label + ": " + std::to_string(residue.payloads) +
                       " generation payloads exist where the durable stage requires " +
                       (is_head_commit_point(point) ? "2" : "1"));
      UC_CHECK_MSG(residue.head == 1u, label + ": the head marker is missing");
      UC_CHECK_MSG(residue.lock == 1u, label + ": the writer lock file is missing");

      const StoreView view = inspect(store);
      UC_CHECK_MSG(view.read_only_opened,
                   label + ": the read-only open failed: " + view.read_only_status.to_string());
      UC_CHECK_MSG(view.open_ok,
                   label + ": the read-write reopen failed: " + view.open_status.to_string());
      UC_CHECK_MSG(view.audit_ok,
                   label + ": the reopened store could not be audited: " +
                       view.audit_status.to_string());
      UC_CHECK_MSG(view.verified,
                   label + ": full verification failed: " + view.verify_status.to_string());
      UC_CHECK_MSG(view.state_valid,
                   label + ": the decoded state is structurally invalid: " +
                       view.state_status.to_string());
      UC_CHECK_MSG(view.residue_after_read_only.staging == residue.staging &&
                       view.residue_after_read_only.head_tmp == residue.head_tmp,
                   label + ": a read-only open modified crashed-writer residue");
      UC_CHECK_MSG(view.residue_after_open.staging == 0u,
                   label + ": staging residue survived the read-write reopen");
      UC_CHECK_MSG(view.residue_after_open.head_tmp == 0u,
                   label + ": head-tmp residue survived the read-write reopen");
      UC_CHECK_MSG(view.residue_after_open.payloads == 1u,
                   label + ": the reopen left " + std::to_string(view.residue_after_open.payloads) +
                       " generation payloads instead of exactly one");
      UC_CHECK_MSG(view.residue_after_open.head == 1u, label + ": the head marker vanished");

      const std::uint64_t expected_generation =
          store.generation.value() + (is_head_commit_point(point) ? ordinal : ordinal - 1);
      UC_CHECK_MSG(view.generation.value() == expected_generation,
                   label + ": the surviving generation is " +
                       std::to_string(view.generation.value()) + " where the protocol requires " +
                       std::to_string(expected_generation));
      UC_CHECK_MSG(view.digest != 0, label + ": the surviving generation has no canonical digest");
      UC_CHECK_MSG(view.unit_count == 1u, label + ": the surviving generation lost its unit");

      // The adapter is invoked only after the durable plan committed and before
      // the acknowledgement commit begins (ordinal 3).
      const std::size_t expected_effects = ordinal == 3 ? 1u : 0u;
      UC_CHECK_MSG(view.effects_lines == expected_effects,
                   label + ": the adapter effects log holds " + std::to_string(view.effects_lines) +
                       " lines where the durable protocol requires " +
                       std::to_string(expected_effects));
    }
  }
}

UC_TEST(crash, recovery_adopts_exactly_the_pre_or_post_commit_generation) {
  // Reference digests: the logical states the probe durably commits, obtained
  // from the head-commit crashes, which publish the commit before they die.
  std::vector<std::uint64_t> reference;
  PreparedStore base = prepare("crash-digest-base");
  if (!base.ready) {
    UC_CHECK_MSG(false, base.error);
    return;
  }
  reference.push_back(base.digest);
  for (std::uint64_t ordinal = 1; ordinal <= 3; ++ordinal) {
    const std::string label = crash_label("after_head_commit", ordinal);
    PreparedStore store = prepare("crash-digest-reference-" + std::to_string(ordinal));
    if (!store.ready) {
      UC_CHECK_MSG(false, label + ": " + store.error);
      return;
    }
    UC_CHECK_MSG(store.digest == reference.front(),
                 label + ": the reference stores do not start from the same logical state");
    int exit_code = -1;
    std::string error;
    const bool started = uc_test::run_probe(
        probe_arguments(store, "digest-key", "after_head_commit", ordinal), exit_code, error);
    UC_CHECK_MSG(started, label + ": " + error);
    UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");
    const StoreView view = inspect(store);
    UC_CHECK_MSG(view.generation.value() == store.generation.value() + ordinal,
                 label + ": the committed generation is " +
                     std::to_string(view.generation.value()) + " instead of " +
                     std::to_string(store.generation.value() + ordinal));
    UC_CHECK_MSG(view.digest != reference.back(),
                 label + ": the commit did not change the canonical digest");
    reference.push_back(view.digest);
  }

  // A crash before the head replacement must leave exactly the state the probe
  // had durably committed before the crashing commit.
  for (const char* point : kPreHeadPoints) {
    for (std::uint64_t ordinal = 1; ordinal <= 3; ++ordinal) {
      const std::string label = crash_label(point, ordinal);
      PreparedStore store = prepare("crash-digest-" + label);
      if (!store.ready) {
        UC_CHECK_MSG(false, label + ": " + store.error);
        continue;
      }
      UC_CHECK_MSG(store.digest == reference.front(),
                   label + ": the prepared store is not logically identical to the reference");
      int exit_code = -1;
      std::string error;
      const bool started =
          uc_test::run_probe(probe_arguments(store, "digest-key", point, ordinal), exit_code, error);
      UC_CHECK_MSG(started, label + ": " + error);
      UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");
      const StoreView view = inspect(store);
      UC_CHECK_MSG(view.generation.value() == store.generation.value() + ordinal - 1,
                   label + ": the surviving generation is " +
                       std::to_string(view.generation.value()) + " instead of " +
                       std::to_string(store.generation.value() + ordinal - 1));
      const std::size_t index = static_cast<std::size_t>(ordinal - 1);
      UC_CHECK_MSG(view.digest == reference[index],
                   label + ": the surviving state is not the state the probe committed before the "
                            "crashing commit: digest " +
                       std::to_string(view.digest) + " instead of " + std::to_string(reference[index]));
    }
  }
}

UC_TEST(crash, before_the_durable_plan_there_is_no_attempt_and_no_adapter_effect) {
  for (const char* point : kPreHeadPoints) {
    for (std::uint64_t ordinal = 1; ordinal <= 2; ++ordinal) {
      const std::string label = crash_label(point, ordinal);
      PreparedStore store = prepare("crash-before-plan-" + label);
      if (!store.ready) {
        UC_CHECK_MSG(false, label + ": " + store.error);
        continue;
      }
      int exit_code = -1;
      std::string error;
      const bool started = uc_test::run_probe(
          probe_arguments(store, "before-plan-key", point, ordinal), exit_code, error);
      UC_CHECK_MSG(started, label + ": " + error);
      UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");
      const StoreView view = inspect(store);
      UC_CHECK_MSG(view.effects_lines == 0u,
                   label + ": the adapter was invoked " + std::to_string(view.effects_lines) +
                       " times although the durable plan was never committed");
      UC_CHECK_MSG(view.attempt_count == 0u,
                   label + ": the journal holds " + std::to_string(view.attempt_count) +
                       " attempts although the durable plan was never committed");
      UC_CHECK_MSG(view.journal.empty(), label + ": the journal is not empty");
      UC_CHECK_MSG(!view.in_flight.has_value(), label + ": the in-flight gate is held");
      UC_CHECK_MSG(view.unresolved_count == 0u, label + ": an unresolved attempt was recovered");
      const std::uint64_t expected_generation = store.generation.value() + ordinal - 1;
      UC_CHECK_MSG(view.generation.value() == expected_generation,
                   label + ": the surviving generation is " +
                       std::to_string(view.generation.value()) + " instead of " +
                       std::to_string(expected_generation));

      // The key was never bound, so the same key is a brand new request: this
      // time it reaches the adapter exactly once, which is what proves that the
      // crashed plan left no trace at all.
      EngineHandle handle = open_read_write(store);
      if (!handle.ok()) {
        UC_CHECK_MSG(false,
                     label + ": the store could not be reopened: " + handle.status.to_string());
        continue;
      }
      RevalidateRequest revalidate;
      revalidate.authority = authority_context();
      revalidate.now = Tick{kParentTick};
      UC_CHECK_MSG(handle.engine->revalidate(revalidate).ok(), label + ": revalidation failed");
      Result<UpsRecord> unit = single_unit(handle.engine);
      if (!unit.ok()) {
        UC_CHECK_MSG(false, label + ": the unit is missing after the reopen");
        continue;
      }
      const Status recorded =
          record_observation(handle.engine, unit.value(), OperatingState::OnlineNormal,
                             Tick{kParentTick}, 2, "post-crash-observation");
      UC_CHECK_MSG(recorded.ok(), label + ": the observation was refused: " + recorded.to_string());
      unit = single_unit(handle.engine);
      if (!unit.ok()) {
        UC_CHECK_MSG(false, label + ": the unit vanished");
        continue;
      }
      const Result<AttemptRecord> resubmitted = handle.engine->submit(command_for(
          unit.value(), CommandKind::EnterStaticBypass, "before-plan-key", Tick{kParentTick}));
      UC_CHECK_MSG(resubmitted.ok(),
                   label + ": the retry was refused: " + resubmitted.status().to_string());
      if (resubmitted.ok()) {
        UC_CHECK_MSG(resubmitted.value().phase == AttemptPhase::Acknowledged,
                     label + ": the retry reached phase " +
                         std::string(to_string(resubmitted.value().phase)));
      }
      UC_CHECK_MSG(uc_test::count_lines(store.effects_path) == 1u,
                   label + ": the retry did not reach the adapter exactly once, the effects log "
                           "holds " +
                       std::to_string(uc_test::count_lines(store.effects_path)) + " lines");
      UC_CHECK_MSG(handle.adapter->issue_count() == 1u,
                   label + ": the adapter issued " + std::to_string(handle.adapter->issue_count()) +
                       " commands instead of 1");
      (void)handle.engine->close();
    }
  }
}

UC_TEST(crash, after_the_durable_plan_the_retry_is_idempotent_and_never_reissues) {
  // Ordinal 2 at the head-commit crash point: the durable plan is published and
  // the process dies before it reaches the adapter.
  const std::string label = crash_label("after_head_commit", 2);
  PreparedStore store = prepare("crash-after-plan");
  if (!store.ready) {
    UC_CHECK_MSG(false, label + ": " + store.error);
    return;
  }
  int exit_code = -1;
  std::string error;
  const bool started = uc_test::run_probe(
      probe_arguments(store, "after-plan-key", "after_head_commit", 2), exit_code, error);
  UC_CHECK_MSG(started, label + ": " + error);
  UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");

  const StoreView recovered = inspect(store);
  UC_CHECK_MSG(recovered.effects_lines == 0u,
               label + ": the adapter was invoked although the crash preceded the issuance");
  UC_CHECK_MSG(recovered.attempt_count == 1u,
               label + ": the journal holds " + std::to_string(recovered.attempt_count) +
                   " attempts instead of the single durable plan");
  if (recovered.journal.size() != 1u) {
    return;
  }
  const AttemptId planned = recovered.journal.front().id;
  UC_CHECK_MSG(recovered.journal.front().phase == AttemptPhase::Planned,
               label + ": the recovered attempt is " +
                   std::string(to_string(recovered.journal.front().phase)) + " instead of planned");
  UC_CHECK_MSG(recovered.in_flight.has_value() && recovered.in_flight.value() == planned,
               label + ": the unit does not hold the in-flight gate for the recovered plan");

  EngineHandle handle = open_read_write(store);
  if (!handle.ok()) {
    UC_CHECK_MSG(false,
                 label + ": the store could not be reopened: " + handle.status.to_string());
    return;
  }
  RevalidateRequest revalidate;
  revalidate.authority = authority_context();
  revalidate.now = Tick{kParentTick};
  UC_CHECK_MSG(handle.engine->revalidate(revalidate).ok(), label + ": revalidation failed");

  Result<UpsRecord> unit = single_unit(handle.engine);
  if (!unit.ok()) {
    UC_CHECK_MSG(false, label + ": the unit is missing");
    return;
  }
  const Result<AttemptRecord> replay = handle.engine->submit(
      command_for(unit.value(), CommandKind::EnterStaticBypass, "after-plan-key", Tick{kParentTick}));
  UC_CHECK_MSG(replay.ok(),
               label + ": the idempotent replay failed: " + replay.status().to_string());
  if (replay.ok()) {
    UC_CHECK_MSG(replay.value().id == planned,
                 label + ": the retry produced attempt " +
                     std::to_string(replay.value().id.value()) + " instead of the durable plan " +
                     std::to_string(planned.value()));
    UC_CHECK_MSG(replay.value().phase == AttemptPhase::Planned,
                 label + ": the retry changed the recovered phase to " +
                     std::string(to_string(replay.value().phase)));
  }
  UC_CHECK_MSG(handle.adapter->issue_count() == 0u,
               label + ": the retry reached the adapter " +
                   std::to_string(handle.adapter->issue_count()) + " times");
  UC_CHECK_MSG(uc_test::count_lines(store.effects_path) == 0u,
               label + ": the retry wrote " +
                   std::to_string(uc_test::count_lines(store.effects_path)) +
                   " adapter effect lines");

  // A different key is a different intent and the single-device gate refuses it.
  // A fresh observation is recorded first, so that the gate and not a stale
  // transfer status is the reason.
  unit = single_unit(handle.engine);
  if (!unit.ok()) {
    UC_CHECK_MSG(false, label + ": the unit vanished");
    return;
  }
  const Status recorded = record_observation(handle.engine, unit.value(),
                                             OperatingState::OnlineNormal, Tick{kParentTick}, 2,
                                             "gate-observation");
  UC_CHECK_MSG(recorded.ok(), label + ": the observation was refused: " + recorded.to_string());
  unit = single_unit(handle.engine);
  if (!unit.ok()) {
    UC_CHECK_MSG(false, label + ": the unit vanished after the observation");
    return;
  }
  const Result<AttemptRecord> other = handle.engine->submit(command_for(
      unit.value(), CommandKind::EnterStaticBypass, "after-plan-other-key", Tick{kParentTick}));
  UC_CHECK_MSG(other.ok(),
               label + ": the different-key submission failed outright: " + other.status().to_string());
  if (other.ok()) {
    UC_CHECK_MSG(other.value().phase == AttemptPhase::Refused,
                 label + ": a different key reached phase " +
                     std::string(to_string(other.value().phase)) + " while the gate was held");
    UC_CHECK_MSG(other.value().refusal.has_value() &&
                     other.value().refusal->code == RefusalCode::TransitionInProgress,
                 label + ": a different key was refused with " +
                     (other.value().refusal.has_value()
                          ? std::string(to_string(other.value().refusal->code))
                          : std::string("no refusal recorded")) +
                     " instead of transition_in_progress");
  }
  UC_CHECK_MSG(handle.adapter->issue_count() == 0u, label + ": the refused key reached the adapter");
  UC_CHECK_MSG(uc_test::count_lines(store.effects_path) == 0u,
               label + ": the refused key wrote an adapter effect line");

  // The original key still replays to the original attempt.
  const Result<UpsRecord> final_unit = single_unit(handle.engine);
  if (final_unit.ok()) {
    const Result<AttemptRecord> again = handle.engine->submit(command_for(
        final_unit.value(), CommandKind::EnterStaticBypass, "after-plan-key", Tick{kParentTick}));
    UC_CHECK_MSG(again.ok() && again.value().id == planned,
                 label + ": the second replay did not return the durable plan");
  }
  (void)handle.engine->close();
}

UC_TEST(crash, after_the_acknowledgement_commit_the_device_was_addressed_exactly_once) {
  const std::string label = crash_label("after_head_commit", 3);
  PreparedStore store = prepare("crash-after-ack");
  if (!store.ready) {
    UC_CHECK_MSG(false, label + ": " + store.error);
    return;
  }
  int exit_code = -1;
  std::string error;
  const bool started = uc_test::run_probe(
      probe_arguments(store, "after-ack-key", "after_head_commit", 3), exit_code, error);
  UC_CHECK_MSG(started, label + ": " + error);
  UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");

  const StoreView recovered = inspect(store);
  UC_CHECK_MSG(recovered.effects_lines == 1u,
               label + ": the effects log holds " + std::to_string(recovered.effects_lines) +
                   " lines instead of exactly one");
  UC_CHECK_MSG(recovered.attempt_count == 1u,
               label + ": the journal holds " + std::to_string(recovered.attempt_count) +
                   " attempts instead of one");
  if (recovered.journal.size() != 1u) {
    return;
  }
  const AttemptId acknowledged = recovered.journal.front().id;
  UC_CHECK_MSG(recovered.journal.front().phase == AttemptPhase::Acknowledged,
               label + ": the recovered attempt is " +
                   std::string(to_string(recovered.journal.front().phase)) +
                   " instead of acknowledged");
  UC_CHECK_MSG(recovered.in_flight.has_value() && recovered.in_flight.value() == acknowledged,
               label + ": an acknowledged but unverified attempt no longer holds the gate");

  EngineHandle handle = open_read_write(store);
  if (!handle.ok()) {
    UC_CHECK_MSG(false,
                 label + ": the store could not be reopened: " + handle.status.to_string());
    return;
  }
  RevalidateRequest revalidate;
  revalidate.authority = authority_context();
  revalidate.now = Tick{kParentTick};
  UC_CHECK_MSG(handle.engine->revalidate(revalidate).ok(), label + ": revalidation failed");

  const Result<UpsRecord> unit = single_unit(handle.engine);
  if (!unit.ok()) {
    UC_CHECK_MSG(false, label + ": the unit is missing");
    return;
  }
  const Result<AttemptRecord> replay = handle.engine->submit(command_for(
      unit.value(), CommandKind::EnterStaticBypass, "after-ack-key", Tick{kParentTick}));
  UC_CHECK_MSG(replay.ok() && replay.value().id == acknowledged,
               label + ": the retry did not replay the acknowledged attempt");
  UC_CHECK_MSG(handle.adapter->issue_count() == 0u,
               label + ": the retry reached the adapter a second time");
  UC_CHECK_MSG(uc_test::count_lines(store.effects_path) == 1u,
               label + ": the retry added a second adapter effect line, the log holds " +
                   std::to_string(uc_test::count_lines(store.effects_path)) + " lines");
  (void)handle.engine->close();
}

UC_TEST(crash, between_the_issue_and_the_acknowledgement_commit_no_acknowledgement_is_recorded) {
  for (const char* point : kPreHeadPoints) {
    const std::string label = crash_label(point, 3);
    PreparedStore store = prepare("crash-during-ack-" + std::string(point));
    if (!store.ready) {
      UC_CHECK_MSG(false, label + ": " + store.error);
      continue;
    }
    int exit_code = -1;
    std::string error;
    const bool started =
        uc_test::run_probe(probe_arguments(store, "during-ack-key", point, 3), exit_code, error);
    UC_CHECK_MSG(started, label + ": " + error);
    UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");
    const StoreView view = inspect(store);
    UC_CHECK_MSG(view.effects_lines == 1u,
                 label + ": the device was not addressed exactly once, the effects log holds " +
                     std::to_string(view.effects_lines) + " lines");
    if (view.journal.size() != 1u) {
      UC_CHECK_MSG(false, label + ": the journal holds " + std::to_string(view.journal.size()) +
                              " attempts instead of one");
      continue;
    }
    const AttemptRecord& attempt = view.journal.front();
    UC_CHECK_MSG(attempt.phase == AttemptPhase::Planned,
                 label + ": the acknowledgement survived a crash before its commit; the attempt is " +
                     std::string(to_string(attempt.phase)));
    UC_CHECK_MSG(attempt.ack == AckOutcome::None,
                 label + ": an acknowledgement outcome was recorded although its commit died");
    UC_CHECK_MSG(view.in_flight.has_value() && view.in_flight.value() == attempt.id,
                 label + ": the recovered plan does not hold the in-flight gate");
    UC_CHECK_MSG(view.unresolved_count == 1u,
                 label + ": the recovered attempt is not counted as unresolved");
  }
}

UC_TEST(crash, a_crashed_store_still_runs_a_complete_command_lifecycle) {
  for (const char* point : kAllPoints) {
    for (std::uint64_t ordinal = 1; ordinal <= 3; ++ordinal) {
      const std::string label = crash_label(point, ordinal);
      PreparedStore store = prepare("crash-lifecycle-" + label);
      if (!store.ready) {
        UC_CHECK_MSG(false, label + ": " + store.error);
        continue;
      }
      int exit_code = -1;
      std::string error;
      const bool started = uc_test::run_probe(
          probe_arguments(store, "lifecycle-probe-key", point, ordinal), exit_code, error);
      UC_CHECK_MSG(started, label + ": " + error);
      UC_CHECK_MSG(exit_code == kCrashExitCode, label + ": the probe did not crash");
      const LifecycleResult lifecycle = complete_lifecycle(store, Tick{kParentTick});
      UC_CHECK_MSG(lifecycle.ok,
                   label + ": the store no longer runs a control lifecycle: " + lifecycle.detail);
      const StoreView view = inspect(store);
      UC_CHECK_MSG(view.verified, label + ": the store fails verification after the lifecycle: " +
                                      view.verify_status.to_string());
      UC_CHECK_MSG(view.residue_after_open.staging == 0u && view.residue_after_open.head_tmp == 0u &&
                       view.residue_after_open.payloads == 1u,
                   label + ": the store directory is not clean after the lifecycle");
      // The journal holds whatever attempt survived the crash plus exactly the
      // one the completed lifecycle added.
      const std::size_t survived = ordinal == 3 ? 1u
                                                : (ordinal == 2 && is_head_commit_point(point) ? 1u
                                                                                               : 0u);
      UC_CHECK_MSG(view.attempt_count == survived + 1u,
                   label + ": the journal holds " + std::to_string(view.attempt_count) +
                       " attempts where the surviving attempt plus the completed lifecycle "
                       "requires " + std::to_string(survived + 1u));
    }
  }
}




