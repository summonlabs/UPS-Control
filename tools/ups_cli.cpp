// ups-control: the inspection and administration command line tool.
//
// The tool is a thin shell over the library. Every command opens a real store,
// runs a real engine call, and prints what the engine returned; nothing about the
// model is reimplemented here. Commands that mutate the store report the durable
// generation they produced, commands that plan report the evaluation report, and
// commands that act report the attempt record.

#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cli_json.hpp"
#include "ups_control/engine.hpp"
#include "ups_control/version.hpp"

namespace {

using namespace ups_control;

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitRefused = 2;

constexpr std::size_t kMaxOptionValueBytes = 4096;
constexpr std::size_t kMaxOptionCount = 96;

struct Options {
  std::map<std::string, std::string> values;
  std::set<std::string> flags;
};

std::string trim(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::string();
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

const std::set<std::string>& boolean_flags() {
  static const std::set<std::string> flags = {"--json",          "--deny",
                                              "--suspend",       "--read-only",
                                              "--no-adapter",    "--enforce-path-binding",
                                              "--no-revalidate", "--help"};
  return flags;
}

bool parse_options(int argc, char** argv, int start, Options& options, std::string& error) {
  for (int index = start; index < argc; ++index) {
    const std::string token = argv[index];
    if (token == "-h") {
      options.flags.insert("--help");
      continue;
    }
    if (token.size() < 3 || token.compare(0, 2, "--") != 0) {
      error = "unexpected argument '" + token + "'; options are of the form --name value";
      return false;
    }
    if (boolean_flags().count(token) != 0) {
      options.flags.insert(token);
      continue;
    }
    if (index + 1 >= argc) {
      error = "option '" + token + "' requires a value";
      return false;
    }
    const std::string value = argv[++index];
    if (value.size() > kMaxOptionValueBytes) {
      error = "the value of '" + token + "' is longer than the accepted limit of " +
              std::to_string(kMaxOptionValueBytes) + " bytes";
      return false;
    }
    if (options.values.size() >= kMaxOptionCount) {
      error = "too many options; the limit is " + std::to_string(kMaxOptionCount);
      return false;
    }
    options.values[token] = value;
  }
  return true;
}

const std::string* find_option(const Options& options, const std::string& name) {
  const auto found = options.values.find(name);
  return found == options.values.end() ? nullptr : &found->second;
}

bool has_flag(const Options& options, const std::string& name) {
  return options.flags.count(name) != 0;
}

std::string optional_text(const Options& options, const std::string& name,
                          const std::string& fallback) {
  const std::string* value = find_option(options, name);
  return value == nullptr ? fallback : *value;
}

bool parse_i64(const std::string& text, std::int64_t& value) {
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

bool parse_u64(const std::string& text, std::uint64_t& value) {
  if (text.empty() || text[0] == '-') {
    return false;
  }
  std::size_t consumed = 0;
  try {
    value = std::stoull(text, &consumed, 10);
  } catch (const std::exception&) {
    return false;
  }
  return consumed == text.size();
}

bool require_text(const Options& options, const std::string& name, std::string& out) {
  const std::string* value = find_option(options, name);
  if (value == nullptr) {
    std::cerr << "error: --" << name.substr(2) << " is required\n";
    return false;
  }
  out = *value;
  return true;
}

bool require_i64_option(const Options& options, const std::string& name, std::int64_t& out) {
  std::string text;
  if (!require_text(options, name, text)) {
    return false;
  }
  if (!parse_i64(text, out)) {
    std::cerr << "error: --" << name.substr(2) << " expects an integer, got '" << text << "'\n";
    return false;
  }
  return true;
}

bool optional_i64_option(const Options& options, const std::string& name, std::int64_t& out) {
  const std::string* value = find_option(options, name);
  if (value == nullptr) {
    return false;
  }
  if (!parse_i64(*value, out)) {
    std::cerr << "error: --" << name.substr(2) << " expects an integer, got '" << *value << "'\n";
    return false;
  }
  return true;
}

bool read_bool3(const Options& options, const std::string& name, Bool3& out) {
  const std::string* value = find_option(options, name);
  if (value == nullptr) {
    return true;
  }
  const Result<Bool3> parsed = parse_bool3(*value);
  if (!parsed.ok()) {
    std::cerr << "error: --" << name.substr(2) << ": " << parsed.status().message() << "\n";
    return false;
  }
  out = parsed.value();
  return true;
}

bool require_now(const Options& options, Tick& now) {
  std::int64_t value = 0;
  if (!require_i64_option(options, "--now", value)) {
    return false;
  }
  if (value <= 0) {
    std::cerr << "error: --now must be a positive logical instant\n";
    return false;
  }
  now = Tick{value};
  return true;
}

int fail(const Status& status) {
  std::cerr << "error: " << status.to_string() << "\n";
  return kExitError;
}

struct EngineHandle {
  std::shared_ptr<UpsControlEngine> engine;
  std::string path;
};

/// Opens the store for one command.
///
/// A freshly opened engine is always Recovered, because state read from a store is
/// not current evidence. Every command that carries an explicit --now therefore
/// revalidates at that instant before doing anything else, which is exactly the
/// documented contract: the operator states the instant at which the recovered
/// state is being revalidated, and evidence freshness is still judged against it,
/// so evidence that has aged past its window stays stale. Pass --no-revalidate to
/// leave the engine Recovered and observe the state_not_revalidated refusal, or
/// --read-only to refuse write access entirely.
bool open_engine(const Options& options, bool writable_intent, std::shared_ptr<UpsAdapter> adapter,
                 EngineHandle& handle, bool auto_revalidate = true) {
  std::string path;
  if (!require_text(options, "--store", path)) {
    return false;
  }
  const bool writable = writable_intent && !has_flag(options, "--read-only");
  EngineOpenOptions open_options;
  open_options.store.path = path;
  open_options.store.access = writable ? StoreAccess::ReadWrite : StoreAccess::ReadOnly;
  open_options.store.create_if_missing = false;
  open_options.store.enforce_path_binding = has_flag(options, "--enforce-path-binding");
  // The generation floor is the durable defense against a store being replaced by
  // an older but perfectly valid copy of itself, which no checksum can detect. An
  // operator who knows the store should be past a generation records that floor
  // here, and a rollback to anything older is then refused instead of adopted.
  std::int64_t minimum_generation = 0;
  if (optional_i64_option(options, "--min-generation", minimum_generation)) {
    if (minimum_generation <= 0) {
      std::cerr << "error: --min-generation must be positive\n";
      return false;
    }
    open_options.store.minimum_generation =
        StoreGeneration{static_cast<std::uint64_t>(minimum_generation)};
  }
  const std::string expected_identity = optional_text(options, "--expect-identity", "");
  if (!expected_identity.empty()) {
    const Result<StoreIdentity> identity = StoreIdentity::parse(expected_identity);
    if (!identity.ok()) {
      fail(identity.status());
      return false;
    }
    open_options.store.expected_store_identity = identity.value();
  }
  open_options.store.created_at = Tick{1};
  open_options.store.epoch = ControlEpoch{1};
  open_options.store.incarnation = Incarnation{1};
  open_options.adapter = std::move(adapter);

  const Result<std::shared_ptr<UpsControlEngine>> engine = UpsControlEngine::open(open_options);
  if (!engine.ok()) {
    fail(engine.status());
    return false;
  }
  handle.engine = engine.value();
  handle.path = path;

  std::int64_t now_value = 0;
  if (!auto_revalidate || !writable || has_flag(options, "--no-revalidate") ||
      !optional_i64_option(options, "--now", now_value) || now_value <= 0) {
    return true;
  }
  const EngineInfo info = handle.engine->info();
  RevalidateRequest revalidate;
  revalidate.authority = ControlContext{info.epoch, info.incarnation};
  revalidate.now = Tick{now_value};
  const Result<RevalidationReport> report = handle.engine->revalidate(revalidate);
  if (!report.ok()) {
    fail(report.status());
    return false;
  }
  return true;
}

/// Resolves the authority the command is planned against. By default this is the
/// currently committed epoch and incarnation; the override flags exist so that a
/// stale-authority refusal can be produced and inspected on purpose.
bool resolve_context(const Options& options, UpsControlEngine& engine, ControlContext& context) {
  const EngineInfo info = engine.info();
  std::int64_t value = 0;
  if (optional_i64_option(options, "--epoch", value)) {
    if (value <= 0) {
      std::cerr << "error: --epoch must be positive\n";
      return false;
    }
    context.epoch = ControlEpoch{static_cast<std::uint64_t>(value)};
  } else {
    context.epoch = info.epoch;
  }
  if (optional_i64_option(options, "--incarnation", value)) {
    if (value <= 0) {
      std::cerr << "error: --incarnation must be positive\n";
      return false;
    }
    context.incarnation = Incarnation{static_cast<std::uint64_t>(value)};
  } else {
    context.incarnation = info.incarnation;
  }
  return true;
}

bool resolve_ups_ref(const Options& options, UpsControlEngine& engine, UpsRef& reference,
                     Tick now) {
  std::string ups;
  if (!require_text(options, "--ups", ups)) {
    return false;
  }
  const Result<UpsId> id = UpsId::parse(ups, engine.limits().max_identifier_bytes);
  if (!id.ok()) {
    fail(id.status());
    return false;
  }
  reference.ups = id.value();
  const Result<StatusReport> report = engine.status(UpsQuery{reference.ups, now});
  if (!report.ok()) {
    fail(report.status());
    return false;
  }
  reference.hardware = report.value().hardware;
  reference.revision = report.value().revision;
  std::int64_t value = 0;
  if (optional_i64_option(options, "--hardware", value)) {
    if (value <= 0 || value > 0xFFFFFFFFll) {
      std::cerr << "error: --hardware must be a positive 32-bit value\n";
      return false;
    }
    reference.hardware = HardwareGeneration{static_cast<std::uint32_t>(value)};
  }
  if (optional_i64_option(options, "--revision", value)) {
    if (value <= 0) {
      std::cerr << "error: --revision must be positive\n";
      return false;
    }
    reference.revision = StateRevision{static_cast<std::uint64_t>(value)};
  }
  return true;
}

bool parse_reserve_quantity_option(const Options& options, const std::string& unit_option,
                                   const std::string& value_option, UpsControlEngine& engine,
                                   std::optional<ReserveQuantity>& out) {
  const std::string* unit_text = find_option(options, unit_option);
  if (unit_text == nullptr) {
    return true;
  }
  const Result<ReserveUnit> unit = parse_reserve_unit(*unit_text);
  if (!unit.ok()) {
    fail(unit.status());
    return false;
  }
  std::int64_t value = 0;
  if (!require_i64_option(options, value_option, value)) {
    return false;
  }
  const Result<ReserveQuantity> quantity =
      make_reserve_quantity(unit.value(), value, engine.limits());
  if (!quantity.ok()) {
    fail(quantity.status());
    return false;
  }
  out = quantity.value();
  return true;
}

bool parse_instant_option(const Options& options, const std::string& name,
                          std::optional<Tick>& out) {
  std::int64_t value = 0;
  if (!optional_i64_option(options, name, value)) {
    return true;
  }
  if (value <= 0) {
    std::cerr << "error: --" << name.substr(2) << " must be a positive instant\n";
    return false;
  }
  out = Tick{value};
  return true;
}

std::string attempt_json(const AttemptRecord& record) {
  cli::JsonObject object;
  object.unsigned_number("attempt", record.id.value());
  object.text("idempotency_key", record.key.value());
  object.text("ups", record.ups.value());
  object.unsigned_number("hardware_generation", record.hardware.value());
  object.unsigned_number("epoch", record.epoch.value());
  object.unsigned_number("incarnation", record.incarnation.value());
  object.unsigned_number("planned_revision", record.planned_revision.value());
  object.text("command", to_string(record.command));
  object.text("target", to_string(record.target));
  object.text("phase", to_string(record.phase));
  object.text("ack", to_string(record.ack));
  object.text("observed", to_string(record.observed));
  object.text("verified", to_string(record.verification));
  object.text("evidence_class", to_string(record.evidence));
  object.number("submitted_at", record.submitted_at.value());
  object.number("acknowledged_at", record.acknowledged_at.value());
  object.number("observed_at", record.observed_at.value());
  object.number("verified_at", record.verified_at.value());
  object.unsigned_number("plan_digest", record.plan_digest);
  if (record.refusal.has_value()) {
    cli::JsonObject refusal;
    refusal.text("code", to_string(record.refusal->code));
    refusal.text("detail", record.refusal->detail);
    object.raw("refusal", refusal.str());
  }
  object.text("detail", record.terminal_detail);
  object.text("verification_detail", record.verification_detail);
  return object.str();
}

void print_attempt_text(const AttemptRecord& record) {
  std::cout << "attempt=" << record.id.value() << "\n";
  std::cout << "idempotency_key=" << record.key.value() << "\n";
  std::cout << "ups=" << record.ups.value() << "\n";
  std::cout << "command=" << to_string(record.command) << "\n";
  std::cout << "target_state=" << to_string(record.target) << "\n";
  std::cout << "phase=" << to_string(record.phase) << "\n";
  std::cout << "ack=" << to_string(record.ack) << "\n";
  std::cout << "observed=" << to_string(record.observed) << "\n";
  std::cout << "verified=" << to_string(record.verification) << "\n";
  std::cout << "evidence_class=" << to_string(record.evidence) << "\n";
  std::cout << "planned_revision=" << record.planned_revision.value() << "\n";
  if (record.refusal.has_value()) {
    std::cout << "refusal=" << to_string(record.refusal->code) << "\n";
    std::cout << "refusal_detail=" << record.refusal->detail << "\n";
  }
  if (!record.terminal_detail.empty()) {
    std::cout << "detail=" << record.terminal_detail << "\n";
  }
  if (!record.verification_detail.empty()) {
    std::cout << "verification_detail=" << record.verification_detail << "\n";
  }
}

int attempt_exit_code(const AttemptRecord& record) {
  switch (record.phase) {
    case AttemptPhase::Refused:
    case AttemptPhase::Failed:
    case AttemptPhase::Unsupported:
      return kExitRefused;
    default:
      return kExitOk;
  }
}

/// A synthetic adapter scripted from the command line. No hardware is involved and
/// every attempt it carries is labelled SYNTHETIC.
bool build_adapter(const Options& options, std::shared_ptr<UpsAdapter>& adapter) {
  adapter.reset();
  if (has_flag(options, "--no-adapter")) {
    return true;
  }
  SyntheticAdapter::Script script;
  script.supported_commands = all_command_kinds_mask();
  const std::string* unsupported = find_option(options, "--unsupported-cmd");
  if (unsupported != nullptr) {
    std::stringstream stream(*unsupported);
    std::string token;
    while (std::getline(stream, token, ',')) {
      const std::string name = trim(token);
      if (name.empty()) {
        continue;
      }
      const Result<CommandKind> kind = parse_command_kind(name);
      if (!kind.ok()) {
        std::cerr << "error: --unsupported-cmd: " << kind.status().message() << "\n";
        return false;
      }
      script.supported_commands &= ~command_kind_bit(kind.value());
    }
  }
  script.effects_log = optional_text(options, "--effects-log", "");
  script.detail = optional_text(options, "--adapter-detail", "");

  const std::string ack = optional_text(options, "--ack", "accepted");
  const std::string command = optional_text(options, "--command", "");
  if (!command.empty()) {
    const Result<CommandKind> kind = parse_command_kind(command);
    if (!kind.ok()) {
      std::cerr << "error: " << kind.status().message() << "\n";
      return false;
    }
    AckOutcome outcome = AckOutcome::Accepted;
    if (ack == "accepted") {
      outcome = AckOutcome::Accepted;
    } else if (ack == "rejected") {
      outcome = AckOutcome::Rejected;
    } else if (ack == "unsupported") {
      outcome = AckOutcome::Unsupported;
    } else if (ack == "no-response") {
      outcome = AckOutcome::NoResponse;
    } else if (ack == "malformed") {
      outcome = AckOutcome::Malformed;
    } else {
      std::cerr << "error: --ack expects accepted, rejected, unsupported, no-response, or malformed\n";
      return false;
    }
    script.acknowledgements[kind.value()] = outcome;
  }
  adapter = std::make_shared<SyntheticAdapter>(std::move(script));
  return true;
}

/// Builds the control request shared by evaluate and run.
bool build_command(const Options& options, UpsControlEngine& engine, Tick now,
                   ControlContext context, UpsRef reference, ControlCommand& command) {
  std::string kind_text;
  if (!require_text(options, "--command", kind_text)) {
    return false;
  }
  const Result<CommandKind> kind = parse_command_kind(kind_text);
  if (!kind.ok()) {
    fail(kind.status());
    return false;
  }
  command.authority = context;
  command.ref = reference;
  command.now = now;
  command.kind = kind.value();
  command.key = IdempotencyKey::parse(optional_text(options, "--key", "key-1"),
                                      engine.limits().max_idempotency_key_bytes)
                    .value();
  const std::string authority = optional_text(options, "--authority", "");
  if (!authority.empty()) {
    const Result<AuthorityRef> reference_id =
        AuthorityRef::parse(authority, engine.limits().max_identifier_bytes);
    if (!reference_id.ok()) {
      fail(reference_id.status());
      return false;
    }
    command.authority_ref = reference_id.value();
  }
  const std::string asserted = optional_text(options, "--asserted-target", "");
  if (!asserted.empty()) {
    const Result<OperatingState> state = parse_operating_state(asserted);
    if (!state.ok()) {
      fail(state.status());
      return false;
    }
    command.parameters.asserted_target = state.value();
  }
  std::int64_t dwell = 0;
  if (optional_i64_option(options, "--dwell", dwell)) {
    if (dwell < 0) {
      std::cerr << "error: --dwell must not be negative\n";
      return false;
    }
    command.parameters.verification_dwell = TickSpan{dwell};
  }
  std::optional<ReserveQuantity> requirement;
  if (!parse_reserve_quantity_option(options, "--reserve-unit", "--reserve-value", engine,
                                     requirement)) {
    return false;
  }
  if (requirement.has_value()) {
    ReserveRequirement floor;
    floor.minimum = requirement.value();
    command.parameters.reserve_requirement = floor;
  }
  return true;
}

bool require_key(const Options& options, UpsControlEngine& engine, IdempotencyKey& key) {
  std::string text;
  if (!require_text(options, "--key", text)) {
    return false;
  }
  const Result<IdempotencyKey> parsed = IdempotencyKey::parse(text, engine.limits().max_idempotency_key_bytes);
  if (!parsed.ok()) {
    fail(parsed.status());
    return false;
  }
  key = parsed.value();
  return true;
}

void print_usage() {
  std::cout <<
      "ups-control " << Version::string << " - UPS control, lifecycle, and reserve authority\n"
      "\n"
      "usage: ups-control <command> [options]\n"
      "\n"
      "Every command except 'version' and 'help' requires --store PATH. Commands that\n"
      "depend on current state require --now TICK, a positive logical instant. By\n"
      "default a command is planned against the currently committed epoch,\n"
      "incarnation, hardware generation, and state revision; --epoch, --incarnation,\n"
      "--hardware, and --revision override them so that stale-authority refusals can\n"
      "be produced and inspected on purpose.\n"
      "\n"
      "  version                       print the library version\n"
      "  init                          create a store\n"
      "      --store PATH --now TICK --epoch N --incarnation N\n"
      "  register                      register a UPS\n"
      "      --store P --now T --ups ID --hardware N [--label L] [--lifecycle S]\n"
      "      [--operating S] [--note N] [--floor-unit U --floor-value V]\n"
      "      [--max-age N]\n"
      "  status                        authoritative state of one unit, or all units\n"
      "      --store P --now T [--ups ID]\n"
      "  battery                       reserve evidence and its assessment\n"
      "      --store P --now T --ups ID\n"
      "  obligations                   protected-load obligations bound to a unit\n"
      "      --store P --now T --ups ID\n"
      "  bind-obligation               bind or re-assert an obligation\n"
      "      --store P --now T --ups ID --ref R --load L [--tier T] [--protection P]\n"
      "      [--expires TICK] [--asserted-by A] [--assertion-epoch N]\n"
      "  release-obligation            release or suspend a binding\n"
      "      --store P --now T --ups ID --ref R --revision N --authority A [--reason S]\n"
      "      [--suspend]\n"
      "  grant                         issue an authority grant\n"
      "      --store P --now T --ups ID --ref R --scope S [--authority A]\n"
      "      [--expires TICK] [--deny] [--floor-unit U --floor-value V]\n"
      "  revoke-grant                  revoke a grant\n"
      "      --store P --now T --ups ID --ref R --authority A\n"
      "  policy                        set the reserve policy of a unit\n"
      "      --store P --now T --ups ID [--floor-unit U --floor-value V] [--max-age N]\n"
      "      [--max-future-skew N] [--min-quality Q]\n"
      "  observe                       record a telemetry observation\n"
      "      --store P --now T --ups ID [--hardware N] [--operating S] [--source S]\n"
      "      [--provenance P] [--observed-at TICK] [--source-revision N] [--evidence ID]\n"
      "      [--reserve-unit U --reserve-value V --reserve-quality Q |\n"
      "       --reserve-unknown REASON] [--soc-value V | --soc-unknown REASON]\n"
      "      [--activity A] [--bypass-kind K] [--bypass-available B3]\n"
      "      [--bypass-qualified B3] [--synchronized B3] [--transfer-ready B3]\n"
      "      [--battery-ready B3] [--recharge-enabled B3] [--discharge-enabled B3]\n"
      "      [--fault-present B3] [--charge-inhibited B3] [--discharge-inhibited B3]\n"
      "  readiness                     what the unit can do now, and why not\n"
      "      --store P --now T --ups ID\n"
      "  evaluate                      evaluate a transition without acting\n"
      "      --store P --now T --ups ID --command C [--authority A] [--dwell N]\n"
      "      [--reserve-unit U --reserve-value V] [--asserted-target S]\n"
      "  run                           evaluate, record, and issue a command through the\n"
      "                                bound adapter; never reports an effect\n"
      "      as evaluate plus --key K [--ack OUTCOME] [--effects-log PATH]\n"
      "      [--unsupported-cmd C] [--no-adapter]\n"
      "  ack                           record an adapter acknowledgement\n"
      "      --store P --now T --attempt N --outcome O [--detail S]\n"
      "  verify                        verify the recorded effect of an attempt\n"
      "      --store P --now T --attempt N\n"
      "  abandon                       end an unresolved attempt without an effect\n"
      "      --store P --now T --attempt N --reason S\n"
      "  attempts                      list attempts, newest first\n"
      "      --store P [--ups ID] [--phase P] [--limit N]\n"
      "  replay                        return the stored result of an idempotency key\n"
      "      --store P --key K\n"
      "  adopt-authority               move the store to a new epoch or incarnation\n"
      "      --store P --now T --epoch N --incarnation N\n"
      "  adopt-state                   adopt a validated operating state\n"
      "      --store P --now T --ups ID --operating S [--reason S]\n"
      "  set-lifecycle                 move a unit through the lifecycle graph\n"
      "      --store P --now T --ups ID --lifecycle S [--reason S]\n"
      "  revalidate                    revalidate recovered state at an instant\n"
      "      --store P --now T\n"
      "  history                       durable commit history\n"
      "      --store P [--limit N]\n"
      "  verify-store                  verify a store artifact without opening it for\n"
      "                                writing\n"
      "      --store P\n"
      "  store-audit                   full store audit\n"
      "      --store P\n"
      "\n"
      "Global flags: --json --read-only --no-revalidate --enforce-path-binding\n"
      "              --min-generation N --expect-identity HEX32\n"
      "\n"
      "--min-generation is the durable floor that detects a store replaced by an\n"
      "older valid copy of itself, and --expect-identity refuses to adopt a store\n"
      "that is not the one the caller is bound to.\n"
      "\n"
      "A command that carries --now revalidates the recovered store at that instant\n"
      "before it does anything else, because state read from a store is not current\n"
      "evidence. --no-revalidate leaves the engine Recovered so that the\n"
      "state_not_revalidated refusal can be produced on purpose, and --read-only\n"
      "refuses write access entirely.\n"
      "\n"
      "Exit codes: 0 success or allowed, 2 the model refused the request,\n"
      "            1 usage, io, or store error.\n";
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int command_version() {
  std::cout << "ups-control " << Version::string << "\n";
  return kExitOk;
}

int command_init(const Options& options) {
  std::string path;
  std::int64_t now_value = 0;
  std::int64_t epoch_value = 0;
  std::int64_t incarnation_value = 0;
  if (!require_text(options, "--store", path) || !require_i64_option(options, "--now", now_value) ||
      !require_i64_option(options, "--epoch", epoch_value) ||
      !require_i64_option(options, "--incarnation", incarnation_value)) {
    return kExitError;
  }
  if (now_value <= 0 || epoch_value <= 0 || incarnation_value <= 0) {
    std::cerr << "error: --now, --epoch, and --incarnation must all be positive\n";
    return kExitError;
  }
  EngineOpenOptions open_options;
  open_options.store.path = path;
  open_options.store.access = StoreAccess::ReadWrite;
  open_options.store.create_if_missing = true;
  open_options.store.created_at = Tick{now_value};
  open_options.store.epoch = ControlEpoch{static_cast<std::uint64_t>(epoch_value)};
  open_options.store.incarnation = Incarnation{static_cast<std::uint64_t>(incarnation_value)};
  const Result<std::shared_ptr<UpsControlEngine>> engine = UpsControlEngine::open(open_options);
  if (!engine.ok()) {
    return fail(engine.status());
  }
  const EngineInfo info = engine.value()->info();
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.text("store", path_to_utf8(info.path));
    object.text("store_identity", info.store_identity.to_hex());
    object.unsigned_number("generation", info.generation.value());
    object.unsigned_number("epoch", info.epoch.value());
    object.unsigned_number("incarnation", info.incarnation.value());
    object.text("lifecycle", to_string(info.lifecycle));
    std::cout << object.str() << "\n";
  } else {
    std::cout << "store=" << path_to_utf8(info.path) << "\n";
    std::cout << "store_identity=" << info.store_identity.to_hex() << "\n";
    std::cout << "generation=" << info.generation.value() << "\n";
    std::cout << "epoch=" << info.epoch.value() << "\n";
    std::cout << "incarnation=" << info.incarnation.value() << "\n";
  }
  return kExitOk;
}

int command_register(const Options& options) {
  Tick now;
  std::int64_t hardware = 0;
  std::string ups;
  if (!require_now(options, now) || !require_i64_option(options, "--hardware", hardware) ||
      !require_text(options, "--ups", ups)) {
    return kExitError;
  }
  if (hardware <= 0 || hardware > 0xFFFFFFFFll) {
    std::cerr << "error: --hardware must be a positive 32-bit value\n";
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  if (!resolve_context(options, *handle.engine, context)) {
    return kExitError;
  }
  const Result<UpsId> id = UpsId::parse(ups, handle.engine->limits().max_identifier_bytes);
  if (!id.ok()) {
    return fail(id.status());
  }
  const Result<LifecycleState> lifecycle =
      parse_lifecycle_state(optional_text(options, "--lifecycle", "standby"));
  if (!lifecycle.ok()) {
    return fail(lifecycle.status());
  }
  const Result<OperatingState> operating =
      parse_operating_state(optional_text(options, "--operating", "unknown"));
  if (!operating.ok()) {
    return fail(operating.status());
  }

  RegisterUpsRequest request;
  request.authority = context;
  request.now = now;
  request.id = id.value();
  request.label = optional_text(options, "--label", ups);
  request.note = optional_text(options, "--note", "");
  request.hardware = HardwareGeneration{static_cast<std::uint32_t>(hardware)};
  request.lifecycle = lifecycle.value();
  request.operating = operating.value();
  std::optional<ReserveQuantity> floor;
  if (!parse_reserve_quantity_option(options, "--floor-unit", "--floor-value", *handle.engine,
                                     floor)) {
    return kExitError;
  }
  request.policy.discharge_floor = floor;
  std::int64_t max_age = 0;
  if (optional_i64_option(options, "--max-age", max_age)) {
    if (max_age < 0) {
      std::cerr << "error: --max-age must not be negative\n";
      return kExitError;
    }
    request.policy.max_evidence_age = TickSpan{max_age};
  }

  const Result<UpsRecord> record = handle.engine->register_ups(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.text("ups", record.value().id.value());
    object.text("label", record.value().label);
    object.unsigned_number("hardware_generation", record.value().hardware.value());
    object.text("lifecycle", to_string(record.value().lifecycle));
    object.text("operating", to_string(record.value().operating));
    object.unsigned_number("state_revision", record.value().revision.value());
    object.unsigned_number("generation", handle.engine->info().generation.value());
    std::cout << object.str() << "\n";
  } else {
    std::cout << "ups=" << record.value().id.value() << "\n";
    std::cout << "hardware_generation=" << record.value().hardware.value() << "\n";
    std::cout << "lifecycle=" << to_string(record.value().lifecycle) << "\n";
    std::cout << "operating=" << to_string(record.value().operating) << "\n";
    std::cout << "state_revision=" << record.value().revision.value() << "\n";
    std::cout << "generation=" << handle.engine->info().generation.value() << "\n";
  }
  return kExitOk;
}

std::string unit_summary_json(const UpsRecord& unit) {
  cli::JsonObject object;
  object.text("ups", unit.id.value());
  object.text("label", unit.label);
  object.unsigned_number("hardware_generation", unit.hardware.value());
  object.text("lifecycle", to_string(unit.lifecycle));
  object.text("operating", to_string(unit.operating));
  object.text("state_basis", to_string(unit.basis));
  object.unsigned_number("state_revision", unit.revision.value());
  object.boolean("in_flight", unit.in_flight.has_value());
  return object.str();
}

int command_status(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  const std::string* ups_option = find_option(options, "--ups");
  if (ups_option == nullptr) {
    const Result<std::vector<UpsRecord>> units = handle.engine->units();
    if (!units.ok()) {
      return fail(units.status());
    }
    if (has_flag(options, "--json")) {
      std::string array = "[";
      for (std::size_t index = 0; index < units.value().size(); ++index) {
        if (index > 0) {
          array += ",";
        }
        array += unit_summary_json(units.value()[index]);
      }
      array += "]";
      cli::JsonObject object;
      object.text("store", handle.path);
      object.unsigned_number("generation", handle.engine->info().generation.value());
      object.unsigned_number("units", units.value().size());
      object.raw("unit_list", array);
      std::cout << object.str() << "\n";
    } else {
      std::cout << "store=" << handle.path << "\n";
      std::cout << "generation=" << handle.engine->info().generation.value() << "\n";
      std::cout << "units=" << units.value().size() << "\n";
      for (const UpsRecord& unit : units.value()) {
        std::cout << "unit=" << unit.id.value() << " hardware_generation="
                  << unit.hardware.value() << " lifecycle=" << to_string(unit.lifecycle)
                  << " operating=" << to_string(unit.operating)
                  << " basis=" << to_string(unit.basis)
                  << " revision=" << unit.revision.value()
                  << " in_flight=" << (unit.in_flight.has_value() ? "true" : "false") << "\n";
      }
    }
    return kExitOk;
  }
  const Result<UpsId> id = UpsId::parse(*ups_option, handle.engine->limits().max_identifier_bytes);
  if (!id.ok()) {
    return fail(id.status());
  }
  const Result<StatusReport> report = handle.engine->status(UpsQuery{id.value(), now});
  if (!report.ok()) {
    return fail(report.status());
  }
  if (has_flag(options, "--json")) {
    const StatusReport& value = report.value();
    cli::JsonObject object;
    object.text("ups", value.ups.value());
    object.text("label", value.label);
    object.unsigned_number("hardware_generation", value.hardware.value());
    object.unsigned_number("epoch", value.epoch.value());
    object.unsigned_number("incarnation", value.incarnation.value());
    object.text("lifecycle", to_string(value.lifecycle));
    object.text("operating", to_string(value.operating));
    object.text("state_basis", to_string(value.basis));
    object.unsigned_number("state_revision", value.revision.value());
    object.number("state_since", value.state_since.value());
    object.boolean("revalidated", value.revalidated);
    object.text("observation_freshness", to_string(value.observation_freshness));
    object.text("reserve_outcome", to_string(value.reserve.outcome));
    object.text("reserve_reason", to_string(value.reserve.reason));
    object.text("reserve_detail", value.reserve.detail);
    object.unsigned_number("obligations", value.obligations.size());
    object.boolean("in_flight", value.in_flight.has_value());
    if (value.in_flight.has_value()) {
      object.unsigned_number("in_flight_attempt", value.in_flight->value());
    }
    std::cout << object.str() << "\n";
  } else {
    std::cout << describe_status(report.value());
  }
  return kExitOk;
}

int command_battery(const Options& options) {
  Tick now;
  std::string ups;
  if (!require_now(options, now) || !require_text(options, "--ups", ups)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  const Result<UpsId> id = UpsId::parse(ups, handle.engine->limits().max_identifier_bytes);
  if (!id.ok()) {
    return fail(id.status());
  }
  const Result<StatusReport> report = handle.engine->status(UpsQuery{id.value(), now});
  if (!report.ok()) {
    return fail(report.status());
  }
  const StatusReport& value = report.value();
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.text("ups", value.ups.value());
    object.text("observation_freshness", to_string(value.observation_freshness));
    object.number("observation_age", value.observation_age.value());
    object.text("reserve_outcome", to_string(value.reserve.outcome));
    object.text("reserve_reason", to_string(value.reserve.reason));
    object.text("reserve_detail", value.reserve.detail);
    if (value.reserve.observed.has_value()) {
      object.text("observed", format_reserve(value.reserve.observed.value()));
    }
    if (value.reserve.required.has_value()) {
      object.text("required", format_reserve(value.reserve.required.value()));
    }
    if (value.observation.has_value()) {
      object.text("evidence", value.observation->evidence.value());
      object.text("source", value.observation->source.value());
      object.text("provenance", to_string(value.observation->provenance));
      object.number("observed_at", value.observation->observed_at.value());
      object.text("reserve_state", to_string(value.observation->battery.reserve.state));
      object.text("reserve_unknown_reason",
                  to_string(value.observation->battery.reserve.unknown_reason));
      object.text("reserve_quality", to_string(value.observation->battery.reserve.quality));
      object.text("activity", to_string(value.observation->battery.activity));
    } else {
      object.boolean("observation_present", false);
    }
    std::cout << object.str() << "\n";
    return kExitOk;
  }
  std::cout << "ups=" << value.ups.value() << "\n";
  if (!value.observation.has_value()) {
    std::cout << "observation=none\n";
  } else {
    const ObservationRecord& observation = value.observation.value();
    std::cout << "evidence=" << observation.evidence.value() << "\n";
    std::cout << "source=" << observation.source.value() << "\n";
    std::cout << "provenance=" << to_string(observation.provenance) << "\n";
    std::cout << "observed_at=" << observation.observed_at.value() << "\n";
    std::cout << "observation_age=" << value.observation_age.value() << "\n";
    std::cout << "observation_freshness=" << to_string(value.observation_freshness) << "\n";
    if (observation.battery.reserve.state == ReserveState::Known) {
      std::cout << "reserve=" << format_reserve(observation.battery.reserve.quantity) << "\n";
      std::cout << "reserve_quality=" << to_string(observation.battery.reserve.quality) << "\n";
    } else {
      std::cout << "reserve=unknown\n";
      std::cout << "reserve_unknown_reason=" << to_string(observation.battery.reserve.unknown_reason)
                << "\n";
    }
    if (observation.battery.state_of_charge.state == ReserveState::Known) {
      std::cout << "state_of_charge=" << format_reserve(observation.battery.state_of_charge.quantity)
                << "\n";
    } else {
      std::cout << "state_of_charge=unknown\n";
    }
    std::cout << "activity=" << to_string(observation.battery.activity) << "\n";
  }
  std::cout << "reserve_outcome=" << to_string(value.reserve.outcome) << "\n";
  std::cout << "reserve_reason=" << to_string(value.reserve.reason) << "\n";
  std::cout << "reserve_detail=" << value.reserve.detail << "\n";
  return kExitOk;
}

int command_obligations(const Options& options) {
  Tick now;
  std::string ups;
  if (!require_text(options, "--ups", ups) || !require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  const Result<UpsId> id = UpsId::parse(ups, handle.engine->limits().max_identifier_bytes);
  if (!id.ok()) {
    return fail(id.status());
  }
  const Result<std::vector<ProtectedLoadObligation>> list =
      handle.engine->obligations(UpsQuery{id.value(), now});
  if (!list.ok()) {
    return fail(list.status());
  }
  if (has_flag(options, "--json")) {
    std::string array = "[";
    for (std::size_t index = 0; index < list.value().size(); ++index) {
      const ProtectedLoadObligation& obligation = list.value()[index];
      cli::JsonObject object;
      object.text("ref", obligation.ref.value());
      object.text("load", obligation.load.value());
      object.text("tier", to_string(obligation.tier));
      object.text("protection", to_string(obligation.protection));
      object.text("state", to_string(obligation.state));
      object.unsigned_number("revision", obligation.revision.value());
      object.text("asserted_by", obligation.asserted_by.value());
      object.boolean("binds", obligation_binds(obligation, now));
      object.boolean("lapsed", obligation_lapsed(obligation, now));
      if (index > 0) {
        array += ",";
      }
      array += object.str();
    }
    array += "]";
    cli::JsonObject root;
    root.text("ups", id.value().value());
    root.raw("obligations", array);
    std::cout << root.str() << "\n";
    return kExitOk;
  }
  std::cout << "ups=" << id.value().value() << "\n";
  std::cout << "count=" << list.value().size() << "\n";
  for (const ProtectedLoadObligation& obligation : list.value()) {
    std::cout << "obligation=" << obligation.ref.value() << " load=" << obligation.load.value()
              << " tier=" << to_string(obligation.tier)
              << " protection=" << to_string(obligation.protection)
              << " state=" << to_string(obligation.state)
              << " revision=" << obligation.revision.value()
              << " asserted_by=" << obligation.asserted_by.value()
              << " asserted_at=" << obligation.asserted_at.value()
              << " binds=" << (obligation_binds(obligation, now) ? "true" : "false")
              << " lapsed=" << (obligation_lapsed(obligation, now) ? "true" : "false");
    if (obligation.expires_at.has_value()) {
      std::cout << " expires_at=" << obligation.expires_at->value();
    }
    std::cout << "\n";
  }
  return kExitOk;
}

int command_bind_obligation(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  BindObligationRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  std::string ref_text;
  std::string load_text;
  if (!require_text(options, "--ref", ref_text) || !require_text(options, "--load", load_text)) {
    return kExitError;
  }
  const Result<ObligationRef> ref = ObligationRef::parse(ref_text, handle.engine->limits().max_identifier_bytes);
  if (!ref.ok()) {
    return fail(ref.status());
  }
  const Result<LoadId> load = LoadId::parse(load_text, handle.engine->limits().max_identifier_bytes);
  if (!load.ok()) {
    return fail(load.status());
  }
  const Result<ObligationTier> tier = parse_obligation_tier(optional_text(options, "--tier", "critical"));
  if (!tier.ok()) {
    return fail(tier.status());
  }
  const Result<ProtectionRequirement> protection = parse_protection_requirement(
      optional_text(options, "--protection", "must_remain_protected"));
  if (!protection.ok()) {
    return fail(protection.status());
  }
  request.obligation.ref = ref.value();
  request.obligation.load = load.value();
  request.obligation.tier = tier.value();
  request.obligation.protection = protection.value();
  const std::string asserted_by = optional_text(options, "--asserted-by", "feed-authority");
  const Result<AuthorityRef> authority = AuthorityRef::parse(asserted_by, handle.engine->limits().max_identifier_bytes);
  if (!authority.ok()) {
    return fail(authority.status());
  }
  request.obligation.asserted_by = authority.value();
  request.obligation.asserted_at = now;
  std::optional<Tick> expires;
  if (!parse_instant_option(options, "--expires", expires)) {
    return kExitError;
  }
  request.obligation.expires_at = expires;
  std::int64_t assertion_epoch = 0;
  if (optional_i64_option(options, "--assertion-epoch", assertion_epoch)) {
    if (assertion_epoch <= 0) {
      std::cerr << "error: --assertion-epoch must be positive\n";
      return kExitError;
    }
    request.obligation.epoch = ControlEpoch{static_cast<std::uint64_t>(assertion_epoch)};
    request.obligation.incarnation = context.incarnation;
  }

  const Result<ProtectedLoadObligation> bound = handle.engine->bind_obligation(request);
  if (!bound.ok()) {
    return fail(bound.status());
  }
  std::cout << "obligation=" << bound.value().ref.value() << "\n";
  std::cout << "load=" << bound.value().load.value() << "\n";
  std::cout << "revision=" << bound.value().revision.value() << "\n";
  std::cout << "state=" << to_string(bound.value().state) << "\n";
  std::cout << "binds=" << (obligation_binds(bound.value(), now) ? "true" : "false") << "\n";
  return kExitOk;
}

int command_release_obligation(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  ReleaseObligationRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  std::string ref_text;
  std::string authority_text;
  std::int64_t revision = 0;
  if (!require_text(options, "--ref", ref_text) || !require_text(options, "--authority", authority_text) ||
      !require_i64_option(options, "--revision", revision)) {
    return kExitError;
  }
  if (revision <= 0) {
    std::cerr << "error: --revision must be positive\n";
    return kExitError;
  }
  const Result<ObligationRef> ref = ObligationRef::parse(ref_text, handle.engine->limits().max_identifier_bytes);
  if (!ref.ok()) {
    return fail(ref.status());
  }
  const Result<AuthorityRef> authority = AuthorityRef::parse(authority_text, handle.engine->limits().max_identifier_bytes);
  if (!authority.ok()) {
    return fail(authority.status());
  }
  request.obligation = ref.value();
  request.obligation_revision = ObligationRevision{static_cast<std::uint64_t>(revision)};
  request.release_authority = authority.value();
  request.reason = optional_text(options, "--reason", "");
  request.outcome = has_flag(options, "--suspend") ? ObligationState::Suspended
                                                   : ObligationState::Released;

  const Result<ProtectedLoadObligation> released = handle.engine->release_obligation(request);
  if (!released.ok()) {
    return fail(released.status());
  }
  std::cout << "obligation=" << released.value().ref.value() << "\n";
  std::cout << "state=" << to_string(released.value().state) << "\n";
  std::cout << "revision=" << released.value().revision.value() << "\n";
  return kExitOk;
}

int command_grant(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  std::string ref_text;
  std::string scope_text;
  if (!require_text(options, "--ref", ref_text) || !require_text(options, "--scope", scope_text)) {
    return kExitError;
  }
  const Result<AuthorityRef> ref = AuthorityRef::parse(ref_text, handle.engine->limits().max_identifier_bytes);
  if (!ref.ok()) {
    return fail(ref.status());
  }
  const Result<GrantScope> scope = parse_grant_scope(scope_text);
  if (!scope.ok()) {
    return fail(scope.status());
  }
  IssueGrantRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  request.grant.ref = ref.value();
  request.grant.scope = scope.value();
  const std::string granted_by = optional_text(options, "--authority", "feed-authority");
  const Result<AuthorityRef> authority = AuthorityRef::parse(granted_by, handle.engine->limits().max_identifier_bytes);
  if (!authority.ok()) {
    return fail(authority.status());
  }
  request.grant.granted_by = authority.value();
  request.grant.issued_at = now;
  request.grant.allowed = !has_flag(options, "--deny");
  std::optional<Tick> expires;
  if (!parse_instant_option(options, "--expires", expires)) {
    return kExitError;
  }
  request.grant.expires_at = expires;
  std::optional<ReserveQuantity> floor;
  if (!parse_reserve_quantity_option(options, "--floor-unit", "--floor-value", *handle.engine, floor)) {
    return kExitError;
  }
  request.grant.reserve_floor = floor;

  const Result<AuthorityGrant> issued = handle.engine->issue_grant(request);
  if (!issued.ok()) {
    return fail(issued.status());
  }
  std::cout << "grant=" << issued.value().ref.value() << "\n";
  std::cout << "scope=" << to_string(issued.value().scope) << "\n";
  std::cout << "allowed=" << (issued.value().allowed ? "true" : "false") << "\n";
  std::cout << "epoch=" << issued.value().epoch.value() << "\n";
  std::cout << "incarnation=" << issued.value().incarnation.value() << "\n";
  std::cout << "revision=" << issued.value().revision.value() << "\n";
  return kExitOk;
}

int command_revoke_grant(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  std::string ref_text;
  std::string authority_text;
  if (!require_text(options, "--ref", ref_text) || !require_text(options, "--authority", authority_text)) {
    return kExitError;
  }
  const Result<AuthorityRef> ref = AuthorityRef::parse(ref_text, handle.engine->limits().max_identifier_bytes);
  if (!ref.ok()) {
    return fail(ref.status());
  }
  const Result<AuthorityRef> authority = AuthorityRef::parse(authority_text, handle.engine->limits().max_identifier_bytes);
  if (!authority.ok()) {
    return fail(authority.status());
  }
  RevokeGrantRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  request.grant = ref.value();
  request.revocation_authority = authority.value();
  const Result<AuthorityGrant> revoked = handle.engine->revoke_grant(request);
  if (!revoked.ok()) {
    return fail(revoked.status());
  }
  std::cout << "grant=" << revoked.value().ref.value() << "\n";
  std::cout << "revoked_at=" << revoked.value().revoked_at->value() << "\n";
  return kExitOk;
}

int command_policy(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  SetReservePolicyRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  std::optional<ReserveQuantity> floor;
  if (!parse_reserve_quantity_option(options, "--floor-unit", "--floor-value", *handle.engine, floor)) {
    return kExitError;
  }
  request.policy.discharge_floor = floor;
  std::int64_t value = 0;
  if (optional_i64_option(options, "--max-age", value)) {
    if (value < 0) {
      std::cerr << "error: --max-age must not be negative\n";
      return kExitError;
    }
    request.policy.max_evidence_age = TickSpan{value};
  }
  if (optional_i64_option(options, "--max-future-skew", value)) {
    if (value < 0) {
      std::cerr << "error: --max-future-skew must not be negative\n";
      return kExitError;
    }
    request.policy.max_future_skew = TickSpan{value};
  }
  const std::string quality = optional_text(options, "--min-quality", "");
  if (!quality.empty()) {
    const Result<EvidenceQuality> parsed = parse_evidence_quality(quality);
    if (!parsed.ok()) {
      return fail(parsed.status());
    }
    request.policy.minimum_quality = parsed.value();
  }
  const Result<UpsRecord> record = handle.engine->set_reserve_policy(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  std::cout << "ups=" << record.value().id.value() << "\n";
  std::cout << "state_revision=" << record.value().revision.value() << "\n";
  return kExitOk;
}

int command_observe(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  const Result<StatusReport> current =
      handle.engine->status(UpsQuery{reference.ups, now});
  if (!current.ok()) {
    return fail(current.status());
  }

  TelemetryReport report;
  const std::string evidence = optional_text(
      options, "--evidence", "obs-" + std::to_string(now.value()));
  const Result<EvidenceId> evidence_id =
      EvidenceId::parse(evidence, handle.engine->limits().max_identifier_bytes);
  if (!evidence_id.ok()) {
    return fail(evidence_id.status());
  }
  const std::string source = optional_text(options, "--source", "synthetic-source");
  const Result<SourceId> source_id = SourceId::parse(source, handle.engine->limits().max_identifier_bytes);
  if (!source_id.ok()) {
    return fail(source_id.status());
  }
  const Result<Provenance> provenance =
      parse_provenance(optional_text(options, "--provenance", "synthetic_adapter"));
  if (!provenance.ok()) {
    return fail(provenance.status());
  }
  const Result<OperatingState> operating =
      parse_operating_state(optional_text(options, "--operating", "unknown"));
  if (!operating.ok()) {
    return fail(operating.status());
  }
  report.evidence = evidence_id.value();
  report.ups = reference.ups;
  report.hardware = reference.hardware;
  report.source = source_id.value();
  report.provenance = provenance.value();
  std::int64_t source_revision = 0;
  if (optional_i64_option(options, "--source-revision", source_revision)) {
    if (source_revision <= 0) {
      std::cerr << "error: --source-revision must be positive\n";
      return kExitError;
    }
  } else if (current.value().observation.has_value()) {
    source_revision = static_cast<std::int64_t>(current.value().observation->revision.value() + 1);
  } else {
    source_revision = 1;
  }
  report.revision = SourceRevision{static_cast<std::uint64_t>(source_revision)};
  std::optional<Tick> observed_at;
  if (!parse_instant_option(options, "--observed-at", observed_at)) {
    return kExitError;
  }
  report.observed_at = observed_at.has_value() ? observed_at.value() : now;
  report.received_at = now;
  report.operating = operating.value();

  const std::string reserve_unknown = optional_text(options, "--reserve-unknown", "");
  if (!reserve_unknown.empty()) {
    const Result<UnknownReason> reason = parse_unknown_reason(reserve_unknown);
    if (!reason.ok()) {
      return fail(reason.status());
    }
    report.battery.reserve = unknown_reserve(reason.value());
  } else {
    std::optional<ReserveQuantity> quantity;
    if (!parse_reserve_quantity_option(options, "--reserve-unit", "--reserve-value", *handle.engine,
                                       quantity)) {
      return kExitError;
    }
    if (quantity.has_value()) {
      const Result<EvidenceQuality> quality =
          parse_evidence_quality(optional_text(options, "--reserve-quality", "measured"));
      if (!quality.ok()) {
        return fail(quality.status());
      }
      report.battery.reserve = known_reserve(quantity.value(), quality.value());
    }
  }

  const std::string soc_unknown = optional_text(options, "--soc-unknown", "");
  if (!soc_unknown.empty()) {
    const Result<UnknownReason> reason = parse_unknown_reason(soc_unknown);
    if (!reason.ok()) {
      return fail(reason.status());
    }
    report.battery.state_of_charge = unknown_reserve(reason.value());
  } else {
    std::int64_t soc = 0;
    if (optional_i64_option(options, "--soc-value", soc)) {
      const Result<ReserveQuantity> quantity =
          make_reserve_quantity(ReserveUnit::BasisPoints, soc, handle.engine->limits());
      if (!quantity.ok()) {
        return fail(quantity.status());
      }
      report.battery.state_of_charge = known_reserve(quantity.value(), EvidenceQuality::Measured);
    }
  }

  const std::string activity = optional_text(options, "--activity", "unknown");
  const Result<BatteryActivity> parsed_activity = parse_battery_activity(activity);
  if (!parsed_activity.ok()) {
    return fail(parsed_activity.status());
  }
  report.battery.activity = parsed_activity.value();

  const std::string bypass_kind = optional_text(options, "--bypass-kind", "unknown");
  if (bypass_kind == "unknown") {
    report.transfer.bypass_kind = BypassKind::Unknown;
  } else if (bypass_kind == "none") {
    report.transfer.bypass_kind = BypassKind::None;
  } else if (bypass_kind == "static") {
    report.transfer.bypass_kind = BypassKind::Static;
  } else if (bypass_kind == "maintenance") {
    report.transfer.bypass_kind = BypassKind::Maintenance;
  } else {
    std::cerr << "error: --bypass-kind expects unknown, none, static, or maintenance\n";
    return kExitError;
  }

  if (!read_bool3(options, "--bypass-available", report.transfer.bypass_available) ||
      !read_bool3(options, "--bypass-qualified", report.transfer.bypass_qualified) ||
      !read_bool3(options, "--synchronized", report.transfer.output_synchronized) ||
      !read_bool3(options, "--transfer-ready", report.transfer.transfer_ready) ||
      !read_bool3(options, "--battery-ready", report.transfer.battery_ready) ||
      !read_bool3(options, "--recharge-enabled", report.capability.recharge_enabled) ||
      !read_bool3(options, "--discharge-enabled", report.capability.discharge_enabled) ||
      !read_bool3(options, "--fault-present", report.fault_present) ||
      !read_bool3(options, "--charge-inhibited", report.battery.charge_inhibited) ||
      !read_bool3(options, "--discharge-inhibited", report.battery.discharge_inhibited)) {
    return kExitError;
  }

  RecordTelemetryRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  request.report = report;
  const Result<ObservationRecord> recorded = handle.engine->record_telemetry(request);
  if (!recorded.ok()) {
    return fail(recorded.status());
  }
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.text("evidence", recorded.value().evidence.value());
    object.text("source", recorded.value().source.value());
    object.text("provenance", to_string(recorded.value().provenance));
    object.text("operating", to_string(recorded.value().operating));
    object.boolean("contradictory", recorded.value().contradictory);
    object.text("contradiction_detail", recorded.value().contradiction_detail);
    object.unsigned_number("generation", handle.engine->info().generation.value());
    std::cout << object.str() << "\n";
  } else {
    std::cout << "evidence=" << recorded.value().evidence.value() << "\n";
    std::cout << "source=" << recorded.value().source.value() << "\n";
    std::cout << "provenance=" << to_string(recorded.value().provenance) << "\n";
    std::cout << "operating=" << to_string(recorded.value().operating) << "\n";
    std::cout << "contradictory=" << (recorded.value().contradictory ? "true" : "false") << "\n";
    if (recorded.value().contradictory) {
      std::cout << "contradiction_detail=" << recorded.value().contradiction_detail << "\n";
    }
    std::cout << "generation=" << handle.engine->info().generation.value() << "\n";
  }
  return kExitOk;
}

int command_readiness(const Options& options) {
  Tick now;
  std::string ups;
  if (!require_now(options, now) || !require_text(options, "--ups", ups)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  const Result<UpsId> id = UpsId::parse(ups, handle.engine->limits().max_identifier_bytes);
  if (!id.ok()) {
    return fail(id.status());
  }
  const Result<ReadinessReport> report = handle.engine->readiness(UpsQuery{id.value(), now});
  if (!report.ok()) {
    return fail(report.status());
  }
  if (has_flag(options, "--json")) {
    const ReadinessReport& value = report.value();
    cli::JsonObject object;
    object.text("ups", value.ups.value());
    object.text("lifecycle", to_string(value.lifecycle));
    object.text("operating", to_string(value.operating));
    object.text("state_basis", to_string(value.basis));
    object.boolean("revalidated", value.revalidated);
    object.text("reserve_outcome", to_string(value.reserve_outcome));
    object.text("reserve_freshness", to_string(value.reserve_freshness));
    object.boolean("static_bypass_ready", value.static_bypass_ready);
    object.boolean("maintenance_bypass_ready", value.maintenance_bypass_ready);
    object.boolean("transfer_ready", value.transfer_ready);
    object.boolean("test_ready", value.test_ready);
    object.boolean("recharge_authorized", value.recharge_authorized);
    object.boolean("discharge_authorized", value.discharge_authorized);
    object.unsigned_number("obligations", value.obligation_count);
    object.unsigned_number("protective_obligations", value.protective_obligation_count);
    std::vector<std::string> blockers;
    for (const EvaluationFinding& blocker : value.blockers) {
      blockers.push_back(std::string(to_string(blocker.code)) + ": " + blocker.detail);
    }
    object.strings("blockers", blockers);
    std::cout << object.str() << "\n";
  } else {
    std::cout << describe_readiness(report.value());
  }
  return kExitOk;
}

int command_evaluate(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  // Planning uses the same adapter binding as execution, because "would this be
  // allowed" is a question about the engine as configured. --no-adapter evaluates
  // the same request with no adapter bound, which refuses with adapter_unavailable
  // at its documented precedence.
  std::shared_ptr<UpsAdapter> adapter;
  if (!build_adapter(options, adapter)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, std::move(adapter), handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  ControlCommand command;
  if (!build_command(options, *handle.engine, now, context, reference, command)) {
    return kExitError;
  }
  const Result<TransitionEvaluation> evaluation = handle.engine->evaluate(command);
  if (!evaluation.ok()) {
    return fail(evaluation.status());
  }
  const EvaluationReport& report = evaluation.value().report;
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.text("ups", report.ups.value());
    object.text("command", to_string(report.command));
    object.text("current_state", to_string(report.current));
    object.text("target_state", to_string(report.target));
    object.boolean("allowed", report.allowed);
    object.text("primary_refusal", to_string(report.primary));
    object.text("primary_detail", report.primary_detail);
    object.text("protection_impact", to_string(report.protection.impact));
    object.text("authority", to_string(report.grant.verdict));
    std::vector<std::string> findings;
    for (const EvaluationFinding& finding : report.findings) {
      findings.push_back(std::string(to_string(finding.code)) + ": " + finding.detail);
    }
    std::vector<std::string> warnings;
    for (const EvaluationFinding& warning : report.warnings) {
      warnings.push_back(std::string(to_string(warning.code)) + ": " + warning.detail);
    }
    object.strings("findings", findings);
    object.strings("warnings", warnings);
    std::cout << object.str() << "\n";
  } else {
    std::cout << describe_evaluation(report);
  }
  return report.allowed ? kExitOk : kExitRefused;
}

int command_run(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  std::shared_ptr<UpsAdapter> adapter;
  if (!build_adapter(options, adapter)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, std::move(adapter), handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  ControlCommand command;
  if (!build_command(options, *handle.engine, now, context, reference, command)) {
    return kExitError;
  }
  const Result<AttemptRecord> record = handle.engine->submit(command);
  if (!record.ok()) {
    return fail(record.status());
  }
  if (has_flag(options, "--json")) {
    std::cout << attempt_json(record.value()) << "\n";
  } else {
    print_attempt_text(record.value());
  }
  return attempt_exit_code(record.value());
}

int command_ack(const Options& options) {
  Tick now;
  std::int64_t attempt = 0;
  if (!require_now(options, now) || !require_i64_option(options, "--attempt", attempt)) {
    return kExitError;
  }
  if (attempt <= 0) {
    std::cerr << "error: --attempt must be positive\n";
    return kExitError;
  }
  const std::string outcome_text = optional_text(options, "--outcome", "accepted");
  AckOutcome outcome = AckOutcome::Accepted;
  if (outcome_text == "accepted") {
    outcome = AckOutcome::Accepted;
  } else if (outcome_text == "rejected") {
    outcome = AckOutcome::Rejected;
  } else if (outcome_text == "unsupported") {
    outcome = AckOutcome::Unsupported;
  } else if (outcome_text == "no-response") {
    outcome = AckOutcome::NoResponse;
  } else if (outcome_text == "malformed") {
    outcome = AckOutcome::Malformed;
  } else {
    std::cerr << "error: --outcome expects accepted, rejected, unsupported, no-response, or malformed\n";
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  if (!resolve_context(options, *handle.engine, context)) {
    return kExitError;
  }
  AcknowledgeRequest request;
  request.authority = context;
  request.now = now;
  request.attempt = AttemptId{static_cast<std::uint64_t>(attempt)};
  request.outcome = outcome;
  request.adapter_detail = optional_text(options, "--detail", "");
  const Result<AttemptRecord> record = handle.engine->acknowledge(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  if (has_flag(options, "--json")) {
    std::cout << attempt_json(record.value()) << "\n";
  } else {
    print_attempt_text(record.value());
  }
  return attempt_exit_code(record.value());
}

int command_verify(const Options& options) {
  Tick now;
  std::int64_t attempt = 0;
  if (!require_now(options, now) || !require_i64_option(options, "--attempt", attempt)) {
    return kExitError;
  }
  if (attempt <= 0) {
    std::cerr << "error: --attempt must be positive\n";
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  if (!resolve_context(options, *handle.engine, context)) {
    return kExitError;
  }
  VerifyRequest request;
  request.authority = context;
  request.now = now;
  request.attempt = AttemptId{static_cast<std::uint64_t>(attempt)};
  const Result<AttemptRecord> record = handle.engine->verify(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  if (has_flag(options, "--json")) {
    std::cout << attempt_json(record.value()) << "\n";
  } else {
    print_attempt_text(record.value());
  }
  return record.value().verification == VerificationVerdict::Verified ? kExitOk : kExitRefused;
}

int command_abandon(const Options& options) {
  Tick now;
  std::int64_t attempt = 0;
  if (!require_now(options, now) || !require_i64_option(options, "--attempt", attempt)) {
    return kExitError;
  }
  if (attempt <= 0) {
    std::cerr << "error: --attempt must be positive\n";
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  if (!resolve_context(options, *handle.engine, context)) {
    return kExitError;
  }
  AbandonRequest request;
  request.authority = context;
  request.now = now;
  request.attempt = AttemptId{static_cast<std::uint64_t>(attempt)};
  request.reason = optional_text(options, "--reason", "");
  const Result<AttemptRecord> record = handle.engine->abandon(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  if (has_flag(options, "--json")) {
    std::cout << attempt_json(record.value()) << "\n";
  } else {
    print_attempt_text(record.value());
  }
  return attempt_exit_code(record.value());
}

int command_attempts(const Options& options) {
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  HistoryQuery query;
  const std::string* ups_option = find_option(options, "--ups");
  if (ups_option != nullptr) {
    const Result<UpsId> id = UpsId::parse(*ups_option, handle.engine->limits().max_identifier_bytes);
    if (!id.ok()) {
      return fail(id.status());
    }
    query.ups = id.value();
  }
  const std::string* phase_option = find_option(options, "--phase");
  if (phase_option != nullptr) {
    bool matched = false;
    for (int index = 1; index <= static_cast<int>(AttemptPhase::Unsupported); ++index) {
      if (*phase_option == to_string(static_cast<AttemptPhase>(index))) {
        query.phase = static_cast<AttemptPhase>(index);
        matched = true;
        break;
      }
    }
    if (!matched) {
      std::cerr << "error: unknown phase '" << *phase_option << "'\n";
      return kExitError;
    }
  }
  std::int64_t limit = 0;
  if (optional_i64_option(options, "--limit", limit)) {
    if (limit <= 0) {
      std::cerr << "error: --limit must be positive\n";
      return kExitError;
    }
    query.limit = static_cast<std::size_t>(limit);
  }
  const Result<std::vector<AttemptRecord>> records = handle.engine->history(query);
  if (!records.ok()) {
    return fail(records.status());
  }
  if (has_flag(options, "--json")) {
    std::string array = "[";
    for (std::size_t index = 0; index < records.value().size(); ++index) {
      if (index > 0) {
        array += ",";
      }
      array += attempt_json(records.value()[index]);
    }
    array += "]";
    cli::JsonObject object;
    object.raw("attempts", array);
    std::cout << object.str() << "\n";
    return kExitOk;
  }
  std::cout << "count=" << records.value().size() << "\n";
  for (const AttemptRecord& record : records.value()) {
    std::cout << "attempt=" << record.id.value() << " ups=" << record.ups.value()
              << " command=" << to_string(record.command)
              << " phase=" << to_string(record.phase) << " ack=" << to_string(record.ack)
              << " observed=" << to_string(record.observed)
              << " verified=" << to_string(record.verification)
              << " evidence_class=" << to_string(record.evidence);
    if (record.refusal.has_value()) {
      std::cout << " refusal=" << to_string(record.refusal->code);
    }
    std::cout << "\n";
  }
  return kExitOk;
}

int command_replay(const Options& options) {
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  IdempotencyKey key;
  if (!require_key(options, *handle.engine, key)) {
    return kExitError;
  }
  const Result<AttemptRecord> record = handle.engine->replay(key);
  if (!record.ok()) {
    return fail(record.status());
  }
  if (has_flag(options, "--json")) {
    std::cout << attempt_json(record.value()) << "\n";
  } else {
    print_attempt_text(record.value());
  }
  return attempt_exit_code(record.value());
}

int command_adopt_authority(const Options& options) {
  Tick now;
  std::int64_t epoch = 0;
  std::int64_t incarnation = 0;
  if (!require_now(options, now) || !require_i64_option(options, "--epoch", epoch) ||
      !require_i64_option(options, "--incarnation", incarnation)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  const EngineInfo info = handle.engine->info();
  std::int64_t current_epoch = 0;
  std::int64_t current_incarnation = 0;
  if (optional_i64_option(options, "--from-epoch", current_epoch) ||
      optional_i64_option(options, "--from-incarnation", current_incarnation)) {
    // Overrides are handled below through the same request shape.
  }
  AdoptAuthorityRequest request;
  request.authority.epoch = current_epoch > 0
                                ? ControlEpoch{static_cast<std::uint64_t>(current_epoch)}
                                : info.epoch;
  request.authority.incarnation = current_incarnation > 0
                                      ? Incarnation{static_cast<std::uint64_t>(current_incarnation)}
                                      : info.incarnation;
  request.now = now;
  request.epoch = ControlEpoch{static_cast<std::uint64_t>(epoch)};
  request.incarnation = Incarnation{static_cast<std::uint64_t>(incarnation)};
  const Result<ControlContext> context = handle.engine->adopt_authority(request);
  if (!context.ok()) {
    return fail(context.status());
  }
  std::cout << "epoch=" << context.value().epoch.value() << "\n";
  std::cout << "incarnation=" << context.value().incarnation.value() << "\n";
  std::cout << "generation=" << handle.engine->info().generation.value() << "\n";
  return kExitOk;
}

int command_adopt_state(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  const Result<OperatingState> operating =
      parse_operating_state(optional_text(options, "--operating", "unknown"));
  if (!operating.ok()) {
    return fail(operating.status());
  }
  AdoptOperatingStateRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  request.operating = operating.value();
  request.reason = optional_text(options, "--reason", "");
  const Result<UpsRecord> record = handle.engine->adopt_operating_state(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  std::cout << "ups=" << record.value().id.value() << "\n";
  std::cout << "operating=" << to_string(record.value().operating) << "\n";
  std::cout << "state_basis=" << to_string(record.value().basis) << "\n";
  std::cout << "state_revision=" << record.value().revision.value() << "\n";
  return kExitOk;
}

int command_set_lifecycle(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  if (!open_engine(options, true, nullptr, handle)) {
    return kExitError;
  }
  ControlContext context;
  UpsRef reference;
  if (!resolve_context(options, *handle.engine, context) ||
      !resolve_ups_ref(options, *handle.engine, reference, now)) {
    return kExitError;
  }
  const Result<LifecycleState> lifecycle =
      parse_lifecycle_state(optional_text(options, "--lifecycle", "in_service"));
  if (!lifecycle.ok()) {
    return fail(lifecycle.status());
  }
  SetLifecycleRequest request;
  request.authority = context;
  request.ref = reference;
  request.now = now;
  request.lifecycle = lifecycle.value();
  request.reason = optional_text(options, "--reason", "");
  const Result<UpsRecord> record = handle.engine->set_lifecycle(request);
  if (!record.ok()) {
    return fail(record.status());
  }
  std::cout << "ups=" << record.value().id.value() << "\n";
  std::cout << "lifecycle=" << to_string(record.value().lifecycle) << "\n";
  std::cout << "state_revision=" << record.value().revision.value() << "\n";
  return kExitOk;
}

int command_revalidate(const Options& options) {
  Tick now;
  if (!require_now(options, now)) {
    return kExitError;
  }
  EngineHandle handle;
  // This command performs the revalidation itself, so the automatic one is
  // suppressed and exactly one durable commit is produced.
  if (!open_engine(options, true, nullptr, handle, /*auto_revalidate=*/false)) {
    return kExitError;
  }
  ControlContext context;
  if (!resolve_context(options, *handle.engine, context)) {
    return kExitError;
  }
  RevalidateRequest request;
  request.authority = context;
  request.now = now;
  const Result<RevalidationReport> report = handle.engine->revalidate(request);
  if (!report.ok()) {
    return fail(report.status());
  }
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.number("revalidated_at", report.value().now.value());
    object.unsigned_number("generation", report.value().generation.value());
    object.unsigned_number("units", report.value().unit_count);
    object.unsigned_number("units_with_fresh_reserve", report.value().units_with_fresh_reserve);
    object.unsigned_number("units_with_stale_reserve", report.value().units_with_stale_reserve);
    object.unsigned_number("units_with_insufficient_reserve",
                           report.value().units_with_insufficient_reserve);
    object.unsigned_number("units_with_unknown_reserve", report.value().units_with_unknown_reserve);
    object.unsigned_number("unresolved_attempts", report.value().unresolved_attempts);
    std::cout << object.str() << "\n";
  } else {
    std::cout << describe_revalidation(report.value());
  }
  return kExitOk;
}

int command_history(const Options& options) {
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  const Result<StoreAuditReport> audit = handle.engine->store_audit();
  if (!audit.ok()) {
    return fail(audit.status());
  }
  std::int64_t limit = 0;
  std::size_t maximum = audit.value().commit_log.size();
  if (optional_i64_option(options, "--limit", limit)) {
    if (limit <= 0) {
      std::cerr << "error: --limit must be positive\n";
      return kExitError;
    }
    maximum = std::min<std::size_t>(static_cast<std::size_t>(limit), maximum);
  }
  const std::string* ups_option = find_option(options, "--ups");
  std::cout << "store=" << handle.path << "\n";
  std::cout << "generation=" << audit.value().generation.value() << "\n";
  std::cout << "operation_count=" << audit.value().operation_count << "\n";
  std::size_t emitted = 0;
  for (auto iterator = audit.value().commit_log.rbegin();
       iterator != audit.value().commit_log.rend() && emitted < maximum; ++iterator, ++emitted) {
    if (ups_option != nullptr && iterator->ups.value() != *ups_option) {
      continue;
    }
    std::cout << "commit generation=" << iterator->generation.value()
              << " operation=" << to_string(iterator->operation)
              << " ups=" << iterator->ups.value()
              << " attempt=" << iterator->attempt.value()
              << " revision=" << iterator->revision.value()
              << " at=" << iterator->at.value();
    if (!iterator->key.value().empty()) {
      std::cout << " key=" << iterator->key.value();
    }
    std::cout << "\n";
  }
  return kExitOk;
}

int command_verify_store(const Options& options) {
  std::string path;
  if (!require_text(options, "--store", path)) {
    return kExitError;
  }
  StoreReadOptions read_options;
  read_options.enforce_path_binding = has_flag(options, "--enforce-path-binding");
  const Status status = UpsStore::verify_file(path, read_options);
  if (!status.ok()) {
    return fail(status);
  }
  const Result<std::shared_ptr<const UpsState>> state = UpsStore::read_file(path, read_options);
  if (!state.ok()) {
    return fail(state.status());
  }
  std::cout << "store=" << path << "\n";
  std::cout << "verified=true\n";
  std::cout << "generation=" << state.value()->generation.value() << "\n";
  std::cout << "epoch=" << state.value()->epoch.value() << "\n";
  std::cout << "incarnation=" << state.value()->incarnation.value() << "\n";
  std::cout << "units=" << state.value()->units.size() << "\n";
  std::cout << "attempts=" << state.value()->attempts.size() << "\n";
  std::cout << "canonical_digest=" << canonical_state_digest(*state.value()) << "\n";
  return kExitOk;
}

int command_store_audit(const Options& options) {
  EngineHandle handle;
  if (!open_engine(options, false, nullptr, handle)) {
    return kExitError;
  }
  const Result<StoreAuditReport> audit = handle.engine->store_audit();
  if (!audit.ok()) {
    return fail(audit.status());
  }
  const StoreAuditReport& value = audit.value();
  if (has_flag(options, "--json")) {
    cli::JsonObject object;
    object.text("store", path_to_utf8(value.path));
    object.text("store_identity", value.store_identity.to_hex());
    object.unsigned_number("generation", value.generation.value());
    object.unsigned_number("epoch", value.epoch.value());
    object.unsigned_number("incarnation", value.incarnation.value());
    object.number("created_at", value.created_at.value());
    object.number("updated_at", value.updated_at.value());
    object.number("revalidated_at", value.revalidated_at.value());
    object.unsigned_number("format_version", value.format_version);
    object.unsigned_number("head_bytes", value.head_bytes);
    object.unsigned_number("payload_bytes", value.payload_bytes);
    object.unsigned_number("payload_crc32c", value.payload_crc32c);
    object.text("recorded_path", value.recorded_path);
    object.boolean("path_binding_matches", value.path_binding_matches);
    object.unsigned_number("units", value.unit_count);
    object.unsigned_number("attempts", value.attempt_count);
    object.unsigned_number("unresolved_attempts", value.unresolved_attempt_count);
    object.unsigned_number("terminal_attempts", value.terminal_attempt_count);
    object.unsigned_number("idempotency_bindings", value.idempotency_count);
    object.unsigned_number("operation_count", value.operation_count);
    object.unsigned_number("canonical_digest", value.canonical_digest);
    object.boolean("writable", value.writable);
    std::cout << object.str() << "\n";
    return kExitOk;
  }
  std::cout << "store=" << path_to_utf8(value.path) << "\n";
  std::cout << "store_identity=" << value.store_identity.to_hex() << "\n";
  std::cout << "generation=" << value.generation.value() << "\n";
  std::cout << "epoch=" << value.epoch.value() << "\n";
  std::cout << "incarnation=" << value.incarnation.value() << "\n";
  std::cout << "created_at=" << value.created_at.value() << "\n";
  std::cout << "updated_at=" << value.updated_at.value() << "\n";
  std::cout << "revalidated_at=" << value.revalidated_at.value() << "\n";
  std::cout << "format_version=" << value.format_version << "\n";
  std::cout << "head_bytes=" << value.head_bytes << "\n";
  std::cout << "payload_bytes=" << value.payload_bytes << "\n";
  std::cout << "payload_crc32c=" << value.payload_crc32c << "\n";
  std::cout << "recorded_path=" << value.recorded_path << "\n";
  std::cout << "path_binding_matches=" << (value.path_binding_matches ? "true" : "false") << "\n";
  std::cout << "units=" << value.unit_count << "\n";
  std::cout << "attempts=" << value.attempt_count << "\n";
  std::cout << "unresolved_attempts=" << value.unresolved_attempt_count << "\n";
  std::cout << "terminal_attempts=" << value.terminal_attempt_count << "\n";
  std::cout << "idempotency_bindings=" << value.idempotency_count << "\n";
  std::cout << "operation_count=" << value.operation_count << "\n";
  std::cout << "canonical_digest=" << value.canonical_digest << "\n";
  std::cout << "writable=" << (value.writable ? "true" : "false") << "\n";
  std::cout << "commit_log_entries=" << value.commit_log.size() << "\n";
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return kExitError;
  }
  const std::string command = argv[1];
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return kExitOk;
  }
  Options options;
  std::string error;
  if (!parse_options(argc, argv, 2, options, error)) {
    std::cerr << "error: " << error << "\n";
    return kExitError;
  }
  if (has_flag(options, "--help")) {
    print_usage();
    return kExitOk;
  }
  try {
    if (command == "version") {
      return command_version();
    }
    if (command == "init") {
      return command_init(options);
    }
    if (command == "register") {
      return command_register(options);
    }
    if (command == "status") {
      return command_status(options);
    }
    if (command == "battery") {
      return command_battery(options);
    }
    if (command == "obligations") {
      return command_obligations(options);
    }
    if (command == "bind-obligation") {
      return command_bind_obligation(options);
    }
    if (command == "release-obligation") {
      return command_release_obligation(options);
    }
    if (command == "grant") {
      return command_grant(options);
    }
    if (command == "revoke-grant") {
      return command_revoke_grant(options);
    }
    if (command == "policy") {
      return command_policy(options);
    }
    if (command == "observe") {
      return command_observe(options);
    }
    if (command == "readiness") {
      return command_readiness(options);
    }
    if (command == "evaluate") {
      return command_evaluate(options);
    }
    if (command == "run") {
      return command_run(options);
    }
    if (command == "ack") {
      return command_ack(options);
    }
    if (command == "verify") {
      return command_verify(options);
    }
    if (command == "abandon") {
      return command_abandon(options);
    }
    if (command == "attempts") {
      return command_attempts(options);
    }
    if (command == "replay") {
      return command_replay(options);
    }
    if (command == "adopt-authority") {
      return command_adopt_authority(options);
    }
    if (command == "adopt-state") {
      return command_adopt_state(options);
    }
    if (command == "set-lifecycle") {
      return command_set_lifecycle(options);
    }
    if (command == "revalidate") {
      return command_revalidate(options);
    }
    if (command == "history") {
      return command_history(options);
    }
    if (command == "verify-store") {
      return command_verify_store(options);
    }
    if (command == "store-audit") {
      return command_store_audit(options);
    }
    std::cerr << "error: unknown command '" << command << "'\n";
    print_usage();
    return kExitError;
  } catch (const std::exception& failure) {
    std::cerr << "error: " << failure.what() << "\n";
    return kExitError;
  }
}
