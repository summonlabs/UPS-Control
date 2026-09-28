// Proof obligations for the ups-control command line tool.
//
// The CLI is the real administration path, so every test here starts a real
// ups-control process through uc_test::run_captured (the child's combined output
// is redirected to a file, never captured through a pipe), drives a real store
// through real commands, and asserts on the tool's own output and exit code.
// Nothing here waits for a fact to become true and nothing here is bounded by a
// timeout: a command that does not finish is a defect to diagnose.
//
// Two contracts shape every assertion below.
//
//   * A freshly opened engine is always EngineLifecycle::Recovered, because state
//     read from a store is not current evidence. Every command that carries an
//     explicit --now and opens the store writable revalidates at that instant
//     before doing anything else, so instant discipline matters: --now must never
//     move backwards between two commands that write to the same store.
//   * Refusal precedence is the lowest numeric RefusalCode
//     (docs/authority-and-transitions.md), so a request that violates several
//     rules always reports the same primary refusal.
//
// Both views of the state basis agree: "status --ups", "status" without --ups and
// every library read path report the basis of a unit that was only read back from
// a store as Recovered, because persistence never carries a freshness claim across
// a restart.

#include <algorithm>
#include <exception>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fixture.hpp"
#include "proc.hpp"
#include "test_harness.hpp"
#include "ups_control/version.hpp"

namespace {

using namespace ups_control;
using namespace uc_test;

constexpr const char* kUps = "ups-under-test";
constexpr const char* kGrant = "grant-1";
constexpr const char* kMaintenanceGrant = "grant-maintenance";
constexpr const char* kTestGrant = "grant-test";
constexpr const char* kGrantedBy = "facility-ops";

// ---------------------------------------------------------------------------
// Running the tool
// ---------------------------------------------------------------------------

std::string join_lines(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) {
    if (!text.empty()) {
      text += "\n";
    }
    text += line;
  }
  return text;
}

bool has_substring(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

/// Parses a decimal integer, refusing an empty string and trailing garbage.
bool parse_decimal(const std::string& text, std::int64_t& value) {
  if (text.empty()) {
    return false;
  }
  std::size_t consumed = 0;
  try {
    value = std::stoll(text, &consumed, 10);
  } catch (const std::exception&) {
    return false;
  }
  return consumed == text.size();
}

/// Everything one CLI process produced: its exit code, its output lines, and its
/// output as one text block. The tool merges stdout and stderr into the same file,
/// so a test asserts on lines and never on the order of two streams.
struct CliRun {
  int exit_code = -1;
  std::vector<std::string> lines;
  std::string text;

  bool has_line(const std::string& expected) const {
    return std::find(lines.begin(), lines.end(), expected) != lines.end();
  }
  bool has_text(const std::string& needle) const { return has_substring(text, needle); }
  std::size_t line_count() const { return lines.size(); }

  /// The first line that begins with the prefix, or the empty string.
  std::string line_starting_with(const std::string& prefix) const {
    for (const std::string& line : lines) {
      if (line.rfind(prefix, 0) == 0) {
        return line;
      }
    }
    return std::string();
  }

  /// The value of the first key=value line, or the empty string.
  std::string value_of(const std::string& key) const {
    const std::string line = line_starting_with(key + "=");
    return line.empty() ? std::string() : line.substr(key.size() + 1);
  }
};

std::string exit_detail(const CliRun& run, int expected) {
  return "expected exit " + std::to_string(expected) + ", actual " +
         std::to_string(run.exit_code) + "; output was:\n" + run.text;
}

std::string line_detail(const CliRun& run, const std::string& expected) {
  return "expected line '" + expected + "' in:\n" + run.text;
}

std::string text_detail(const CliRun& run, const std::string& needle) {
  return "expected text '" + needle + "' in:\n" + run.text;
}

#define UC_CHECK_EXIT(run, expected) UC_CHECK_MSG((run).exit_code == (expected), exit_detail((run), (expected)))
#define UC_CHECK_LINE(run, expected_line) UC_CHECK_MSG((run).has_line(expected_line), line_detail((run), (expected_line)))
#define UC_CHECK_TEXT(run, needle) UC_CHECK_MSG((run).has_text(needle), text_detail((run), (needle)))

CliRun run_cli(const ScratchDirectory& scratch, const std::vector<std::string>& arguments) {
  static std::uint64_t counter = 0;
  ++counter;
  const std::filesystem::path output =
      scratch.file("cli-" + std::to_string(counter) + "-output.txt");
  CliRun run;
  std::string error;
  if (!uc_test::run_captured(uc_test::cli_path(), arguments, output, run.exit_code, error)) {
    run.text = "the CLI process could not be started: " + error;
    run.lines.push_back(run.text);
    return run;
  }
  run.lines = uc_test::read_lines(output);
  run.text = join_lines(run.lines);
  return run;
}

/// Runs the tool with --store inserted directly after the command, which is the
/// shape every command in this file uses.
CliRun run_store(const ScratchDirectory& scratch, const std::filesystem::path& store,
                 std::initializer_list<std::string> arguments) {
  std::vector<std::string> full;
  full.reserve(arguments.size() + 2);
  auto iterator = arguments.begin();
  if (iterator == arguments.end()) {
    return CliRun{};
  }
  full.push_back(*iterator);
  ++iterator;
  full.push_back("--store");
  full.push_back(store.string());
  for (; iterator != arguments.end(); ++iterator) {
    full.push_back(*iterator);
  }
  return run_cli(scratch, full);
}

std::vector<std::string> with_store(const std::filesystem::path& store,
                                    std::initializer_list<std::string> arguments) {
  std::vector<std::string> full;
  full.reserve(arguments.size() + 2);
  auto iterator = arguments.begin();
  if (iterator == arguments.end()) {
    return full;
  }
  full.push_back(*iterator);
  ++iterator;
  full.push_back("--store");
  full.push_back(store.string());
  for (; iterator != arguments.end(); ++iterator) {
    full.push_back(*iterator);
  }
  return full;
}

/// A scratch store prepared the way the documented administration path prepares
/// one: init, register, one healthy observation, one bypass-transfer grant. Every
/// preparation step is a separate process, and the instants only move forward.
///
/// The store generations are deterministic: init commits generation 1, and each
/// later write command revalidates (one commit) and then commits its own mutation
/// (one commit), so the prepared store ends at generation 7.
struct ReadyStore {
  explicit ReadyStore(const std::string& test_name)
      : scratch("cli-" + test_name), path(scratch.file("unit.upsstore")) {}

  ScratchDirectory scratch;
  std::filesystem::path path;

  CliRun run(std::initializer_list<std::string> arguments) const {
    return run_store(scratch, path, arguments);
  }

  void prepare() {
    const CliRun initialized = run({"init", "--now", "100", "--epoch", "1", "--incarnation", "1"});
    UC_REQUIRE(initialized.exit_code == 0);
    const CliRun registered =
        run({"register", "--now", "110", "--ups", kUps, "--hardware", "1", "--lifecycle",
             "in_service", "--operating", "online_normal", "--floor-unit", "s", "--floor-value",
             "600", "--max-age", "600"});
    UC_REQUIRE(registered.exit_code == 0);
    const CliRun observed =
        run({"observe", "--now", "120", "--ups", kUps, "--operating", "online_normal",
             "--reserve-unit", "s", "--reserve-value", "900", "--reserve-quality", "measured",
             "--bypass-kind", "static", "--bypass-available", "true", "--bypass-qualified",
             "true", "--synchronized", "true", "--transfer-ready", "true", "--battery-ready",
             "true", "--activity", "idle", "--evidence", "ev-1"});
    UC_REQUIRE(observed.exit_code == 0);
    const CliRun granted = run({"grant", "--now", "130", "--ups", kUps, "--ref", kGrant,
                                "--scope", "bypass_transfer", "--authority", kGrantedBy});
    UC_REQUIRE(granted.exit_code == 0);
  }

  /// Issues one more grant at the given instant, which must be at or after the
  /// last instant written to this store.
  CliRun grant(const std::string& reference, const std::string& scope, const std::string& at) const {
    return run({"grant", "--now", at, "--ups", kUps, "--ref", reference, "--scope", scope,
                "--authority", kGrantedBy});
  }

  /// Binds one life-safety obligation that must remain protected.
  CliRun bind_life_safety_obligation(const std::string& at) const {
    return run({"bind-obligation", "--now", at, "--ups", kUps, "--ref", "obl-1", "--load", "load-1",
                "--tier", "life_safety", "--protection", "must_remain_protected", "--asserted-by",
                "fire-marshal"});
  }

  /// Issues the standard bypass-transfer command and returns the run.
  CliRun enter_static_bypass(const std::string& key, const std::string& at) const {
    return run({"run", "--now", at, "--ups", kUps, "--command", "enter_static_bypass",
                "--authority", kGrant, "--key", key});
  }
};

// ---------------------------------------------------------------------------
// Output inspection
// ---------------------------------------------------------------------------

bool is_lower_hex(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool letter = character >= 'a' && character <= 'f';
    if (!digit && !letter) {
      return false;
    }
  }
  return true;
}

