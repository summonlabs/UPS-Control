#pragma once

// Shared test fixtures. These helpers build real stores, real telemetry, and real
// engine handles through the public API. Nothing here reimplements a model rule:
// a helper that needs a decision asks the library for it.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ups_control/engine.hpp"

namespace uc_test {

/// A scratch directory removed when the object is destroyed. The name carries the
/// test name and the process identifier so that two test executables running at
/// the same time never share a directory.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& name);
  ~ScratchDirectory();
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path file(const std::string& name) const;

 private:
  std::filesystem::path path_;
};

/// A standard healthy telemetry report.
ups_control::TelemetryReport healthy_report(const ups_control::UpsId& ups,
                                            ups_control::HardwareGeneration hardware,
                                            ups_control::Tick at,
                                            ups_control::SourceRevision revision,
                                            const std::string& evidence);

/// A ready-to-use engine bound to a fresh scratch store, already registered,
/// observed, and granted.
struct Fixture {
  explicit Fixture(const std::string& test_name);

  ups_control::Tick now{1000};
  ups_control::ControlContext context{ups_control::ControlEpoch{1}, ups_control::Incarnation{1}};
  ups_control::SyntheticAdapter::Script script;
  std::shared_ptr<ups_control::SyntheticAdapter> adapter;
  ups_control::StoreOpenOptions store_options;
  ups_control::EngineOpenOptions open_options;
  std::shared_ptr<ups_control::UpsControlEngine> engine;
  std::unique_ptr<ScratchDirectory> scratch;
  std::string last_error;

  /// Creates the store, revalidates it, registers one unit, records one healthy
  /// observation, and issues a grant for the given scope. Returns false and fills
  /// last_error when any of that fails.
  bool build(ups_control::GrantScope scope = ups_control::GrantScope::BypassTransfer,
             ups_control::ReserveUnit floor_unit = ups_control::ReserveUnit::Seconds,
             std::int64_t floor_value = 600);

  ups_control::Status revalidate(ups_control::Tick at);
  ups_control::Result<ups_control::UpsRecord> current();
  ups_control::Result<ups_control::ObservationRecord> observe(
      const ups_control::TelemetryReport& report, ups_control::Tick at);
  ups_control::Result<ups_control::AttemptRecord> submit(
      ups_control::CommandKind kind, const std::string& key, ups_control::Tick at,
      const std::string& authority, ups_control::IdempotencyKey* out_key = nullptr);
  ups_control::Result<ups_control::AttemptRecord> verify(ups_control::AttemptId attempt,
                                                         ups_control::Tick at);
  ups_control::Result<ups_control::AttemptRecord> abandon(ups_control::AttemptId attempt,
                                                          ups_control::Tick at,
                                                          const std::string& reason = "test");
  /// The control command a fixture-based test would submit, without submitting it.
  ups_control::ControlCommand command(ups_control::CommandKind kind, const std::string& key,
                                      ups_control::Tick at, const std::string& authority);
};

/// Counts the lines of a file, or zero when it does not exist. The line reader
/// itself lives in proc.hpp, next to the process helpers that use it.
std::size_t count_lines(const std::filesystem::path& path);

/// Writes text to a file, creating it when needed.
void write_text(const std::filesystem::path& path, const std::string& text);

/// True when the file exists and is non-empty.
bool file_ready(const std::filesystem::path& path);

/// Spawns the probe and waits for it. Returns its exit code through exit_code.
bool run_probe(const std::vector<std::string>& arguments, int& exit_code, std::string& error);

/// Spawns the probe without waiting, returning its process identifier.
bool spawn_probe(const std::vector<std::string>& arguments, std::uint64_t& pid, std::string& error);

}  // namespace uc_test
