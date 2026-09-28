// Real independent processes: writer exclusion, release by process death, epoch
// handoff and stale-incarnation fencing.
//
// Every process here is started with the operating system's own spawn and is
// ended either by its own clean exit or by a real operating-system termination.
// Nothing establishes a fact by waiting: the readiness loops can only fail, and
// every "the lock is free" claim is checked by actually taking it.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "fixture.hpp"
#include "proc.hpp"
#include "test_harness.hpp"

#include "ups_control/engine.hpp"
#include "ups_control/store.hpp"

using namespace ups_control;

namespace {

constexpr const char* kUpsText = "ups-under-test";
constexpr const char* kGrantText = "test-authority";
/// Inside the freshness window of the prepared observation (observed at 1000
/// with max_evidence_age 600), so a probe submission is evaluated on its merits.
constexpr std::int64_t kProbeTick = 1500;
/// After the prepared store's last committed instant and after any handoff.
constexpr std::int64_t kParentTick = 6000;
/// Bound of every readiness poll. A loop that runs out fails; it never turns a
/// missing fact into a pass.
constexpr int kPollAttempts = 20000;

/// A started probe process. The harness spawn helper discards the operating
/// system's process identifier and handle, so this local one keeps both: the exit
/// code of a process that loses the writer-lock race is a fact the tests have to
/// observe, not assume.
struct Child {
  bool started = false;
  std::uint64_t pid = 0;
  bool exited = false;
  int exit_code = -1;
  bool exit_code_known = false;
  std::string error;
#if defined(_WIN32)
  void* handle = nullptr;
#endif
};

/// What the operating system reports about one started probe process.
struct ProcessState {
  /// True when the process is known not to be running any more.
  bool exited = false;
  int exit_code = -1;
  /// True when the exit code was actually retrieved rather than defaulted.
  bool exit_code_known = false;
};

/// Reports, and remembers, whether the process has exited and with what code.
ProcessState query_child(Child& child) {
  ProcessState state;
  if (!child.exited) {
#if defined(_WIN32)
    if (child.handle == nullptr) {
      child.exited = true;
    } else {
      const HANDLE handle = static_cast<HANDLE>(child.handle);
      if (::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (::GetExitCodeProcess(handle, &code) != 0) {
          child.exit_code = static_cast<int>(code);
          child.exit_code_known = true;
        }
        child.exited = true;
        (void)::CloseHandle(handle);
        child.handle = nullptr;
      }
    }
#else
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG);
    if (result == static_cast<pid_t>(child.pid)) {
      child.exited = true;
      child.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      child.exit_code_known = WIFEXITED(status);
    } else if (result < 0 && errno == ECHILD) {
      if (::kill(static_cast<pid_t>(child.pid), 0) != 0 && errno == ESRCH) {
        child.exited = true;
      }
    }
#endif
  }
  state.exited = child.exited;
  state.exit_code = child.exit_code;
  state.exit_code_known = child.exit_code_known;
  return state;
}