/// The JSON escaping a correct emitter must apply to a plain ASCII string value.
std::string json_escape_ascii(const std::string& value) {
  std::string escaped;
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      escaped += '\\';
    }
    escaped += character;
  }
  return escaped;
}

/// A structural check for one emitted JSON value. This is deliberately not a
/// parser: it proves that braces and brackets balance outside strings, that every
/// string is opened and closed on the same line, and that no raw control byte
/// (a newline or a tab) ever appears inside a string. A raw quote or a raw newline
/// inside a string value desynchronises the scan and fails.
bool json_is_structurally_sound(const std::string& text) {
  if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
    return false;
  }
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20u) {
      return false;
    }
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
    } else if (character == '{' || character == '[') {
      ++depth;
    } else if (character == '}' || character == ']') {
      --depth;
      if (depth < 0) {
        return false;
      }
    }
  }
  return !in_string && !escaped && depth == 0;
}

/// The raw text of the field named by key_token (for example "canonical_digest":)
/// up to the next top-level comma or closing brace. String values keep their
/// surrounding quotes, which is enough to compare the same field as reported by
/// two different commands.
std::string json_value_after(const std::string& line, const std::string& key_token) {
  const std::size_t position = line.find(key_token);
  if (position == std::string::npos) {
    return std::string();
  }
  const std::size_t start = position + key_token.size();
  bool in_string = false;
  bool escaped = false;
  std::size_t end = start;
  for (; end < line.size(); ++end) {
    const char character = line[end];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        in_string = false;
      }
      continue;
    }
    if (character == '"') {
      in_string = true;
      continue;
    }
    if (character == ',' || character == '}') {
      break;
    }
  }
  return line.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Command surface
// ---------------------------------------------------------------------------

