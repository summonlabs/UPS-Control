#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ups_control/command.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/operating.hpp"
#include "ups_control/status.hpp"

namespace ups_control {

/// What one adapter reports about itself.
struct AdapterCapabilities {
  AdapterId id;
  /// One bit per \c CommandKind, at position \c static_cast<unsigned>(kind).
  std::uint32_t supported_commands = 0;
  /// True when the adapter can report bypass availability and qualification.
  bool reports_transfer_status = false;
  /// True when the adapter can report battery reserve evidence.
  bool reports_battery_evidence = false;
  EvidenceClass evidence = EvidenceClass::Unsupported;

  bool supports(CommandKind kind) const noexcept;
};

/// The plan handed to an adapter. It carries the operation and the exact device
/// generation it targets, and nothing else: an adapter never sees the store, the
/// engine state, or another device's data.
struct AdapterPlan {
  UpsId ups;
  HardwareGeneration hardware;
  AttemptId attempt;
  CommandKind command = CommandKind::EnterStaticBypass;
  Tick issued_at;
};

/// What the adapter reported about one issued command.
struct AdapterIssueResult {
  AckOutcome outcome = AckOutcome::NoResponse;
  std::string detail;

  friend bool operator==(const AdapterIssueResult&, const AdapterIssueResult&) = default;
};

/// The only boundary through which this runtime can affect a device.
///
/// Implementations must not block indefinitely and must not perform hidden
/// autonomous control. A deployment that binds a real device adapter labels its
/// attempts \c Real; the deterministic simulator below labels them \c Synthetic.
class UpsAdapter {
 public:
  virtual ~UpsAdapter() = default;

  virtual AdapterCapabilities capabilities() const = 0;

  /// Issues one command. The engine holds no internal lock across this call and
  /// does not interpret a result as an effect: the result is only an
  /// acknowledgement, and the effective state is established separately from
  /// telemetry.
  virtual AdapterIssueResult issue(const AdapterPlan& plan) = 0;
};

/// A deterministic, scripted adapter. No hardware is involved.
///
/// The script maps a command to the acknowledgement the simulated device returns.
/// When an effects log is configured, every issuance appends one line before the
/// acknowledgement is returned, so an independent observer can prove whether a
/// command reached the adapter at all.
class SyntheticAdapter final : public UpsAdapter {
 public:
  struct Script {
    /// The command set the simulated device implements. Every other command is
    /// reported as unsupported, explicitly and without substitution.
    std::uint32_t supported_commands = 0xFFFFFFFFu;
    /// Acknowledgement returned per command. A command absent from the map is
    /// acknowledged as accepted.
    std::map<CommandKind, AckOutcome> acknowledgements;
    /// Optional append-only record of every issuance.
    std::filesystem::path effects_log;
    /// Detail recorded with the acknowledgement, bounded.
    std::string detail;
  };

  explicit SyntheticAdapter(Script script);

  AdapterCapabilities capabilities() const override;
  AdapterIssueResult issue(const AdapterPlan& plan) override;

  /// Number of commands this instance has issued.
  std::uint64_t issue_count() const noexcept { return issue_count_; }

 private:
  Script script_;
  std::uint64_t issue_count_ = 0;
};

/// Builds the mask of every documented command kind.
std::uint32_t all_command_kinds_mask() noexcept;

/// The bit position used for a command kind.
std::uint32_t command_kind_bit(CommandKind kind) noexcept;

}  // namespace ups_control
