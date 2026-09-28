#include "fixture.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "proc.hpp"

namespace uc_test {
namespace {

using namespace ups_control;

std::string unique_suffix() {
  static std::uint64_t counter = 0;
  ++counter;
  std::ostringstream text;
  text << "-" << current_pid() << "-" << counter;
  return text.str();
}

}  // namespace

ScratchDirectory::ScratchDirectory(const std::string& name) {
  std::error_code error;
  std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    base = std::filesystem::current_path();
  }
  path_ = base / ("ups-control-tests-" + name + unique_suffix());
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
}

ScratchDirectory::~ScratchDirectory() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path ScratchDirectory::file(const std::string& name) const {
  return path_ / name;
}

TelemetryReport healthy_report(const UpsId& ups, HardwareGeneration hardware, Tick at,
                               SourceRevision revision, const std::string& evidence) {
  TelemetryReport report;
  report.evidence = EvidenceId::parse(evidence).value();
  report.ups = ups;
  report.hardware = hardware;
  report.source = SourceId::parse("test-source").value();
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
  report.battery.reserve =
      known_reserve(ReserveQuantity{ReserveUnit::Seconds, 900}, EvidenceQuality::Measured);
  report.battery.activity = BatteryActivity::Idle;
  return report;
}

Fixture::Fixture(const std::string& test_name) {
  scratch = std::make_unique<ScratchDirectory>(test_name);
  store_options.path = scratch->file("unit.upsstore");
  store_options.access = StoreAccess::ReadWrite;
  store_options.create_if_missing = true;
  store_options.created_at = now;
  store_options.epoch = context.epoch;
  store_options.incarnation = context.incarnation;
}

bool Fixture::build(GrantScope scope, ReserveUnit floor_unit, std::int64_t floor_value) {
  adapter = std::make_shared<SyntheticAdapter>(script);
  open_options.store = store_options;
  open_options.adapter = adapter;
  const Result<std::shared_ptr<UpsControlEngine>> opened = UpsControlEngine::open(open_options);
  if (!opened.ok()) {
    last_error = opened.status().to_string();
    return false;
  }
  engine = opened.value();

  RevalidateRequest revalidate_request;
  revalidate_request.authority = context;
  revalidate_request.now = now;
  const Result<RevalidationReport> revalidated = engine->revalidate(revalidate_request);
  if (!revalidated.ok()) {
    last_error = revalidated.status().to_string();
    return false;
  }

  RegisterUpsRequest registration;
  registration.authority = context;
  registration.now = now;
  const Result<UpsId> id = UpsId::parse("ups-under-test");
  if (!id.ok()) {
    last_error = id.status().to_string();
    return false;
  }
  registration.id = id.value();
  registration.label = "unit under test";
  registration.hardware = HardwareGeneration{1};
  registration.lifecycle = LifecycleState::InService;
  registration.operating = OperatingState::OnlineNormal;
  registration.policy.max_evidence_age = TickSpan{600};
  registration.policy.discharge_floor = ReserveQuantity{floor_unit, floor_value};
  const Result<UpsRecord> registered = engine->register_ups(registration);
  if (!registered.ok()) {
    last_error = registered.status().to_string();
    return false;
  }

  const Result<ObservationRecord> observed = engine->record_telemetry(RecordTelemetryRequest{
      context, UpsRef{registered.value().id, registered.value().hardware, registered.value().revision},
      now, healthy_report(registered.value().id, registered.value().hardware, now, SourceRevision{1},
                          "ev-1")});
  if (!observed.ok()) {
    last_error = observed.status().to_string();
    return false;
  }

  IssueGrantRequest grant_request;
  grant_request.authority = context;
  const Result<std::vector<UpsRecord>> units = engine->units();
  if (!units.ok()) {
    last_error = units.status().to_string();
    return false;
  }
  grant_request.ref = UpsRef{units.value().front().id, units.value().front().hardware,
                             units.value().front().revision};
  grant_request.now = now;
  grant_request.grant.ref = AuthorityRef::parse("test-authority").value();
  grant_request.grant.scope = scope;
  grant_request.grant.granted_by = AuthorityRef::parse("facility-ops").value();
  grant_request.grant.issued_at = now;
  grant_request.grant.expires_at = Tick{100000000};
  const Result<AuthorityGrant> granted = engine->issue_grant(grant_request);
  if (!granted.ok()) {
    last_error = granted.status().to_string();
    return false;
  }
  return true;
}

Status Fixture::revalidate(Tick at) {
  RevalidateRequest request;
  request.authority = context;
  request.now = at;
  return engine->revalidate(request).status();
}

Result<UpsRecord> Fixture::current() {
  const Result<std::vector<UpsRecord>> units = engine->units();
  if (!units.ok()) {
    return units.status();
  }
  if (units.value().empty()) {
    return Status::error(StatusCode::NotFound, "no unit is registered");
  }
  return units.value().front();
}

Result<ObservationRecord> Fixture::observe(const TelemetryReport& report, Tick at) {
  const Result<UpsRecord> unit = current();
  if (!unit.ok()) {
    return unit.status();
  }
  RecordTelemetryRequest request;
  request.authority = context;
  request.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
  request.now = at;
  request.report = report;
  return engine->record_telemetry(request);
}

ControlCommand Fixture::command(CommandKind kind, const std::string& key, Tick at,
                                const std::string& authority) {
  ControlCommand control;
  control.authority = context;
  const Result<UpsRecord> unit = current();
  if (unit.ok()) {
    control.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
  }
  control.now = at;
  const Result<IdempotencyKey> parsed = IdempotencyKey::parse(key);
  if (parsed.ok()) {
    control.key = parsed.value();
  }
  control.kind = kind;
  control.authority_ref = AuthorityRef::parse(authority).value();
  return control;
}

Result<AttemptRecord> Fixture::submit(CommandKind kind, const std::string& key, Tick at,
                                      const std::string& authority, IdempotencyKey* out_key) {
  const ControlCommand control = command(kind, key, at, authority);
  if (out_key != nullptr) {
    *out_key = control.key;
  }
  return engine->submit(control);
}

Result<AttemptRecord> Fixture::verify(AttemptId attempt, Tick at) {
  VerifyRequest request;
  request.authority = context;
  request.now = at;
  request.attempt = attempt;
  return engine->verify(request);
}

Result<AttemptRecord> Fixture::abandon(AttemptId attempt, Tick at, const std::string& reason) {
  AbandonRequest request;
  request.authority = context;
  request.now = at;
  request.attempt = attempt;
  request.reason = reason;
  return engine->abandon(request);
}

std::size_t count_lines(const std::filesystem::path& path) {
  return uc_test::read_lines(path).size();
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  stream.flush();
}

bool file_ready(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return !error && size > 0;
}

bool run_probe(const std::vector<std::string>& arguments, int& exit_code, std::string& error) {
  return uc_test::spawn_wait(probe_path(), arguments, exit_code, error);
}

bool spawn_probe(const std::vector<std::string>& arguments, std::uint64_t& pid, std::string& error) {
  (void)pid;
  return uc_test::spawn_nowait(probe_path(), arguments, error);
}

}  // namespace uc_test