/// Waits until the process is gone and returns its exit code.
bool wait_for_exit(Child& child, int& exit_code, std::string& error) {
  for (int attempt = 0; attempt < kPollAttempts; ++attempt) {
    const ProcessState state = query_child(child);
    if (state.exited) {
      exit_code = state.exit_code;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  error = "process " + std::to_string(child.pid) + " never exited";
  return false;
}

std::int64_t now_nanos() {
  return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
}

/// A deterministic store prepared through the public API, with the writer lock
/// released so that a probe process can take it.
struct PreparedStore {
  std::unique_ptr<uc_test::ScratchDirectory> scratch;
  std::filesystem::path store_path;
  std::filesystem::path effects_path;
  StoreGeneration generation;
  std::uint64_t digest = 0;
  StateRevision revision;
  bool ready = false;
  std::string error;

  std::filesystem::path file(const std::string& name) const { return scratch->file(name); }
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
  prepared.generation = audit.value().generation;
  prepared.digest = audit.value().canonical_digest;
  prepared.revision = unit.value().revision;
  (void)fixture.engine->close();
  fixture.engine.reset();
  fixture.adapter.reset();
  prepared.ready = true;
  return prepared;
}

#if defined(_WIN32)
std::string quote_argument(const std::string& value) {
  if (value.find_first_of(" \t\"") == std::string::npos) {
    return value;
  }
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += "\"";
  return quoted;
}
#endif

/// Renders a probe result file for a failure message.
std::string render_values(const std::map<std::string, std::string>& values) {
  std::string text;
  for (const auto& entry : values) {
    if (!text.empty()) {
      text += " ";
    }
    text += entry.first + "=" + entry.second;
  }
  return text.empty() ? std::string("<no result was written>") : text;
}

std::vector<std::string> hold_arguments(const PreparedStore& store, const std::string& ready,
                                        const std::string& release) {
  std::vector<std::string> arguments;
  arguments.push_back("hold");
  arguments.push_back("store=" + store.store_path.string());
  arguments.push_back("ready=" + store.file(ready).string());
  arguments.push_back("release=" + store.file(release).string());
  return arguments;
}

Child start_probe(const std::vector<std::string>& arguments) {
  Child child;
  const std::string executable = uc_test::probe_path();
#if defined(_WIN32)
  std::vector<std::string> storage;
  storage.reserve(arguments.size() + 2);
  storage.push_back(quote_argument(executable));
  for (const std::string& argument : arguments) {
    storage.push_back(quote_argument(argument));
  }
  std::vector<const char*> pointers;
  pointers.reserve(storage.size() + 1);
  for (const std::string& value : storage) {
    pointers.push_back(value.c_str());
  }
  pointers.push_back(nullptr);
  const intptr_t result = _spawnv(_P_NOWAIT, executable.c_str(), pointers.data());
  if (result == -1) {
    child.error = "could not start the probe process";
    return child;
  }
  const HANDLE handle = reinterpret_cast<HANDLE>(result);
  child.pid = static_cast<std::uint64_t>(::GetProcessId(handle));
  // The handle is kept so that the exit code stays observable after the process
  // object would otherwise have been released by the operating system.
  child.handle = handle;
#else
  std::vector<char*> pointers;
  pointers.push_back(const_cast<char*>(executable.c_str()));
  for (const std::string& argument : arguments) {
    pointers.push_back(const_cast<char*>(argument.c_str()));
  }
  pointers.push_back(nullptr);
  pid_t child_pid = 0;
  if (posix_spawn(&child_pid, executable.c_str(), nullptr, nullptr, pointers.data(), environ) != 0) {
    child.error = "could not start the probe process";
    return child;
  }
  child.pid = static_cast<std::uint64_t>(child_pid);
#endif
  if (child.pid == 0) {
    child.error = "the started probe has no process identifier";
    return child;
  }
  child.started = true;
  return child;
}

Result<std::shared_ptr<UpsStore>> open_store(const std::filesystem::path& path, StoreAccess access) {
  StoreOpenOptions options;
  options.path = path;
  options.access = access;
  options.create_if_missing = false;
  return UpsStore::open(options);
}

struct OpenedEngine {
  std::shared_ptr<UpsControlEngine> engine;
  std::shared_ptr<SyntheticAdapter> adapter;
  Status status;
  bool ok() const { return engine != nullptr; }
};

OpenedEngine open_engine(const std::filesystem::path& path, StoreAccess access,
                         const std::filesystem::path& effects = {}) {
  OpenedEngine opened;
  EngineOpenOptions options;
  options.store.path = path;
  options.store.access = access;
  options.store.create_if_missing = false;
  if (!effects.empty()) {
    SyntheticAdapter::Script script;
    script.effects_log = effects;
    opened.adapter = std::make_shared<SyntheticAdapter>(std::move(script));
    options.adapter = opened.adapter;
  }
  const Result<std::shared_ptr<UpsControlEngine>> engine = UpsControlEngine::open(options);
  opened.status = engine.status();
  if (engine.ok()) {
    opened.engine = engine.value();
  }
  return opened;
}

struct StoreFacts {
  bool present = false;
  StoreGeneration generation;
  std::uint64_t digest = 0;
  std::size_t attempts = 0;
  Status status;
};

/// Reads the committed generation through a read-only engine. A read-only open
/// takes no writer lock and observes either one whole verified generation or
/// nothing at all.
StoreFacts read_facts(const std::filesystem::path& path) {
  StoreFacts facts;
  OpenedEngine opened = open_engine(path, StoreAccess::ReadOnly);
  facts.status = opened.status;
  if (!opened.ok()) {
    return facts;
  }
  const Result<StoreAuditReport> audit = opened.engine->store_audit();
  facts.status = audit.status();
  if (audit.ok()) {
    facts.present = true;
    facts.generation = audit.value().generation;
    facts.digest = audit.value().canonical_digest;
    facts.attempts = audit.value().attempt_count;
  }
  (void)opened.engine->close();
  return facts;
}

ControlContext authority_context() {
  return ControlContext{ControlEpoch{1}, Incarnation{1}};
}

}  // namespace

UC_TEST(multiprocess, writer_exclusion_blocks_a_second_writer_and_read_only_observes) {
  PreparedStore store = prepare("mp-exclusion");
  if (!store.ready) {
    UC_CHECK_MSG(false, store.error);
    return;
  }
  const std::filesystem::path ready = store.file("hold.ready");
  const std::filesystem::path release = store.file("hold.release");
  Child child = start_probe(hold_arguments(store, "hold.ready", "hold.release"));
  UC_CHECK_MSG(child.started, child.error);
  if (!child.started) {
    return;
  }
  std::string error;
  UC_CHECK_MSG(uc_test::wait_for_file(ready, kPollAttempts, error), error);
  const std::uint64_t reported = uc_test::read_pid(ready);
  UC_CHECK_MSG(reported != 0, "the ready file does not name a process");
  UC_CHECK_MSG(reported == child.pid,
               "the ready file names process " + std::to_string(reported) +
                   " but the started process is " + std::to_string(child.pid));

  const Result<std::shared_ptr<UpsStore>> blocked =
      open_store(store.store_path, StoreAccess::ReadWrite);
  UC_CHECK_MSG(!blocked.ok(),
               "a second read-write open succeeded while another process held the writer lock");
  UC_REQUIRE_STATUS(blocked, StatusCode::LockConflict);

  // A read-only open takes no writer lock, and it observes one whole verified
  // generation: not a partial one, not a stitched one.
  const Result<std::shared_ptr<UpsStore>> reader =
      open_store(store.store_path, StoreAccess::ReadOnly);
  UC_CHECK_MSG(reader.ok(), "the read-only open failed: " + reader.status().to_string());
  if (reader.ok()) {
    const std::shared_ptr<const UpsState>& state = reader.value()->opened_state();
    UC_CHECK_MSG(state != nullptr, "the read-only open produced no state");
    if (state != nullptr) {
      UC_CHECK_MSG(validate_state(*state, ResourceLimits::defaults()).ok(),
                   "the observed state is structurally invalid");
      UC_CHECK_MSG(state->generation == store.generation,
                   "the read-only open observed generation " +
                       std::to_string(state->generation.value()) + " instead of " +
                       std::to_string(store.generation.value()));
      UC_CHECK_MSG(canonical_state_digest(*state) == store.digest,
                   "the read-only open observed a different logical state");
    }
    (void)reader.value()->close();
  }

  // The same fact from an independent process, while the writer still holds it.
  int reader_exit = -1;
  std::string reader_error;
  std::vector<std::string> read_arguments;
  read_arguments.push_back("read");
  read_arguments.push_back("store=" + store.store_path.string());
  read_arguments.push_back("result=" + store.file("reader.result").string());
  const bool reader_started = uc_test::run_probe(read_arguments, reader_exit, reader_error);
  UC_CHECK_MSG(reader_started, reader_error);
  UC_CHECK_MSG(reader_exit == 0,
               "the independent reader exited with " + std::to_string(reader_exit));
  const std::map<std::string, std::string> values =
      uc_test::read_key_values(store.file("reader.result"));
  const auto generation = values.find("generation");
  const auto digest = values.find("digest");
  UC_CHECK_MSG(generation != values.end() &&
                   generation->second == std::to_string(store.generation.value()),
               "the independent reader observed a different generation");
  UC_CHECK_MSG(digest != values.end() && digest->second == std::to_string(store.digest),
               "the independent reader observed a different logical state");

  // Release the holder and let it exit on its own.
  uc_test::write_text(release, "release\n");
  int exit_code = -1;
  UC_CHECK_MSG(wait_for_exit(child, exit_code, error), error);
  UC_CHECK_MSG(exit_code == 0,
               "the holder exited with " + std::to_string(exit_code) + " instead of 0");

  const Result<std::shared_ptr<UpsStore>> reopened =
      open_store(store.store_path, StoreAccess::ReadWrite);
  UC_CHECK_MSG(reopened.ok(),
               "the writer lock was not released by a clean exit: " +
                   reopened.status().to_string());
  if (reopened.ok()) {
    (void)reopened.value()->close();
  }
  const StoreFacts facts = read_facts(store.store_path);
  UC_CHECK_MSG(facts.present, facts.status.to_string());
  UC_CHECK_MSG(facts.generation == store.generation && facts.digest == store.digest,
               "merely holding the store changed it");
}

UC_TEST(multiprocess, process_death_releases_writer_authority_without_a_recovery_step) {
  PreparedStore store = prepare("mp-death");
  if (!store.ready) {
    UC_CHECK_MSG(false, store.error);
    return;
  }
  const std::filesystem::path ready = store.file("hold.ready");
  Child child = start_probe(hold_arguments(store, "hold.ready", "hold.release"));
  UC_CHECK_MSG(child.started, child.error);
  if (!child.started) {
    return;
  }
  std::string error;
  UC_CHECK_MSG(uc_test::wait_for_file(ready, kPollAttempts, error), error);
  const Result<std::shared_ptr<UpsStore>> blocked =
      open_store(store.store_path, StoreAccess::ReadWrite);
  UC_REQUIRE_STATUS(blocked, StatusCode::LockConflict);

  // Terminate the holder at the operating-system level. No destructor, no
  // flush-on-exit, no cooperative shutdown of any kind runs.
  UC_CHECK_MSG(uc_test::terminate_process(child.pid, error), error);
  int exit_code = -1;
  UC_CHECK_MSG(wait_for_exit(child, exit_code, error), error);

  // No recovery step: the next read-write open simply succeeds.
  const Result<std::shared_ptr<UpsStore>> reopened =
      open_store(store.store_path, StoreAccess::ReadWrite);
  UC_CHECK_MSG(reopened.ok(),
               "process death did not release writer authority: " +
                   reopened.status().to_string());
  if (reopened.ok()) {
    UC_CHECK_MSG(reopened.value()->writable(), "the reopened store is not writable");
    (void)reopened.value()->close();
  }
  UC_CHECK_MSG(UpsStore::verify_file(store.store_path).ok(),
               "the store no longer verifies after a writer was killed");
  const StoreFacts facts = read_facts(store.store_path);
  UC_CHECK_MSG(facts.present, facts.status.to_string());
  UC_CHECK_MSG(facts.generation == store.generation,
               "the generation changed although the killed writer never committed");
  UC_CHECK_MSG(facts.digest == store.digest,
               "the logical state changed although the killed writer never committed");
  UC_CHECK_MSG(facts.attempts == 0u, "the killed writer left an attempt behind");
}

UC_TEST(multiprocess, a_probe_that_cannot_take_the_lock_exits_nonzero_without_a_ready_file) {
  PreparedStore store = prepare("mp-no-lock");
  if (!store.ready) {
    UC_CHECK_MSG(false, store.error);
    return;
  }
  const std::filesystem::path ready = store.file("loser.ready");
  OpenedEngine holder = open_engine(store.store_path, StoreAccess::ReadWrite);
  UC_CHECK_MSG(holder.ok(), "the parent could not take the writer lock: " +
                                holder.status.to_string());
  if (!holder.ok()) {
    return;
  }
  Child child = start_probe(hold_arguments(store, "loser.ready", "loser.release"));
  UC_CHECK_MSG(child.started, child.error);
  if (!child.started) {
    return;
  }
  std::string error;
  int exit_code = -1;
  UC_CHECK_MSG(wait_for_exit(child, exit_code, error), error);
  UC_CHECK_MSG(exit_code == 1,
               "a probe that cannot take the writer lock exited with " +
                   std::to_string(exit_code) + " instead of 1");
  UC_CHECK_MSG(!uc_test::file_ready(ready),
               "a probe that never held the writer lock published a ready file");
  (void)holder.engine->close();
}

UC_TEST(multiprocess, concurrent_probes_yield_exactly_one_holder_and_disjoint_intervals) {
  constexpr std::size_t kRounds = 3;
  constexpr std::size_t kProbes = 3;
  PreparedStore store = prepare("mp-election");
  if (!store.ready) {
    UC_CHECK_MSG(false, store.error);
    return;
  }
  struct Interval {
    /// The conservative bounds of the tenure: from before the round's probes were
    /// spawned to after the holder was confirmed gone.
    std::int64_t start = 0;
    std::int64_t end = 0;
    /// The exact instants required by the proof: the ready file was observed and
    /// the release file was created.
    std::int64_t ready_observed_at = 0;
    std::int64_t released_at = 0;
    std::uint64_t pid = 0;
  };
  std::vector<Interval> intervals;
  for (std::size_t round = 0; round < kRounds; ++round) {
    std::vector<Child> children;
    std::vector<std::filesystem::path> readies;
    std::vector<std::filesystem::path> releases;
    const std::int64_t spawned_at = now_nanos();
    for (std::size_t index = 0; index < kProbes; ++index) {
      const std::string suffix = "-r" + std::to_string(round) + "-p" + std::to_string(index);
      readies.push_back(store.file("hold" + suffix + ".ready"));
      releases.push_back(store.file("hold" + suffix + ".release"));
      children.push_back(start_probe(hold_arguments(store, "hold" + suffix + ".ready",
                                                    "hold" + suffix + ".release")));
      UC_CHECK_MSG(children.back().started, children.back().error);
    }
    if (!children.back().started) {
      return;
    }

    // Settle the round: every process is either the holder (its ready file is
    // published) or gone. This is an assertion about the running processes, not
    // a timing assumption.
    bool settled = false;
    std::string error;
    for (int attempt = 0; attempt < kPollAttempts && !settled; ++attempt) {
      settled = true;
      for (std::size_t index = 0; index < kProbes; ++index) {
        if (uc_test::file_ready(readies[index])) {
          continue;
        }
        if (!query_child(children[index]).exited) {
          settled = false;
        }
      }
      if (!settled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    UC_CHECK_MSG(settled, "round " + std::to_string(round) + ": the probes never settled");

    std::size_t holders = 0;
    std::size_t holder_index = 0;
    for (std::size_t index = 0; index < kProbes; ++index) {
      if (uc_test::file_ready(readies[index])) {
        ++holders;
        holder_index = index;
      }
    }
    UC_CHECK_MSG(holders == 1, "round " + std::to_string(round) + ": " +
                                   std::to_string(holders) +
                                   " probes became the writer-lock holder instead of one");

    // Every process that did not become the holder had to fail with the probe's
    // lock-conflict exit code, and it must not have published a ready file.
    for (std::size_t index = 0; index < kProbes; ++index) {
      if (index == holder_index) {
        continue;
      }
      const ProcessState state = query_child(children[index]);
      UC_CHECK_MSG(state.exited, "round " + std::to_string(round) + ": a losing probe is still running");
      UC_CHECK_MSG(state.exit_code == 1,
                   "round " + std::to_string(round) + ": a losing probe exited with " +
                       std::to_string(state.exit_code) + " instead of 1");
      UC_CHECK_MSG(!uc_test::file_ready(readies[index]),
                   "round " + std::to_string(round) + ": a losing probe published a ready file");
    }

    // While the holder holds, the parent cannot take the lock: that is the fact
    // that makes the losers' exit code a lock conflict rather than anything else.
    const Result<std::shared_ptr<UpsStore>> blocked =
        open_store(store.store_path, StoreAccess::ReadWrite);
    UC_CHECK_MSG(!blocked.ok(), "round " + std::to_string(round) +
                                    ": the writer lock was free while a holder held it");
    UC_REQUIRE_STATUS(blocked, StatusCode::LockConflict);
    const std::int64_t held_at = now_nanos();

    const std::int64_t released_at = now_nanos();
    uc_test::write_text(releases[holder_index], "release\n");
    int exit_code = -1;
    UC_CHECK_MSG(wait_for_exit(children[holder_index], exit_code, error), error);
    UC_CHECK_MSG(exit_code == 0, "round " + std::to_string(round) + ": the holder exited with " +
                                     std::to_string(exit_code) + " instead of 0");
    const std::int64_t lock_free_at = now_nanos();
    const Result<std::shared_ptr<UpsStore>> free_open =
        open_store(store.store_path, StoreAccess::ReadWrite);
    UC_CHECK_MSG(free_open.ok(), "round " + std::to_string(round) +
                                     ": the writer lock was not released by the holder's exit: " +
                                     free_open.status().to_string());
    if (free_open.ok()) {
      (void)free_open.value()->close();
    }
    Interval interval;
    interval.start = spawned_at;
    interval.end = lock_free_at;
    interval.ready_observed_at = held_at;
    interval.released_at = released_at;
    interval.pid = children[holder_index].pid;
    UC_CHECK_MSG(held_at > spawned_at && released_at >= held_at && lock_free_at >= released_at,
                 "round " + std::to_string(round) + ": the round's instants are inconsistent");
    intervals.push_back(interval);
    const StoreFacts facts = read_facts(store.store_path);
    UC_CHECK_MSG(facts.generation == store.generation && facts.digest == store.digest,
                 "round " + std::to_string(round) + ": holding the store changed it");
  }

  for (std::size_t left = 0; left < intervals.size(); ++left) {
    for (std::size_t right = left + 1; right < intervals.size(); ++right) {
      const bool disjoint = intervals[left].end <= intervals[right].start ||
                            intervals[right].end <= intervals[left].start;
      UC_CHECK_MSG(disjoint, "the holding intervals of processes " +
                                 std::to_string(intervals[left].pid) + " and " +
                                 std::to_string(intervals[right].pid) + " overlap");
      // The same claim on the tighter interval: from the instant the ready file
      // was observed to the instant the release file was created.
      const bool tight_disjoint =
          intervals[left].released_at <= intervals[right].ready_observed_at ||
          intervals[right].released_at <= intervals[left].ready_observed_at;
      UC_CHECK_MSG(tight_disjoint, "processes " + std::to_string(intervals[left].pid) + " and " +
                                       std::to_string(intervals[right].pid) +
                                       " were both lock holders at the same time");
    }
  }
}

std::vector<std::string> submit_arguments(const PreparedStore& store, const std::string& key,
                                          std::uint64_t incarnation, bool revalidate,
                                          std::int64_t tick, std::uint64_t revision,
                                          const std::string& result_name,
                                          const std::string& authority = kGrantText) {
  std::vector<std::string> arguments;
  arguments.push_back("submit");
  arguments.push_back("store=" + store.store_path.string());
  arguments.push_back("effects=" + store.effects_path.string());
  arguments.push_back("result=" + store.file(result_name).string());
  arguments.push_back("tick=" + std::to_string(tick));
  arguments.push_back("epoch=1");
  arguments.push_back("incarnation=" + std::to_string(incarnation));
  if (revalidate) {
    arguments.push_back("revalidate=1");
  }
  arguments.push_back("command=enter_static_bypass");
  arguments.push_back("ups=" + std::string(kUpsText));
  arguments.push_back("hardware=1");
  arguments.push_back("revision=" + std::to_string(revision));
  arguments.push_back("key=" + key);
  arguments.push_back("authority=" + authority);
  return arguments;
}

UC_TEST(multiprocess, a_superseded_incarnation_is_fenced_out_of_the_store) {
  PreparedStore store = prepare("mp-fencing");
  if (!store.ready) {
    UC_CHECK_MSG(false, store.error);
    return;
  }
  // One acknowledged attempt under incarnation 1, from a real process.
  int exit_code = -1;
  std::string error;
  const bool first_started = uc_test::run_probe(
      submit_arguments(store, "fence-key", 1, true, kProbeTick, store.revision.value(), "first.result"),
      exit_code, error);
  UC_CHECK_MSG(first_started, error);
  UC_CHECK_MSG(exit_code == 0, "the first probe exited with " + std::to_string(exit_code));
  const std::map<std::string, std::string> first = uc_test::read_key_values(store.file("first.result"));
  const auto first_phase = first.find("phase");
  UC_CHECK_MSG(first_phase != first.end() && first_phase->second == "acknowledged",
               "the first probe did not reach the acknowledged phase");

  const StoreFacts before_handoff = read_facts(store.store_path);
  UC_CHECK_MSG(before_handoff.present, before_handoff.status.to_string());
  UC_CHECK_MSG(before_handoff.attempts == 1u, "the first probe left no attempt behind");

  // The parent adopts incarnation 2 and takes a fresh observation under it, so a
  // later probe is evaluated on its merits.
  OpenedEngine parent = open_engine(store.store_path, StoreAccess::ReadWrite, store.effects_path);
  UC_CHECK_MSG(parent.ok(), "the parent could not open the store: " + parent.status.to_string());
  if (!parent.ok()) {
    return;
  }
  AdoptAuthorityRequest adopt;
  adopt.authority = authority_context();
  adopt.now = Tick{2000};
  adopt.epoch = ControlEpoch{1};
  adopt.incarnation = Incarnation{2};
  const Result<ControlContext> adopted = parent.engine->adopt_authority(adopt);
  UC_CHECK_MSG(adopted.ok(), "the forward handoff was refused: " + adopted.status().to_string());
  if (adopted.ok()) {
    UC_CHECK_MSG(adopted.value().incarnation == Incarnation{2},
                 "the handoff did not adopt incarnation 2");
  }
  const Result<std::vector<UpsRecord>> units = parent.engine->units();
  UC_CHECK_MSG(units.ok() && !units.value().empty(), "the unit is missing after the handoff");
  std::uint64_t revision_after_handoff = store.revision.value();
  if (units.ok() && !units.value().empty()) {
    const UpsRecord& unit = units.value().front();
    revision_after_handoff = unit.revision.value();
    RecordTelemetryRequest telemetry;
    telemetry.authority = ControlContext{ControlEpoch{1}, Incarnation{2}};
    telemetry.ref = UpsRef{unit.id, unit.hardware, unit.revision};
    telemetry.now = Tick{2100};
    telemetry.report = uc_test::healthy_report(unit.id, unit.hardware, Tick{2100},
                                               SourceRevision{2}, "post-handoff-observation");
    const Result<ObservationRecord> recorded = parent.engine->record_telemetry(telemetry);
    UC_CHECK_MSG(recorded.ok(),
                 "the post-handoff observation was refused: " + recorded.status().to_string());
    revision_after_handoff = revision_after_handoff + 1;
  }
  // A grant bound to the new incarnation, so that the current authority can be
  // evaluated on its merits rather than being fenced like the superseded one.
  if (units.ok() && !units.value().empty()) {
    const Result<std::vector<UpsRecord>> current = parent.engine->units();
    if (current.ok() && !current.value().empty()) {
      const UpsRecord& unit = current.value().front();
      IssueGrantRequest grant;
      grant.authority = ControlContext{ControlEpoch{1}, Incarnation{2}};
      grant.ref = UpsRef{unit.id, unit.hardware, unit.revision};
      grant.now = Tick{2100};
      grant.grant.ref = AuthorityRef::parse("fresh-authority").value();
      grant.grant.scope = GrantScope::BypassTransfer;
      grant.grant.granted_by = AuthorityRef::parse("facility-ops").value();
      grant.grant.issued_at = Tick{2100};
      grant.grant.expires_at = Tick{100000000};
      const Result<AuthorityGrant> issued = parent.engine->issue_grant(grant);
      UC_CHECK_MSG(issued.ok(),
                   "the grant for the new incarnation was refused: " + issued.status().to_string());
      if (issued.ok()) {
        revision_after_handoff = revision_after_handoff + 1;
      }
    }
  }
  (void)parent.engine->close();

  const StoreFacts after_handoff = read_facts(store.store_path);
  UC_CHECK_MSG(after_handoff.present, after_handoff.status.to_string());
  UC_CHECK_MSG(after_handoff.generation.value() > before_handoff.generation.value(),
               "the handoff did not advance the store");

  // The superseded incarnation cannot even revalidate, and the store is
  // untouched: generation and logical state are identical afterwards.
  int stale_exit = -1;
  std::string stale_error;
  const bool stale_started = uc_test::run_probe(
      submit_arguments(store, "fence-old-key", 1, true, 2100, revision_after_handoff,
                       "stale.result"),
      stale_exit, stale_error);
  UC_CHECK_MSG(stale_started, stale_error);
  UC_CHECK_MSG(stale_exit != 0, "a superseded incarnation was allowed to act on the store");
  const StoreFacts after_stale = read_facts(store.store_path);
  UC_CHECK_MSG(after_stale.present, after_stale.status.to_string());
  UC_CHECK_MSG(after_stale.generation == after_handoff.generation,
               "a fenced process changed the generation: " +
                   std::to_string(after_stale.generation.value()) + " instead of " +
                   std::to_string(after_handoff.generation.value()));
  UC_CHECK_MSG(after_stale.digest == after_handoff.digest,
               "a fenced process changed the logical state");

  // Without revalidation the same superseded incarnation is refused on the
  // merits, and the refusal names the stale incarnation rather than anything
  // else.
  int refusal_exit = -1;
  std::string refusal_error;
  const bool refusal_started = uc_test::run_probe(
      submit_arguments(store, "fence-old-key", 1, false, 2100, revision_after_handoff,
                       "old.result"),
      refusal_exit, refusal_error);
  UC_CHECK_MSG(refusal_started, refusal_error);
  UC_CHECK_MSG(refusal_exit == 0,
               "the refusal probe exited with " + std::to_string(refusal_exit));
  const std::map<std::string, std::string> old =
      uc_test::read_key_values(store.file("old.result"));
  const auto refusal = old.find("refusal");
  UC_CHECK_MSG(refusal != old.end() && refusal->second == "stale_incarnation",
               "the superseded incarnation was refused with " +
                   (refusal == old.end() ? std::string("no recorded refusal") : refusal->second) +
                   " instead of stale_incarnation; the result file holds: " + render_values(old));

  // The new incarnation gets past revalidation and is evaluated on its merits:
  // the single-device gate of the still unresolved first attempt refuses it.
  int fresh_exit = -1;
  std::string fresh_error;
  const bool fresh_started = uc_test::run_probe(
      submit_arguments(store, "fence-new-key", 2, true, 2100, revision_after_handoff,
                       "new.result", "fresh-authority"),
      fresh_exit, fresh_error);
  UC_CHECK_MSG(fresh_started, fresh_error);
  const std::map<std::string, std::string> fresh =
      uc_test::read_key_values(store.file("new.result"));
  UC_CHECK_MSG(fresh_exit == 0, "the new-incarnation probe exited with " +
                                    std::to_string(fresh_exit) + "; the result file holds: " +
                                    render_values(fresh));
  const auto fresh_refusal = fresh.find("refusal");
  UC_CHECK_MSG(fresh_refusal != fresh.end() && fresh_refusal->second == "transition_in_progress",
               "the current incarnation was not evaluated on its merits: " +
                   (fresh_refusal == fresh.end() ? std::string("no recorded refusal")
                                                 : fresh_refusal->second) +
                   "; the result file holds: " + render_values(fresh));

  // Across all five processes the device was addressed exactly once, by the one
  // process that held the current incarnation and a free writer lock.
  UC_CHECK_MSG(uc_test::count_lines(store.effects_path) == 1u,
               "the device was addressed " + std::to_string(uc_test::count_lines(store.effects_path)) +
                   " times instead of exactly once");
}


