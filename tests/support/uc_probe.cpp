// The independent-process probe used by the multiprocess and crash-recovery tests.
//
// Every scenario opens a real store through the real library, and a crash scenario
// terminates the process at a durable stage of the real commit protocol. Nothing
// here simulates process death with a thread.
//
// Invocation: uc_probe <scenario> key=value ...
//
//   store=PATH        the store path (required)
//   ready=PATH        written with \c pid=N once the scenario is established
//   release=PATH      the scenario proceeds once this file exists
//   result=PATH       key=value results written when the scenario completes
//   tick / epoch / incarnation / ups / hardware / revision / key / command / authority
//   crash=POINT       a durable crash point name
//   nth=N             the commit ordinal at which the crash point fires
//   create=1          create the store when it does not exist
//   revalidate=1      revalidate before acting
//   ack=OUTCOME       the synthetic adapter acknowledgement for the command
//   effects=PATH      the synthetic adapter effects log

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ups_control/engine.hpp"

namespace {

using namespace ups_control;

using Arguments = std::map<std::string, std::string>;

std::string value_of(const Arguments& arguments, const std::string& key,
                     const std::string& fallback = std::string()) {
  const auto found = arguments.find(key);
  return found == arguments.end() ? fallback : found->second;
}

std::int64_t number_of(const Arguments& arguments, const std::string& key, std::int64_t fallback) {
  const std::string text = value_of(arguments, key);
  if (text.empty()) {
    return fallback;
  }
  return std::strtoll(text.c_str(), nullptr, 10);
}

bool flag_of(const Arguments& arguments, const std::string& key) {
  return value_of(arguments, key) == "1";
}

void write_results(const Arguments& arguments, const std::vector<std::string>& lines) {
  const std::string path = value_of(arguments, "result");
  if (path.empty()) {
    for (const std::string& line : lines) {
      std::cout << line << "\n";
    }
    std::cout.flush();
    return;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  for (const std::string& line : lines) {
    stream << line << "\n";
  }
  stream.flush();
}

void signal_ready(const Arguments& arguments) {
  const std::string path = value_of(arguments, "ready");
  if (path.empty()) {
    return;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
#if defined(_WIN32)
  stream << "pid=" << _getpid() << "\n";
#else
  stream << "pid=" << ::getpid() << "\n";
#endif
  stream.flush();
}

bool wait_for_release(const Arguments& arguments) {
  const std::string path = value_of(arguments, "release");
  if (path.empty()) {
    return true;
  }
  for (int attempt = 0; attempt < 20000; ++attempt) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error && size > 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::cerr << "probe: the release file never appeared\n";
  return false;
}

DurableCrashPoint parse_crash_point(const std::string& text) {
  if (text == "before_staging_write") {
    return DurableCrashPoint::BeforeStagingWrite;
  }
  if (text == "after_staging_write") {
    return DurableCrashPoint::AfterStagingWrite;
  }
  if (text == "after_staging_flush") {
    return DurableCrashPoint::AfterStagingFlush;
  }
  if (text == "after_staging_verify") {
    return DurableCrashPoint::AfterStagingVerify;
  }
  if (text == "after_head_commit") {
    return DurableCrashPoint::AfterHeadCommit;
  }
  return DurableCrashPoint::None;
}

struct Opened {
  std::shared_ptr<UpsControlEngine> engine;
  Status status;
};

Opened open_engine(const Arguments& arguments, bool writable, bool with_adapter) {
  EngineOpenOptions options;
  options.store.path = value_of(arguments, "store");
  options.store.access = writable ? StoreAccess::ReadWrite : StoreAccess::ReadOnly;
  options.store.create_if_missing = flag_of(arguments, "create");
  options.store.created_at = Tick{number_of(arguments, "created_at", 1)};
  options.store.epoch = ControlEpoch{static_cast<std::uint64_t>(number_of(arguments, "epoch", 1))};
  options.store.incarnation =
      Incarnation{static_cast<std::uint64_t>(number_of(arguments, "incarnation", 1))};
  const std::string crash = value_of(arguments, "crash");
  options.store.crash_at = parse_crash_point(crash);
  options.store.crash_after_commits =
      static_cast<std::uint64_t>(number_of(arguments, "nth", 0));
  if (with_adapter) {
    SyntheticAdapter::Script script;
    script.effects_log = value_of(arguments, "effects");
    const std::string ack = value_of(arguments, "ack");
    const std::string command = value_of(arguments, "command");
    if (!ack.empty() && !command.empty()) {
      const Result<CommandKind> kind = parse_command_kind(command);
      if (kind.ok()) {
        AckOutcome outcome = AckOutcome::Accepted;
        if (ack == "no_response") {
          outcome = AckOutcome::NoResponse;
        } else if (ack == "rejected") {
          outcome = AckOutcome::Rejected;
        } else if (ack == "unsupported") {
          outcome = AckOutcome::Unsupported;
        }
        script.acknowledgements[kind.value()] = outcome;
      }
    }
    const std::string unsupported = value_of(arguments, "unsupported");
    if (!unsupported.empty()) {
      const Result<CommandKind> kind = parse_command_kind(unsupported);
      if (kind.ok()) {
        script.supported_commands &= ~command_kind_bit(kind.value());
      }
    }
    options.adapter = std::make_shared<SyntheticAdapter>(std::move(script));
  }
  const Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(options);
  Opened result;
  if (!opened.ok()) {
    result.status = opened.status();
    return result;
  }
  result.engine = opened.value();
  return result;
}

int scenario_hold(const Arguments& arguments) {
  Opened opened = open_engine(arguments, true, false);
  if (opened.engine == nullptr) {
    std::cerr << "probe: hold could not open the store: " << opened.status.to_string() << "\n";
    return 1;
  }
  signal_ready(arguments);
  if (!wait_for_release(arguments)) {
    return 1;
  }
  return opened.engine->close().ok() ? 0 : 1;
}

int scenario_submit(const Arguments& arguments) {
  const bool crashing = !value_of(arguments, "crash").empty();
  Opened opened = open_engine(arguments, true, true);
  if (opened.engine == nullptr) {
    std::cerr << "probe: submit could not open the store: " << opened.status.to_string() << "\n";
    return 1;
  }
  signal_ready(arguments);
  if (!wait_for_release(arguments)) {
    return 1;
  }
  const Tick tick{number_of(arguments, "tick", 1)};
  const ControlContext context{ControlEpoch{static_cast<std::uint64_t>(number_of(arguments, "epoch", 1))},
                               Incarnation{static_cast<std::uint64_t>(number_of(arguments, "incarnation", 1))}};
  if (flag_of(arguments, "revalidate")) {
    RevalidateRequest revalidate;
    revalidate.authority = context;
    revalidate.now = tick;
    const Status status = opened.engine->revalidate(revalidate).status();
    if (!status.ok()) {
      std::cerr << "probe: revalidate failed: " << status.to_string() << "\n";
      return 1;
    }
  }
  ControlCommand command;
  command.authority = context;
  command.now = tick;
  const Result<CommandKind> kind = parse_command_kind(value_of(arguments, "command", "enter_static_bypass"));
  if (!kind.ok()) {
    std::cerr << "probe: " << kind.status().message() << "\n";
    return 1;
  }
  command.kind = kind.value();
  const Result<UpsId> ups = UpsId::parse(value_of(arguments, "ups", "ups-under-test"));
  if (!ups.ok()) {
    std::cerr << "probe: " << ups.status().message() << "\n";
    return 1;
  }
  command.ref.ups = ups.value();
  command.ref.hardware =
      HardwareGeneration{static_cast<std::uint32_t>(number_of(arguments, "hardware", 1))};
  command.ref.revision = StateRevision{static_cast<std::uint64_t>(number_of(arguments, "revision", 1))};
  const Result<IdempotencyKey> key = IdempotencyKey::parse(value_of(arguments, "key", "probe-key"));
  if (!key.ok()) {
    std::cerr << "probe: " << key.status().message() << "\n";
    return 1;
  }
  command.key = key.value();
  const std::string authority = value_of(arguments, "authority");
  if (!authority.empty()) {
    const Result<AuthorityRef> parsed = AuthorityRef::parse(authority);
    if (parsed.ok()) {
      command.authority_ref = parsed.value();
    }
  }

  const Result<AttemptRecord> attempt = opened.engine->submit(command);
  if (!attempt.ok()) {
    std::vector<std::string> lines;
    lines.push_back("submit_status=" + attempt.status().to_string());
    write_results(arguments, lines);
    // The store is still whole; report the refusal through the exit code so a
    // caller cannot mistake it for a completed attempt.
    return crashing ? 0 : 3;
  }
  std::vector<std::string> lines;
  lines.push_back("attempt=" + std::to_string(attempt.value().id.value()));
  lines.push_back(std::string("phase=") + to_string(attempt.value().phase));
  lines.push_back(std::string("ack=") + to_string(attempt.value().ack));
  if (attempt.value().refusal.has_value()) {
    lines.push_back(std::string("refusal=") + to_string(attempt.value().refusal->code));
  }
  write_results(arguments, lines);
  const Status closed = opened.engine->close();
  if (!closed.ok()) {
    std::cerr << "probe: close failed: " << closed.to_string() << "\n";
    return 1;
  }
  return 0;
}

int scenario_read(const Arguments& arguments) {
  Opened opened = open_engine(arguments, false, false);
  if (opened.engine == nullptr) {
    std::vector<std::string> lines;
    lines.push_back("open_status=" + opened.status.to_string());
    write_results(arguments, lines);
    return 2;
  }
  const Result<StoreAuditReport> audit = opened.engine->store_audit();
  if (!audit.ok()) {
    std::vector<std::string> lines;
    lines.push_back("audit_status=" + audit.status().to_string());
    write_results(arguments, lines);
    return 2;
  }
  std::vector<std::string> lines;
  lines.push_back("generation=" + std::to_string(audit.value().generation.value()));
  lines.push_back("epoch=" + std::to_string(audit.value().epoch.value()));
  lines.push_back("incarnation=" + std::to_string(audit.value().incarnation.value()));
  lines.push_back("units=" + std::to_string(audit.value().unit_count));
  lines.push_back("attempts=" + std::to_string(audit.value().attempt_count));
  lines.push_back("unresolved=" + std::to_string(audit.value().unresolved_attempt_count));
  lines.push_back("digest=" + std::to_string(audit.value().canonical_digest));
  lines.push_back("store_identity=" + audit.value().store_identity.to_hex());
  write_results(arguments, lines);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: uc_probe <scenario> key=value ...\n";
    return 1;
  }
  const std::string scenario = argv[1];
  Arguments arguments;
  for (int index = 2; index < argc; ++index) {
    const std::string token = argv[index];
    const std::size_t equals = token.find('=');
    if (equals == std::string::npos) {
      std::cerr << "probe: '" << token << "' is not of the form key=value\n";
      return 1;
    }
    arguments[token.substr(0, equals)] = token.substr(equals + 1);
  }
  if (arguments.find("store") == arguments.end() && scenario != "noop") {
    std::cerr << "probe: store=PATH is required\n";
    return 1;
  }
  if (scenario == "hold") {
    return scenario_hold(arguments);
  }
  if (scenario == "submit") {
    return scenario_submit(arguments);
  }
  if (scenario == "read") {
    return scenario_read(arguments);
  }
  std::cerr << "probe: unknown scenario '" << scenario << "'\n";
  return 1;
}