UC_TEST(cli, version_prints_the_exact_library_version) {
  ScratchDirectory scratch("cli-version");
  const CliRun run = run_cli(scratch, {"version"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_EQ(run.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_EQ(run.text, std::string("ups-control ") + Version::string);
  UC_CHECK_EQ(std::string(Version::string),
              std::to_string(Version::major) + "." + std::to_string(Version::minor) + "." +
                  std::to_string(Version::patch));
}

UC_TEST(cli, help_exits_zero_and_prints_usage) {
  ScratchDirectory scratch("cli-help");
  const std::vector<std::vector<std::string>> forms = {
      {"help"}, {"--help"}, {"-h"}, {"status", "--help"}};
  for (const std::vector<std::string>& form : forms) {
    const CliRun run = run_cli(scratch, form);
    UC_CHECK_EXIT(run, 0);
    UC_CHECK_TEXT(run, "usage: ups-control <command> [options]");
    UC_CHECK_TEXT(run, "Exit codes: 0 success or allowed, 2 the model refused the request,");
    UC_CHECK_TEXT(run, "version                       print the library version");
  }
}

UC_TEST(cli, no_arguments_prints_usage_and_exits_one) {
  ScratchDirectory scratch("cli-no-arguments");
  const CliRun run = run_cli(scratch, {});
  UC_CHECK_EXIT(run, 1);
  UC_CHECK_TEXT(run, "usage: ups-control <command> [options]");
}

UC_TEST(cli, unknown_command_exits_one_and_prints_usage) {
  ScratchDirectory scratch("cli-unknown-command");
  const CliRun run = run_cli(scratch, {"frobnicate"});
  UC_CHECK_EXIT(run, 1);
  UC_CHECK_TEXT(run, "error: unknown command 'frobnicate'");
  UC_CHECK_TEXT(run, "usage: ups-control <command> [options]");
}

UC_TEST(cli, missing_required_options_are_named) {
  ScratchDirectory scratch("cli-missing-options");
  const std::filesystem::path store = scratch.file("unit.upsstore");
  UC_CHECK_EXIT(run_cli(scratch, {"init", "--store", store.string(), "--now", "100", "--epoch", "1",
                                  "--incarnation", "1"}),
                0);
  UC_CHECK_EXIT(run_cli(scratch, {"register", "--store", store.string(), "--now", "110", "--ups",
                                  kUps, "--hardware", "1"}),
                0);

  const CliRun no_store = run_cli(scratch, {"status", "--now", "10"});
  UC_CHECK_EXIT(no_store, 1);
  UC_CHECK_LINE(no_store, "error: --store is required");

  const CliRun no_now = run_cli(scratch, {"status", "--store", store.string()});
  UC_CHECK_EXIT(no_now, 1);
  UC_CHECK_LINE(no_now, "error: --now is required");

  const CliRun no_epoch = run_cli(scratch, {"init", "--store", store.string(), "--now", "10"});
  UC_CHECK_EXIT(no_epoch, 1);
  UC_CHECK_LINE(no_epoch, "error: --epoch is required");

  const CliRun no_command =
      run_cli(scratch, {"run", "--store", store.string(), "--now", "120", "--ups", kUps});
  UC_CHECK_EXIT(no_command, 1);
  UC_CHECK_LINE(no_command, "error: --command is required");

  const CliRun no_attempt = run_cli(scratch, {"verify", "--store", store.string(), "--now", "120"});
  UC_CHECK_EXIT(no_attempt, 1);
  UC_CHECK_LINE(no_attempt, "error: --attempt is required");

  const CliRun positional =
      run_cli(scratch, {"status", "--store", store.string(), "--now", "120", "extra"});
  UC_CHECK_EXIT(positional, 1);
  UC_CHECK_LINE(positional,
                "error: unexpected argument 'extra'; options are of the form --name value");

  const CliRun not_a_number =
      run_cli(scratch, {"status", "--store", store.string(), "--now", "abc"});
  UC_CHECK_EXIT(not_a_number, 1);
  UC_CHECK_LINE(not_a_number, "error: --now expects an integer, got 'abc'");
}

UC_TEST(cli, unknown_option_values_are_refused_and_never_remapped) {
  ReadyStore store("cli-unknown-values");
  store.prepare();

  const CliRun bad_command = store.run({"run", "--now", "140", "--ups", kUps, "--command",
                                        "go_to_bypass", "--key", "bad-1"});
  UC_CHECK_EXIT(bad_command, 1);
  UC_CHECK_LINE(bad_command,
                "error: unsupported command 'go_to_bypass'; this runtime never maps an unknown "
                "command onto a nearby operation");

  const CliRun bad_ack = store.run({"run", "--now", "140", "--ups", kUps, "--command",
                                    "enter_static_bypass", "--ack", "maybe", "--key", "bad-2"});
  UC_CHECK_EXIT(bad_ack, 1);
  UC_CHECK_LINE(bad_ack,
                "error: --ack expects accepted, rejected, unsupported, no-response, or malformed");

  const CliRun bad_unit = store.run({"observe", "--now", "140", "--ups", kUps, "--reserve-unit",
                                     "furlongs", "--reserve-value", "5"});
  UC_CHECK_EXIT(bad_unit, 1);
  UC_CHECK_LINE(bad_unit,
                "error: invalid_argument: unknown reserve unit 'furlongs'; accepted units are mwh, "
                "wh, kwh, s, bp");

  const CliRun bad_operating =
      store.run({"observe", "--now", "140", "--ups", kUps, "--operating", "nonsense"});
  UC_CHECK_EXIT(bad_operating, 1);
  UC_CHECK_LINE(bad_operating, "error: invalid_argument: unknown operating state 'nonsense'");

  const CliRun bad_scope = store.run({"grant", "--now", "140", "--ups", kUps, "--ref", "grant-2",
                                      "--scope", "nonsense"});
  UC_CHECK_EXIT(bad_scope, 1);
  UC_CHECK_LINE(bad_scope, "error: invalid_argument: unknown grant scope 'nonsense'");

  const CliRun bad_phase = store.run({"attempts", "--phase", "nonsense"});
  UC_CHECK_EXIT(bad_phase, 1);
  UC_CHECK_LINE(bad_phase, "error: unknown phase 'nonsense'");

  // "Never silently maps to a neighbour": none of the refused commands above may
  // have recorded an attempt or changed the authoritative state.
  const CliRun attempts = store.run({"attempts"});
  UC_CHECK_EXIT(attempts, 0);
  UC_CHECK_LINE(attempts, "count=0");
  const CliRun status = store.run({"status", "--now", "151", "--ups", kUps});
  UC_CHECK_EXIT(status, 0);
  UC_CHECK_LINE(status, "operating=online_normal");
  UC_CHECK_LINE(status, "state_revision=3");
}

// ---------------------------------------------------------------------------
// Store creation and inspection
// ---------------------------------------------------------------------------

UC_TEST(cli, init_creates_a_store_with_an_identity_and_generation_one) {
  ScratchDirectory scratch("cli-init");
  const std::filesystem::path store = scratch.file("unit.upsstore");
  const CliRun run = run_cli(scratch, {"init", "--store", store.string(), "--now", "100", "--epoch",
                                       "1", "--incarnation", "1"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "store=" + store.string());
  UC_CHECK_LINE(run, "generation=1");
  UC_CHECK_LINE(run, "epoch=1");
  UC_CHECK_LINE(run, "incarnation=1");
  const std::string identity = run.value_of("store_identity");
  UC_REQUIRE(!identity.empty());
  UC_CHECK_EQ(identity.size(), static_cast<std::size_t>(32));
  UC_CHECK_MSG(is_lower_hex(identity), "store_identity=" + identity);
  UC_CHECK(std::filesystem::exists(store));
}

UC_TEST(cli, init_twice_opens_the_existing_store_and_keeps_its_identity) {
  ScratchDirectory scratch("cli-init-twice");
  const std::filesystem::path store = scratch.file("unit.upsstore");
  const CliRun first = run_cli(scratch, {"init", "--store", store.string(), "--now", "100", "--epoch",
                                         "1", "--incarnation", "1"});
  UC_CHECK_EXIT(first, 0);
  const CliRun second = run_cli(scratch, {"init", "--store", store.string(), "--now", "500",
                                          "--epoch", "1", "--incarnation", "1"});
  UC_CHECK_EXIT(second, 0);
  UC_CHECK_EQ(second.value_of("store_identity"), first.value_of("store_identity"));
  UC_CHECK_LINE(second, "generation=1");

  // The durable creation instant is still the first one and the generation never
  // moved, so the second run opened the existing store instead of replacing it.
  const CliRun audit = run_cli(scratch, {"store-audit", "--store", store.string(), "--json"});
  UC_CHECK_EXIT(audit, 0);
  UC_CHECK_TEXT(audit, "\"created_at\":100");
  UC_CHECK_TEXT(audit, "\"generation\":1");
  UC_CHECK_TEXT(audit, "\"store_identity\":\"" + first.value_of("store_identity") + "\"");
}

UC_TEST(cli, register_reports_the_first_revision_and_generation) {
  ScratchDirectory scratch("cli-register");
  const std::filesystem::path store = scratch.file("unit.upsstore");
  UC_CHECK_EXIT(run_cli(scratch, {"init", "--store", store.string(), "--now", "100", "--epoch", "1",
                                  "--incarnation", "1"}),
                0);
  const CliRun run = run_cli(scratch, {"register", "--store", store.string(), "--now", "110", "--ups",
                                       kUps, "--hardware", "1", "--lifecycle", "in_service",
                                       "--operating", "online_normal"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "ups=ups-under-test");
  UC_CHECK_LINE(run, "hardware_generation=1");
  UC_CHECK_LINE(run, "lifecycle=in_service");
  UC_CHECK_LINE(run, "operating=online_normal");
  UC_CHECK_LINE(run, "state_revision=1");
  // The command revalidates the recovered store (generation 2) and then commits
  // the registration (generation 3).
  UC_CHECK_LINE(run, "generation=3");
}

UC_TEST(cli, status_by_ups_reports_the_exact_authoritative_facts) {
  ReadyStore store("cli-status");
  store.prepare();
  const CliRun run = store.run({"status", "--now", "150", "--ups", kUps});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "unit=ups-under-test");
  UC_CHECK_LINE(run, "label=ups-under-test");
  UC_CHECK_LINE(run, "hardware_generation=1");
  UC_CHECK_LINE(run, "epoch=1");
  UC_CHECK_LINE(run, "incarnation=1");
  UC_CHECK_LINE(run, "lifecycle=in_service");
  UC_CHECK_LINE(run, "operating=online_normal");
  // The state was read back from a store and this session has confirmed nothing,
  // so the reported basis is Recovered and no reserve claim is current.
  UC_CHECK_LINE(run, "state_basis=recovered");
  UC_CHECK_LINE(run, "state_revision=3");
  UC_CHECK_LINE(run, "state_since=110");
  UC_CHECK_LINE(run, "revalidated=false");
  UC_CHECK_LINE(run, "observation_freshness=fresh");
  UC_CHECK_LINE(run, "observation_evidence=ev-1");
  UC_CHECK_LINE(run, "observation_source=synthetic-source");
  UC_CHECK_LINE(run, "observation_provenance=synthetic_adapter");
  UC_CHECK_LINE(run, "observation_observed_at=120");
  UC_CHECK_LINE(run, "observation_age=30");
  UC_CHECK_LINE(run, "observation_contradictory=false");
  UC_CHECK_LINE(run, "reserve=900 s");
  UC_CHECK_LINE(run, "reserve_quality=measured");
  UC_CHECK_LINE(run, "reserve_outcome=indeterminate");
  UC_CHECK_LINE(run, "reserve_reason=not_revalidated");
  UC_CHECK_LINE(run, "obligations=0");
  UC_CHECK_LINE(run, "grant=grant-1 scope=bypass_transfer allowed=true revoked=false");
  // The tool appends the readiness view of the same unit.
  UC_CHECK_LINE(run, "basis=recovered");
  UC_CHECK_LINE(run, "transfer_ready=true");
  UC_CHECK_LINE(run, "static_bypass_ready=false");
  UC_CHECK_LINE(run,
                "blocker=state_not_revalidated the store has not been revalidated at this instant");

  const CliRun json = store.run({"status", "--now", "150", "--ups", kUps, "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"operating\":\"online_normal\"");
  UC_CHECK_TEXT(json, "\"state_basis\":\"recovered\"");
  UC_CHECK_TEXT(json, "\"state_revision\":3");
  UC_CHECK_TEXT(json, "\"revalidated\":false");
  UC_CHECK_TEXT(json, "\"in_flight\":false");
}

UC_TEST(cli, status_without_ups_lists_the_units) {
  ReadyStore store("cli-status-list");
  store.prepare();
  const CliRun run = store.run({"status", "--now", "150"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "store=" + store.path.string());
  UC_CHECK_LINE(run, "generation=7");
  UC_CHECK_LINE(run, "units=1");
  UC_CHECK_TEXT(run, "unit=ups-under-test hardware_generation=1 lifecycle=in_service "
                     "operating=online_normal basis=");
  UC_CHECK_TEXT(run, " revision=3 in_flight=false");

  const CliRun json = store.run({"status", "--now", "150", "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"units\":1");
  UC_CHECK_TEXT(json, "\"unit_list\":[{\"ups\":\"ups-under-test\"");
  UC_CHECK_TEXT(json, "\"state_revision\":3,\"in_flight\":false");
}

UC_TEST(cli, battery_reports_the_evidence_and_its_assessment) {
  ReadyStore store("cli-battery");
  store.prepare();
  const CliRun run = store.run({"battery", "--now", "150", "--ups", kUps});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "ups=ups-under-test");
  UC_CHECK_LINE(run, "evidence=ev-1");
  UC_CHECK_LINE(run, "source=synthetic-source");
  UC_CHECK_LINE(run, "provenance=synthetic_adapter");
  UC_CHECK_LINE(run, "observed_at=120");
  UC_CHECK_LINE(run, "observation_age=30");
  UC_CHECK_LINE(run, "observation_freshness=fresh");
  UC_CHECK_LINE(run, "reserve=900 s");
  UC_CHECK_LINE(run, "reserve_quality=measured");
  UC_CHECK_LINE(run, "state_of_charge=unknown");
  UC_CHECK_LINE(run, "activity=idle");
  // Fresh evidence is still not current authority in a session that has only
  // recovered the store, so the assessment is indeterminate, never sufficient.
  UC_CHECK_LINE(run, "reserve_outcome=indeterminate");
  UC_CHECK_LINE(run, "reserve_reason=not_revalidated");

  const CliRun json = store.run({"battery", "--now", "150", "--ups", kUps, "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"reserve_state\":\"known\"");
  UC_CHECK_TEXT(json, "\"reserve_quality\":\"measured\"");
  UC_CHECK_TEXT(json, "\"observation_age\":30");
  UC_CHECK_TEXT(json, "\"evidence\":\"ev-1\"");
}

UC_TEST(cli, readiness_reports_only_what_is_established) {
  ReadyStore store("cli-readiness");
  store.prepare();
  const CliRun run = store.run({"readiness", "--now", "150", "--ups", kUps});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "unit=ups-under-test");
  UC_CHECK_LINE(run, "lifecycle=in_service");
  UC_CHECK_LINE(run, "operating=online_normal");
  UC_CHECK_LINE(run, "basis=recovered");
  UC_CHECK_LINE(run, "revalidated=false");
  UC_CHECK_LINE(run, "transfer_ready=true");
  UC_CHECK_LINE(run, "static_bypass_ready=false");
  UC_CHECK_LINE(run, "maintenance_bypass_ready=false");
  UC_CHECK_LINE(run, "test_ready=false");
  UC_CHECK_LINE(run, "recharge_authorized=false");
  UC_CHECK_LINE(run,
                "blocker=state_not_revalidated the store has not been revalidated at this instant");

  const CliRun json = store.run({"readiness", "--now", "150", "--ups", kUps, "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"static_bypass_ready\":false");
  UC_CHECK_TEXT(json, "\"transfer_ready\":true");
  UC_CHECK_TEXT(
      json,
      "\"blockers\":[\"state_not_revalidated: the store has not been revalidated at this instant\"]");
}

UC_TEST(cli, obligations_lists_the_bound_obligation_exactly) {
  ReadyStore store("cli-obligations");
  store.prepare();
  const CliRun bound = store.bind_life_safety_obligation("140");
  UC_CHECK_EXIT(bound, 0);
  UC_CHECK_LINE(bound, "obligation=obl-1");
  UC_CHECK_LINE(bound, "load=load-1");
  UC_CHECK_LINE(bound, "revision=1");
  UC_CHECK_LINE(bound, "state=active");
  UC_CHECK_LINE(bound, "binds=true");

  const CliRun run = store.run({"obligations", "--now", "150", "--ups", kUps});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "ups=ups-under-test");
  UC_CHECK_LINE(run, "count=1");
  UC_CHECK_LINE(run,
                "obligation=obl-1 load=load-1 tier=life_safety protection=must_remain_protected "
                "state=active revision=1 asserted_by=fire-marshal asserted_at=140 binds=true "
                "lapsed=false");

  const CliRun json = store.run({"obligations", "--now", "150", "--ups", kUps, "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"tier\":\"life_safety\"");
  UC_CHECK_TEXT(json, "\"binds\":true");
}

UC_TEST(cli, store_audit_reports_the_durable_identity_and_accounting) {
  ReadyStore store("cli-store-audit");
  store.prepare();
  UC_CHECK_EXIT(store.bind_life_safety_obligation("140"), 0);
  const CliRun run = store.run({"store-audit"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "store=" + store.path.string());
  UC_CHECK_LINE(run, "recorded_path=" + store.path.string());
  UC_CHECK_LINE(run, "path_binding_matches=true");
  UC_CHECK_LINE(run, "generation=9");
  UC_CHECK_LINE(run, "epoch=1");
  UC_CHECK_LINE(run, "incarnation=1");
  UC_CHECK_LINE(run, "created_at=100");
  UC_CHECK_LINE(run, "updated_at=140");
  UC_CHECK_LINE(run, "revalidated_at=140");
  UC_CHECK_LINE(run, "format_version=1");
  UC_CHECK_LINE(run, "head_bytes=128");
  UC_CHECK_LINE(run, "units=1");
  UC_CHECK_LINE(run, "attempts=0");
  UC_CHECK_LINE(run, "unresolved_attempts=0");
  UC_CHECK_LINE(run, "terminal_attempts=0");
  UC_CHECK_LINE(run, "idempotency_bindings=0");
  UC_CHECK_LINE(run, "operation_count=8");
  UC_CHECK_LINE(run, "writable=false");
  UC_CHECK_LINE(run, "commit_log_entries=9");
  UC_CHECK_EQ(run.value_of("store_identity").size(), static_cast<std::size_t>(32));

  const CliRun json = store.run({"store-audit", "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"head_bytes\":128");
  UC_CHECK_TEXT(json, "\"format_version\":1");
  UC_CHECK_TEXT(json, "\"writable\":false");
  UC_CHECK_TEXT(json, "\"recorded_path\":\"" + json_escape_ascii(store.path.string()) + "\"");
}

UC_TEST(cli, verify_store_verifies_the_artifact_and_agrees_with_the_audit) {
  ReadyStore store("cli-verify-store");
  store.prepare();
  UC_CHECK_EXIT(store.bind_life_safety_obligation("140"), 0);
  const CliRun run = store.run({"verify-store"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "store=" + store.path.string());
  UC_CHECK_LINE(run, "verified=true");
  UC_CHECK_LINE(run, "generation=9");
  UC_CHECK_LINE(run, "epoch=1");
  UC_CHECK_LINE(run, "incarnation=1");
  UC_CHECK_LINE(run, "units=1");
  UC_CHECK_LINE(run, "attempts=0");

  const CliRun audit = store.run({"store-audit", "--json"});
  UC_CHECK_EXIT(audit, 0);
  const std::string from_audit =
      json_value_after(audit.line_starting_with("{"), "\"canonical_digest\":");
  UC_REQUIRE(!from_audit.empty());
  UC_CHECK_EQ(run.value_of("canonical_digest"), from_audit);
}

UC_TEST(cli, history_reports_every_durable_commit_newest_first) {
  ReadyStore store("cli-history");
  store.prepare();
  UC_CHECK_EXIT(store.bind_life_safety_obligation("140"), 0);
  const CliRun run = store.run({"history"});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "store=" + store.path.string());
  UC_CHECK_LINE(run, "generation=9");
  UC_CHECK_LINE(run, "operation_count=8");
  UC_CHECK_LINE(run, "commit generation=9 operation=obligation_bound ups=ups-under-test attempt=0 revision=4 at=140");
  UC_CHECK_LINE(run, "commit generation=8 operation=revalidated ups= attempt=0 revision=0 at=140");
  UC_CHECK_LINE(run, "commit generation=7 operation=grant_issued ups=ups-under-test attempt=0 revision=3 at=130");
  UC_CHECK_LINE(run, "commit generation=6 operation=revalidated ups= attempt=0 revision=0 at=130");
  UC_CHECK_LINE(run, "commit generation=5 operation=telemetry_recorded ups=ups-under-test attempt=0 revision=2 at=120");
  UC_CHECK_LINE(run, "commit generation=3 operation=ups_registered ups=ups-under-test attempt=0 revision=1 at=110");
  UC_CHECK_LINE(run, "commit generation=1 operation=store_created ups= attempt=0 revision=0 at=100");

  const CliRun limited = store.run({"history", "--limit", "2"});
  UC_CHECK_EXIT(limited, 0);
  UC_CHECK_LINE(limited, "commit generation=9 operation=obligation_bound ups=ups-under-test attempt=0 revision=4 at=140");
  UC_CHECK_LINE(limited, "commit generation=8 operation=revalidated ups= attempt=0 revision=0 at=140");
  UC_CHECK(!limited.has_text("commit generation=7"));
}

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

UC_TEST(cli, json_output_of_every_command_is_structurally_sound) {
  ReadyStore store("cli-json");
  store.prepare();

  const std::vector<std::pair<std::string, CliRun>> runs = {
      {"init", run_cli(store.scratch, with_store(store.path, {"init", "--now", "500", "--epoch", "1",
                                                              "--incarnation", "1", "--json"}))},
      {"register", store.run({"register", "--now", "141", "--ups", "second-unit", "--hardware", "2",
                              "--json"})},
      {"status", store.run({"status", "--now", "142", "--ups", kUps, "--json"})},
      {"battery", store.run({"battery", "--now", "142", "--ups", kUps, "--json"})},
      {"readiness", store.run({"readiness", "--now", "142", "--ups", kUps, "--json"})},
      {"obligations", store.run({"obligations", "--now", "142", "--ups", kUps, "--json"})},
      {"store-audit", store.run({"store-audit", "--json"})},
      {"revalidate", store.run({"revalidate", "--now", "143", "--json"})},
      {"run", store.run({"run", "--now", "144", "--ups", kUps, "--command",
                            "enter_static_bypass", "--authority", kGrant, "--key", "json-key",
                            "--json"})},
  };
  for (const std::pair<std::string, CliRun>& entry : runs) {
    UC_CHECK_MSG(entry.second.exit_code == 0, entry.first + ": " + exit_detail(entry.second, 0));
    UC_CHECK_MSG(entry.second.line_count() == 1,
                 entry.first + ": a JSON command emits exactly one line, got:\n" + entry.second.text);
    UC_CHECK_MSG(json_is_structurally_sound(entry.second.text),
                 entry.first + ": structurally unsound JSON: " + entry.second.text);
  }

  // Named fields a correct emitter must produce. The store path carries
  // backslashes and spaces, so the escaped path proves the emitter escapes rather
  // than copying bytes through.
  const CliRun status = store.run({"status", "--now", "145", "--ups", kUps, "--json"});
  UC_CHECK_TEXT(status, "\"operating\":\"online_normal\"");
  UC_CHECK_TEXT(status, "\"state_basis\":\"recovered\"");
  UC_CHECK_TEXT(status, "\"in_flight\":true");
  UC_CHECK_TEXT(status, "\"in_flight_attempt\":1");
  const CliRun audit = store.run({"store-audit", "--json"});
  UC_CHECK_TEXT(audit, "\"store\":\"" + json_escape_ascii(store.path.string()) + "\"");
  UC_CHECK_TEXT(audit, "\"attempts\":1");
  const CliRun revalidate = store.run({"revalidate", "--now", "146", "--json"});
  UC_CHECK_TEXT(revalidate, "\"revalidated_at\":146");
  UC_CHECK_TEXT(revalidate, "\"units\":2");
  UC_CHECK_TEXT(revalidate, "\"units_with_fresh_reserve\":2");
  const CliRun run_json = store.run({"run", "--now", "147", "--ups", kUps, "--command",
                                       "enter_static_bypass", "--authority", kGrant, "--key",
                                       "json-key-2", "--json"});
  UC_CHECK_EXIT(run_json, 2);
  UC_CHECK_EQ(run_json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_TEXT(run_json, "\"phase\":\"refused\"");
  UC_CHECK_TEXT(run_json, "\"refusal\":{\"code\":\"transition_in_progress\"");
  const CliRun attempts = store.run({"attempts", "--json"});
  UC_CHECK_EXIT(attempts, 0);
  UC_CHECK_EQ(attempts.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(attempts.text), "unsound JSON: " + attempts.text);
  UC_CHECK_TEXT(attempts, "\"attempts\":[{\"attempt\":2");
}

UC_TEST(cli, json_escapes_a_store_path_and_never_emits_a_raw_control_byte) {
  ScratchDirectory scratch("cli-json-escape");
  const std::filesystem::path store = scratch.file("unit.upsstore");
  const CliRun init = run_cli(scratch, {"init", "--store", store.string(), "--now", "100", "--epoch",
                                        "1", "--incarnation", "1", "--json"});
  UC_CHECK_EXIT(init, 0);
  UC_CHECK_EQ(init.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(init.text), "unsound JSON: " + init.text);
  // Every backslash of the recorded path is escaped, and the value is closed by
  // the escaping rather than by the raw bytes of the path.
  UC_CHECK_TEXT(init, "\"store\":\"" + json_escape_ascii(store.string()) + "\"");
  UC_CHECK_TEXT(init, "\"store_identity\":\"");
  UC_CHECK_TEXT(init, "\"generation\":1");
  UC_CHECK_MSG(!init.has_text("\\\\\":\""), "a raw path byte leaked into the JSON: " + init.text);
}

// ---------------------------------------------------------------------------
// The control path
// ---------------------------------------------------------------------------

UC_TEST(cli, evaluate_is_allowed_when_the_request_is_authorized) {
  ReadyStore store("cli-evaluate-allowed");
  store.prepare();
  const CliRun run = store.run({"evaluate", "--now", "140", "--ups", kUps, "--command",
                                "enter_static_bypass", "--authority", kGrant});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "unit=ups-under-test");
  UC_CHECK_LINE(run, "command=enter_static_bypass");
  UC_CHECK_LINE(run, "planned_revision=3");
  UC_CHECK_LINE(run, "current_revision=3");
  UC_CHECK_LINE(run, "current_state=online_normal");
  UC_CHECK_LINE(run, "target_state=static_bypass");
  UC_CHECK_LINE(run, "verdict=allowed");
  UC_CHECK_LINE(run, "primary=none");
  UC_CHECK_LINE(run, "primary_detail=every precondition is established");
  UC_CHECK_LINE(run, "protection=preserves");
  UC_CHECK_LINE(run, "authority=allowed");
  UC_CHECK_LINE(run, "reserve=sufficient");

  const CliRun json = store.run({"evaluate", "--now", "141", "--ups", kUps, "--command",
                                 "enter_static_bypass", "--authority", kGrant, "--json"});
  UC_CHECK_EXIT(json, 0);
  UC_CHECK_EQ(json.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(json.text), "unsound JSON: " + json.text);
  UC_CHECK_TEXT(json, "\"allowed\":true");
  UC_CHECK_TEXT(json, "\"primary_refusal\":\"none\"");
  UC_CHECK_TEXT(json, "\"authority\":\"allowed\"");
}

UC_TEST(cli, evaluate_without_a_cited_grant_is_refused_with_authority_missing) {
  ReadyStore store("cli-no-grant");
  store.prepare();
  const CliRun run =
      store.run({"evaluate", "--now", "140", "--ups", kUps, "--command", "enter_static_bypass"});
  UC_CHECK_EXIT(run, 2);
  UC_CHECK_LINE(run, "verdict=refused");
  UC_CHECK_LINE(run, "primary=authority_missing");
  UC_CHECK_LINE(run, "authority=missing");
  UC_CHECK_LINE(run,
                "primary_detail=no authority was cited for the bypass_transfer scope, so permission "
                "cannot be established");
  UC_CHECK_TEXT(run,
                "finding=authority_missing no authority was cited for the bypass_transfer scope, so "
                "permission cannot be established");
}

UC_TEST(cli, evaluate_with_no_adapter_is_refused_with_adapter_unavailable) {
  ReadyStore store("cli-no-adapter");
  store.prepare();
  const CliRun run = store.run({"evaluate", "--now", "140", "--ups", kUps, "--command",
                                "enter_static_bypass", "--authority", kGrant, "--no-adapter"});
  UC_CHECK_EXIT(run, 2);
  UC_CHECK_LINE(run, "verdict=refused");
  UC_CHECK_LINE(run, "primary=adapter_unavailable");
  UC_CHECK_LINE(run,
                "primary_detail=no adapter is bound, so no command can be issued for unit "
                "'ups-under-test'");
  // The grant itself is still fine: only the adapter is missing.
  UC_CHECK_LINE(run, "authority=allowed");
}

UC_TEST(cli, entering_maintenance_bypass_with_a_bound_life_safety_obligation_is_refused) {
  ReadyStore store("cli-obligation-refusal");
  store.prepare();
  UC_CHECK_EXIT(store.grant(kMaintenanceGrant, "maintenance_entry", "140"), 0);
  UC_CHECK_EXIT(store.bind_life_safety_obligation("141"), 0);

  const CliRun run = store.run({"evaluate", "--now", "142", "--ups", kUps, "--command",
                                "enter_maintenance_bypass", "--authority", kMaintenanceGrant});
  UC_CHECK_EXIT(run, 2);
  UC_CHECK_LINE(run, "verdict=refused");
  UC_CHECK_LINE(run, "primary=obligation_unreleased");
  UC_CHECK_LINE(run, "protection=drops_undeclared");
  UC_CHECK_LINE(run, "authority=allowed");
  UC_CHECK_LINE(run,
                "primary_detail=enter_maintenance_bypass removes UPS protection from protected loads "
                "whose obligations are still binding: obl-1");

  // Releasing the obligation removes the refusal, which proves the binding is what
  // refused the transition.
  const CliRun released = store.run({"release-obligation", "--now", "143", "--ups", kUps, "--ref",
                                     "obl-1", "--revision", "1", "--authority", "fire-marshal"});
  UC_CHECK_EXIT(released, 0);
  UC_CHECK_LINE(released, "state=released");
  const CliRun after = store.run({"evaluate", "--now", "144", "--ups", kUps, "--command",
                                  "enter_maintenance_bypass", "--authority", kMaintenanceGrant});
  UC_CHECK_EXIT(after, 0);
  UC_CHECK_LINE(after, "verdict=allowed");
  UC_CHECK(!after.has_text("obligation_unreleased"));
}

UC_TEST(cli, a_reserve_dependent_command_is_refused_when_the_evidence_is_stale) {
  ReadyStore store("cli-stale-reserve");
  store.prepare();
  UC_CHECK_EXIT(store.grant(kTestGrant, "test_execution", "140"), 0);

  // The observation is 10 ticks old and the policy window is 600 ticks.
  const CliRun fresh = store.run({"evaluate", "--now", "150", "--ups", kUps, "--command",
                                  "start_battery_test", "--authority", kTestGrant});
  UC_CHECK_EXIT(fresh, 0);
  UC_CHECK_LINE(fresh, "verdict=allowed");
  UC_CHECK_LINE(fresh, "reserve=sufficient");

  // Well past the window the same request is refused for the evidence itself.
  const CliRun stale = store.run({"evaluate", "--now", "1000", "--ups", kUps, "--command",
                                  "start_battery_test", "--authority", kTestGrant});
  UC_CHECK_EXIT(stale, 2);
  UC_CHECK_LINE(stale, "verdict=refused");
  UC_CHECK_LINE(stale, "primary=reserve_evidence_stale");
  UC_CHECK_LINE(stale, "reserve=indeterminate");
  UC_CHECK_LINE(stale,
                "primary_detail=the reserve evidence was observed at 120 and is older than the "
                "accepted window of 600 ticks");
}

UC_TEST(cli, a_request_citing_a_superseded_epoch_is_refused_with_stale_epoch) {
  ReadyStore store("cli-stale-epoch");
  store.prepare();
  const CliRun adopted =
      store.run({"adopt-authority", "--now", "140", "--epoch", "2", "--incarnation", "1"});
  UC_CHECK_EXIT(adopted, 0);
  UC_CHECK_LINE(adopted, "epoch=2");
  UC_CHECK_LINE(adopted, "incarnation=1");

  const CliRun run = store.run({"run", "--now", "141", "--ups", kUps, "--command",
                                "enter_static_bypass", "--authority", kGrant, "--key", "epoch-key",
                                "--epoch", "1"});
  UC_CHECK_EXIT(run, 2);
  UC_CHECK_LINE(run, "attempt=1");
  UC_CHECK_LINE(run, "phase=refused");
  UC_CHECK_LINE(run, "ack=none");
  UC_CHECK_LINE(run, "refusal=stale_epoch");
  UC_CHECK_LINE(run, "refusal_detail=the request cites control epoch 1 but the store is at epoch 2");
}

UC_TEST(cli, a_request_citing_a_superseded_hardware_or_revision_is_fenced) {
  ReadyStore store("cli-stale-hardware");
  store.prepare();
  const CliRun hardware = store.run({"run", "--now", "140", "--ups", kUps, "--command",
                                     "enter_static_bypass", "--authority", kGrant, "--key", "hw-key",
                                     "--hardware", "7"});
  UC_CHECK_EXIT(hardware, 2);
  UC_CHECK_LINE(hardware, "phase=refused");
  UC_CHECK_LINE(hardware, "refusal=stale_hardware_generation");
  UC_CHECK_LINE(hardware,
                "refusal_detail=the request was planned against hardware generation 7 but unit "
                "'ups-under-test' is at hardware generation 1; the request is fenced");

  const CliRun revision = store.run({"run", "--now", "141", "--ups", kUps, "--command",
                                     "enter_static_bypass", "--authority", kGrant, "--key", "rev-key",
                                     "--revision", "1"});
  UC_CHECK_EXIT(revision, 2);
  UC_CHECK_LINE(revision, "phase=refused");
  UC_CHECK_LINE(revision, "refusal=stale_revision");
  UC_CHECK_LINE(revision,
                "refusal_detail=the request was planned against state revision 1 but unit "
                "'ups-under-test' is at revision 3");
}

UC_TEST(cli, a_recovered_store_refuses_control_when_revalidation_is_suppressed) {
  ReadyStore store("cli-no-revalidate");
  store.prepare();
  const CliRun run = store.run({"run", "--now", "140", "--ups", kUps, "--command",
                                "enter_static_bypass", "--authority", kGrant, "--key", "nr-key",
                                "--no-revalidate"});
  UC_CHECK_EXIT(run, 2);
  UC_CHECK_LINE(run, "attempt=1");
  UC_CHECK_LINE(run, "phase=refused");
  // Nothing beyond the plan is claimed: the command never reached the adapter.
  UC_CHECK_LINE(run, "ack=none");
  UC_CHECK_LINE(run, "observed=none");
  UC_CHECK_LINE(run, "verified=not_evaluated");
  UC_CHECK_LINE(run, "refusal=state_not_revalidated");
  UC_CHECK_LINE(run,
                "refusal_detail=the store has been recovered and not revalidated at instant 140, so "
                "no control decision may be taken");
}

UC_TEST(cli, an_unsupported_command_is_refused_before_the_adapter_is_reached) {
  ReadyStore store("cli-unsupported");
  store.prepare();
  const std::filesystem::path effects = store.scratch.file("effects.log");
  const CliRun run = store.run({"run", "--now", "140", "--ups", kUps, "--command", "start_self_test",
                                "--unsupported-cmd", "start_self_test", "--authority", kGrant,
                                "--key", "unsup-1", "--effects-log", effects.string()});
  UC_CHECK_EXIT(run, 2);
  UC_CHECK_LINE(run, "command=start_self_test");
  UC_CHECK_LINE(run, "target_state=self_test");
  UC_CHECK_LINE(run, "phase=refused");
  UC_CHECK_LINE(run, "refusal=capability_unsupported");
  UC_CHECK_LINE(run,
                "refusal_detail=adapter 'synthetic' does not implement start_self_test; the "
                "operation is reported unsupported rather than substituted");
  // The model refuses the request at evaluation, so it is never handed to the
  // adapter: no effects log is created, and the model never reports the
  // AttemptPhase::Unsupported stage that an adapter-side refusal would produce.
  UC_CHECK(!std::filesystem::exists(effects));

  const CliRun attempts = store.run({"attempts"});
  UC_CHECK_EXIT(attempts, 0);
  UC_CHECK_LINE(attempts, "count=1");
  UC_CHECK_LINE(attempts,
                "attempt=1 ups=ups-under-test command=start_self_test phase=refused ack=none "
                "observed=none verified=not_evaluated evidence_class=SYNTHETIC "
                "refusal=capability_unsupported");
}

UC_TEST(cli, the_control_lifecycle_reaches_a_verified_effect) {
  // The documented administration path, run as separate process invocations:
  // init -> register -> observe -> grant -> evaluate -> run -> observe -> verify
  // -> status -> revalidate -> store-audit -> verify-store.
  ReadyStore store("cli-lifecycle");
  store.prepare();
  const std::filesystem::path effects = store.scratch.file("effects.log");

  const CliRun evaluate = store.run({"evaluate", "--now", "140", "--ups", kUps, "--command",
                                     "enter_static_bypass", "--authority", kGrant});
  UC_CHECK_EXIT(evaluate, 0);
  UC_CHECK_LINE(evaluate, "verdict=allowed");

  const CliRun run = store.run({"run", "--now", "141", "--ups", kUps, "--command",
                                "enter_static_bypass", "--authority", kGrant, "--key", "life-1",
                                "--effects-log", effects.string()});
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "attempt=1");
  UC_CHECK_LINE(run, "command=enter_static_bypass");
  UC_CHECK_LINE(run, "target_state=static_bypass");
  UC_CHECK_LINE(run, "phase=acknowledged");
  UC_CHECK_LINE(run, "ack=accepted");
  // An acknowledgement is not an effect and this runtime never claims one.
  UC_CHECK_LINE(run, "observed=none");
  UC_CHECK_LINE(run, "verified=not_evaluated");
  UC_CHECK_LINE(run, "evidence_class=SYNTHETIC");
  UC_CHECK_LINE(run, "planned_revision=3");

  // The command really did reach the adapter boundary.
  UC_REQUIRE(std::filesystem::exists(effects));
  const std::vector<std::string> issued = read_lines(effects);
  UC_REQUIRE(issued.size() == 1);
  UC_CHECK_EQ(issued.front(),
              std::string("issue attempt=1 ups=ups-under-test hardware=1 "
                          "command=enter_static_bypass at=141"));

  // Nothing has moved: the unit still reports its previous operating state and the
  // unresolved attempt holds the single-device ordering gate.
  const CliRun before = store.run({"status", "--now", "142", "--ups", kUps});
  UC_CHECK_EXIT(before, 0);
  UC_CHECK_LINE(before, "operating=online_normal");
  UC_CHECK_LINE(before, "state_revision=3");
  UC_CHECK_LINE(before, "in_flight_attempt=1");
  UC_CHECK_LINE(before, "blocker=transition_in_progress attempt 1 is unresolved");

  const CliRun observed =
      store.run({"observe", "--now", "143", "--ups", kUps, "--operating", "static_bypass",
                 "--reserve-unit", "s", "--reserve-value", "880", "--reserve-quality", "measured",
                 "--bypass-kind", "static", "--bypass-available", "true", "--bypass-qualified",
                 "true", "--synchronized", "true", "--transfer-ready", "true", "--battery-ready",
                 "true", "--activity", "idle", "--evidence", "ev-2"});
  UC_CHECK_EXIT(observed, 0);
  UC_CHECK_LINE(observed, "evidence=ev-2");
  UC_CHECK_LINE(observed, "operating=static_bypass");
  UC_CHECK_LINE(observed, "contradictory=false");

  const CliRun verify = store.run({"verify", "--now", "144", "--attempt", "1"});
  UC_CHECK_EXIT(verify, 0);
  UC_CHECK_LINE(verify, "attempt=1");
  UC_CHECK_LINE(verify, "phase=verified");
  UC_CHECK_LINE(verify, "ack=accepted");
  UC_CHECK_LINE(verify, "observed=matches_target");
  UC_CHECK_LINE(verify, "verified=verified");
  UC_CHECK_LINE(verify, "evidence_class=SYNTHETIC");
  UC_CHECK_LINE(verify,
                "detail=the observed operating state static_bypass is an accepted effect of "
                "enter_static_bypass");

  // Only the verification moves the authoritative operating state.
  const CliRun after = store.run({"status", "--now", "145", "--ups", kUps});
  UC_CHECK_EXIT(after, 0);
  UC_CHECK_LINE(after, "operating=static_bypass");
  UC_CHECK_LINE(after, "state_revision=4");
  UC_CHECK_LINE(after, "state_since=143");
  // A later session preserves no freshness claim, so the durable effect is
  // reported with a recovered basis rather than a verified one.
  UC_CHECK_LINE(after, "state_basis=recovered");
  UC_CHECK_LINE(after, "revalidated=false");

  const CliRun attempts = store.run({"attempts"});
  UC_CHECK_EXIT(attempts, 0);
  UC_CHECK_LINE(attempts, "count=1");
  UC_CHECK_LINE(attempts,
                "attempt=1 ups=ups-under-test command=enter_static_bypass phase=verified "
                "ack=accepted observed=matches_target verified=verified evidence_class=SYNTHETIC");

  const CliRun revalidate = store.run({"revalidate", "--now", "146"});
  UC_CHECK_EXIT(revalidate, 0);
  UC_CHECK_LINE(revalidate, "revalidated_at=146");
  UC_CHECK_LINE(revalidate, "units=1");
  UC_CHECK_LINE(revalidate, "units_with_fresh_reserve=1");
  UC_CHECK_LINE(revalidate, "units_with_stale_reserve=0");
  UC_CHECK_LINE(revalidate, "unresolved_attempts=0");
  UC_CHECK_TEXT(revalidate, "unit=ups-under-test reserve=sufficient freshness=fresh");

  const CliRun audit = store.run({"store-audit"});
  UC_CHECK_EXIT(audit, 0);
  UC_CHECK_LINE(audit, "revalidated_at=146");
  UC_CHECK_LINE(audit, "units=1");
  UC_CHECK_LINE(audit, "attempts=1");
  UC_CHECK_LINE(audit, "unresolved_attempts=0");
  UC_CHECK_LINE(audit, "terminal_attempts=1");
  UC_CHECK_LINE(audit, "idempotency_bindings=1");

  const CliRun verified = store.run({"verify-store"});
  UC_CHECK_EXIT(verified, 0);
  UC_CHECK_LINE(verified, "verified=true");
  UC_CHECK_LINE(verified, "units=1");
  UC_CHECK_LINE(verified, "attempts=1");
}

UC_TEST(cli, revalidation_is_durable_but_only_the_process_that_performed_it_may_act) {
  ReadyStore store("cli-revalidation");
  store.prepare();
  const CliRun before = store.run({"store-audit"});
  UC_CHECK_EXIT(before, 0);
  std::int64_t generation_before = 0;
  UC_REQUIRE(parse_decimal(before.value_of("generation"), generation_before));

  const CliRun revalidate = store.run({"revalidate", "--now", "140"});
  UC_CHECK_EXIT(revalidate, 0);
  UC_CHECK_LINE(revalidate, "revalidated_at=140");
  // Exactly one durable commit: the command performs the revalidation itself and
  // does not also perform the automatic one that a mutating command performs.
  std::int64_t generation_after = 0;
  UC_REQUIRE(parse_decimal(revalidate.value_of("generation"), generation_after));
  UC_CHECK_EQ(generation_after, generation_before + 1);

  // The durable record survives; the next process is recovered again and must
  // revalidate before it may act.
  const CliRun audit = store.run({"store-audit"});
  UC_CHECK_EXIT(audit, 0);
  UC_CHECK_LINE(audit, "revalidated_at=140");
  const CliRun status = store.run({"status", "--now", "141", "--ups", kUps});
  UC_CHECK_EXIT(status, 0);
  UC_CHECK_LINE(status, "revalidated=false");
  const CliRun refused = store.run({"run", "--now", "142", "--ups", kUps, "--command",
                                    "enter_static_bypass", "--authority", kGrant, "--key",
                                    "recovered-key", "--no-revalidate"});
  UC_CHECK_EXIT(refused, 2);
  UC_CHECK_LINE(refused, "refusal=state_not_revalidated");
  const CliRun allowed = store.enter_static_bypass("revalidated-key", "143");
  UC_CHECK_EXIT(allowed, 0);
  UC_CHECK_LINE(allowed, "phase=acknowledged");
}

UC_TEST(cli, an_acknowledged_attempt_blocks_a_second_command_until_it_is_resolved) {
  ReadyStore store("cli-ordering-gate");
  store.prepare();
  const CliRun first = store.enter_static_bypass("first-key", "140");
  UC_CHECK_EXIT(first, 0);
  UC_CHECK_LINE(first, "phase=acknowledged");

  const CliRun second = store.enter_static_bypass("second-key", "141");
  UC_CHECK_EXIT(second, 2);
  UC_CHECK_LINE(second, "attempt=2");
  UC_CHECK_LINE(second, "phase=refused");
  UC_CHECK_LINE(second, "refusal=transition_in_progress");
  UC_CHECK_LINE(second,
                "refusal_detail=unit 'ups-under-test' already has unresolved attempt 1; a second "
                "command is refused so that device transitions stay ordered");

  const CliRun abandon =
      store.run({"abandon", "--now", "142", "--attempt", "1", "--reason", "abandoned"});
  UC_CHECK_EXIT(abandon, 2);
  UC_CHECK_LINE(abandon, "attempt=1");
  UC_CHECK_LINE(abandon, "phase=failed");
  UC_CHECK_LINE(abandon, "detail=abandoned");

  // The abandoned attempt leaves the operating state untrustworthy, so the next
  // command is refused until fresh telemetry restores it.
  const CliRun third = store.enter_static_bypass("third-key", "143");
  UC_CHECK_EXIT(third, 2);
  UC_CHECK_LINE(third, "attempt=3");
  UC_CHECK_LINE(third, "refusal=state_basis_unverified");
  UC_CHECK_LINE(third,
                "refusal_detail=the operating state of unit 'ups-under-test' rests on an "
                "acknowledged command whose effect was never verified; record telemetry or abandon "
                "the attempt before commanding again");
}

UC_TEST(cli, an_issued_attempt_is_resolved_by_acknowledgement_and_a_fresh_observation) {
  ReadyStore store("cli-issued");
  store.prepare();
  const std::filesystem::path effects = store.scratch.file("effects.log");
  const CliRun issued = store.run({"run", "--now", "140", "--ups", kUps, "--command",
                                   "enter_static_bypass", "--authority", kGrant, "--key", "live-1",
                                   "--ack", "no-response", "--effects-log", effects.string()});
  UC_CHECK_EXIT(issued, 0);
  UC_CHECK_LINE(issued, "attempt=1");
  UC_CHECK_LINE(issued, "phase=issued");
  UC_CHECK_LINE(issued, "ack=no_response");
  UC_CHECK_LINE(issued, "verified=not_evaluated");
  UC_CHECK_LINE(issued,
                "detail=the adapter returned no response; the attempt stays unresolved because the "
                "device may have acted, and it is never reissued");
  UC_CHECK(std::filesystem::exists(effects));

  // An issued attempt is not yet verifiable at all: the device may have acted and
  // nothing has confirmed that it did.
  const CliRun early = store.run({"verify", "--now", "141", "--attempt", "1"});
  UC_CHECK_EXIT(early, 1);
  UC_CHECK_LINE(early,
                "error: attempt_state_conflict: attempt 1 is in phase issued and cannot be verified "
                "yet; record telemetry or abandon it");

  const CliRun acknowledged =
      store.run({"ack", "--now", "142", "--attempt", "1", "--outcome", "accepted"});
  UC_CHECK_EXIT(acknowledged, 0);
  UC_CHECK_LINE(acknowledged, "phase=acknowledged");
  UC_CHECK_LINE(acknowledged, "ack=accepted");

  // Once acknowledged, the recorded observation still predates the
  // acknowledgement, so it cannot establish the effect.
  const CliRun stale = store.run({"verify", "--now", "143", "--attempt", "1"});
  UC_CHECK_EXIT(stale, 2);
  UC_CHECK_LINE(stale, "phase=observed");
  UC_CHECK_LINE(stale, "verified=indeterminate");
  UC_CHECK_LINE(stale, "verification_detail=the observation is not newer than the acknowledgement");

  UC_CHECK_EXIT(store.run({"observe", "--now", "144", "--ups", kUps, "--operating", "static_bypass",
                           "--reserve-unit", "s", "--reserve-value", "880", "--reserve-quality",
                           "measured", "--bypass-kind", "static", "--bypass-available", "true",
                           "--bypass-qualified", "true", "--synchronized", "true",
                           "--transfer-ready", "true", "--battery-ready", "true", "--activity",
                           "idle", "--evidence", "ev-2"}),
                0);

  const CliRun verified = store.run({"verify", "--now", "145", "--attempt", "1"});
  UC_CHECK_EXIT(verified, 0);
  UC_CHECK_LINE(verified, "phase=verified");
  UC_CHECK_LINE(verified, "observed=matches_target");
  UC_CHECK_LINE(verified, "verified=verified");
}

UC_TEST(cli, the_same_key_in_a_second_process_returns_the_same_attempt) {
  ReadyStore store("cli-idempotent");
  store.prepare();
  const CliRun first = store.enter_static_bypass("idem-1", "140");
  UC_CHECK_EXIT(first, 0);
  UC_CHECK_LINE(first, "attempt=1");
  UC_CHECK_LINE(first, "phase=acknowledged");

  // A retry from a different process, much later, with the same key. The stored
  // result is returned, including the instant it was submitted at.
  const CliRun second = store.run({"run", "--now", "900", "--ups", kUps, "--command",
                                   "enter_static_bypass", "--authority", kGrant, "--key", "idem-1",
                                   "--json"});
  UC_CHECK_EXIT(second, 0);
  UC_CHECK_EQ(second.line_count(), static_cast<std::size_t>(1));
  UC_CHECK_MSG(json_is_structurally_sound(second.text), "unsound JSON: " + second.text);
  UC_CHECK_TEXT(second, "\"attempt\":1");
  UC_CHECK_TEXT(second, "\"phase\":\"acknowledged\"");
  UC_CHECK_TEXT(second, "\"submitted_at\":140");
  UC_CHECK_TEXT(second, "\"acknowledged_at\":140");

  const CliRun attempts = store.run({"attempts", "--limit", "5"});
  UC_CHECK_EXIT(attempts, 0);
  UC_CHECK_LINE(attempts, "count=1");
  UC_CHECK_LINE(attempts,
                "attempt=1 ups=ups-under-test command=enter_static_bypass phase=acknowledged "
                "ack=accepted observed=none verified=not_evaluated evidence_class=SYNTHETIC");

  const CliRun replay = store.run({"replay", "--key", "idem-1"});
  UC_CHECK_EXIT(replay, 0);
  UC_CHECK_LINE(replay, "attempt=1");
  UC_CHECK_LINE(replay, "idempotency_key=idem-1");
  UC_CHECK_LINE(replay, "phase=acknowledged");
}

UC_TEST(cli, the_same_key_with_a_different_intent_is_refused) {
  ReadyStore store("cli-idempotency-conflict");
  store.prepare();
  const CliRun first = store.enter_static_bypass("one-key", "140");
  UC_CHECK_EXIT(first, 0);
  const CliRun second = store.run({"run", "--now", "141", "--ups", kUps, "--command",
                                   "start_battery_test", "--authority", kGrant, "--key", "one-key"});
  UC_CHECK_EXIT(second, 1);
  UC_CHECK_LINE(second,
                "error: idempotency_conflict: idempotency key 'one-key' is already bound to a "
                "different request; the same key never describes two intents");
  const CliRun attempts = store.run({"attempts"});
  UC_CHECK_EXIT(attempts, 0);
  UC_CHECK_LINE(attempts, "count=1");
}

UC_TEST(cli, a_terminal_attempt_never_reopens) {
  ReadyStore store("cli-terminal");
  store.prepare();
  const CliRun run = store.enter_static_bypass("terminal-1", "140");
  UC_CHECK_EXIT(run, 0);
  UC_CHECK_LINE(run, "phase=acknowledged");
  const CliRun abandon =
      store.run({"abandon", "--now", "141", "--attempt", "1", "--reason", "abandoned"});
  UC_CHECK_EXIT(abandon, 2);
  UC_CHECK_LINE(abandon, "phase=failed");

  const CliRun verify = store.run({"verify", "--now", "142", "--attempt", "1"});
  UC_CHECK_EXIT(verify, 1);
  UC_CHECK_LINE(verify,
                "error: attempt_state_conflict: attempt 1 is already terminal in phase failed");

  const CliRun ack = store.run({"ack", "--now", "143", "--attempt", "1", "--outcome", "accepted"});
  UC_CHECK_EXIT(ack, 1);
  UC_CHECK_LINE(ack, "error: attempt_state_conflict: attempt 1 is already terminal in phase failed");

  const CliRun again =
      store.run({"abandon", "--now", "144", "--attempt", "1", "--reason", "again"});
  UC_CHECK_EXIT(again, 1);
  UC_CHECK_LINE(again,
                "error: attempt_state_conflict: attempt 1 is already terminal in phase failed");

  const CliRun missing = store.run({"verify", "--now", "145", "--attempt", "99"});
  UC_CHECK_EXIT(missing, 1);
  UC_CHECK_LINE(missing, "error: attempt_not_found: no attempt 99 exists");
}

UC_TEST(cli, attempts_are_listed_newest_first_and_the_limit_is_honoured) {
  ReadyStore store("cli-attempts");
  store.prepare();
  UC_CHECK_EXIT(store.enter_static_bypass("hist-1", "140"), 0);
  UC_CHECK_EXIT(store.enter_static_bypass("hist-2", "141"), 2);

  const CliRun all = store.run({"attempts"});
  UC_CHECK_EXIT(all, 0);
  UC_CHECK_LINE(all, "count=2");
  UC_CHECK_EQ(all.line_starting_with("attempt="),
              std::string("attempt=2 ups=ups-under-test command=enter_static_bypass phase=refused "
                          "ack=none observed=none verified=not_evaluated evidence_class=SYNTHETIC "
                          "refusal=transition_in_progress"));

  const CliRun limited = store.run({"attempts", "--limit", "1"});
  UC_CHECK_EXIT(limited, 0);
  UC_CHECK_LINE(limited, "count=1");
  UC_CHECK_TEXT(limited, "attempt=2 ups=ups-under-test");
  UC_CHECK(!limited.has_text("attempt=1 ups="));

  const CliRun by_phase = store.run({"attempts", "--phase", "refused"});
  UC_CHECK_EXIT(by_phase, 0);
  UC_CHECK_LINE(by_phase, "count=1");
  const CliRun verified_phase = store.run({"attempts", "--phase", "verified"});
  UC_CHECK_EXIT(verified_phase, 0);
  UC_CHECK_LINE(verified_phase, "count=0");
}

UC_TEST(cli, an_unknown_unit_is_refused_with_not_found) {
  ReadyStore store("cli-unknown-unit");
  store.prepare();
  const CliRun status = store.run({"status", "--now", "150", "--ups", "no-such-unit"});
  UC_CHECK_EXIT(status, 1);
  UC_CHECK_LINE(status, "error: not_found: no unit 'no-such-unit' is registered");

  const CliRun run = store.run({"run", "--now", "150", "--ups", "no-such-unit", "--command",
                                "enter_static_bypass", "--key", "ghost-1"});
  UC_CHECK_EXIT(run, 1);
  UC_CHECK_LINE(run, "error: not_found: no unit 'no-such-unit' is registered");
}

// ---------------------------------------------------------------------------
// Store errors
// ---------------------------------------------------------------------------

UC_TEST(cli, a_missing_store_is_refused_with_not_found) {
  ScratchDirectory scratch("cli-missing-store");
  const std::filesystem::path store = scratch.file("absent.upsstore");
  UC_REQUIRE(!std::filesystem::exists(store));

  const CliRun status = run_cli(scratch, {"status", "--store", store.string(), "--now", "10"});
  UC_CHECK_EXIT(status, 1);
  UC_CHECK_TEXT(status, "error: not_found: no store exists at '");
  UC_CHECK_TEXT(status, store.string());

  const CliRun audit = run_cli(scratch, {"store-audit", "--store", store.string()});
  UC_CHECK_EXIT(audit, 1);
  UC_CHECK_TEXT(audit, "error: not_found: no store exists at '");

  const CliRun verify = run_cli(scratch, {"verify-store", "--store", store.string()});
  UC_CHECK_EXIT(verify, 1);
  UC_CHECK_TEXT(verify, "error: not_found: file not found: '");
  UC_CHECK(!verify.has_text("verified=true"));
}

UC_TEST(cli, a_store_path_that_is_a_directory_is_refused) {
  ScratchDirectory scratch("cli-store-directory");
  const std::filesystem::path directory = scratch.file("not-a-store");
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  UC_REQUIRE(!error);
  UC_REQUIRE(std::filesystem::is_directory(directory));

  const CliRun status = run_cli(scratch, {"status", "--store", directory.string(), "--now", "10"});
  UC_CHECK_EXIT(status, 1);
  UC_CHECK_TEXT(status, "error: invalid_argument: store path '");
  UC_CHECK_TEXT(status, "is a directory");

  const CliRun verify = run_cli(scratch, {"verify-store", "--store", directory.string()});
  UC_CHECK_EXIT(verify, 1);
  UC_CHECK_TEXT(verify, "is a directory");
  UC_CHECK(!verify.has_text("verified=true"));

  const CliRun init = run_cli(scratch, {"init", "--store", directory.string(), "--now", "10",
                                        "--epoch", "1", "--incarnation", "1"});
  UC_CHECK_EXIT(init, 1);
  UC_CHECK_TEXT(init, "is a directory");
}

UC_TEST(cli, a_truncated_head_is_refused_as_corruption) {
  ScratchDirectory scratch("cli-corrupt-store");
  const std::filesystem::path store = scratch.file("unit.upsstore");
  UC_CHECK_EXIT(run_cli(scratch, {"init", "--store", store.string(), "--now", "100", "--epoch", "1",
                                  "--incarnation", "1"}),
                0);
  std::error_code error;
  std::filesystem::resize_file(store, 10, error);
  UC_REQUIRE(!error);
  UC_CHECK_EQ(std::filesystem::file_size(store), static_cast<std::uintmax_t>(10));

  const CliRun verify = run_cli(scratch, {"verify-store", "--store", store.string()});
  UC_CHECK_EXIT(verify, 1);
  UC_CHECK_TEXT(verify, "error: corruption: '");
  UC_CHECK_TEXT(verify, "a store head marker must be exactly 128 bytes; this file is 10 bytes");
  UC_CHECK(!verify.has_text("verified=true"));

  const CliRun status = run_cli(scratch, {"status", "--store", store.string(), "--now", "10"});
  UC_CHECK_EXIT(status, 1);
  UC_CHECK_TEXT(status, "error: corruption: '");
  const CliRun audit = run_cli(scratch, {"store-audit", "--store", store.string()});
  UC_CHECK_EXIT(audit, 1);
  UC_CHECK_TEXT(audit, "error: corruption: '");
}

}  // namespace
