#include "ups_control/adapter.hpp"

#include <fstream>
#include <string>

namespace ups_control {

bool AdapterCapabilities::supports(CommandKind kind) const noexcept {
  return (supported_commands & command_kind_bit(kind)) != 0;
}

std::uint32_t command_kind_bit(CommandKind kind) noexcept {
  const auto index = static_cast<unsigned>(kind);
  if (index >= 32u) {
    return 0;
  }
  return 1u << index;
}

std::uint32_t all_command_kinds_mask() noexcept {
  std::uint32_t mask = 0;
  for (int index = 1; index <= static_cast<int>(CommandKind::ClearFaults); ++index) {
    mask |= command_kind_bit(static_cast<CommandKind>(index));
  }
  return mask;
}

SyntheticAdapter::SyntheticAdapter(Script script) : script_(std::move(script)) {
  if (script_.supported_commands == 0xFFFFFFFFu) {
    script_.supported_commands = all_command_kinds_mask();
  }
}

AdapterCapabilities SyntheticAdapter::capabilities() const {
  AdapterCapabilities capabilities;
  capabilities.id = AdapterId::parse("synthetic").value();
  capabilities.supported_commands = script_.supported_commands;
  capabilities.reports_transfer_status = true;
  capabilities.reports_battery_evidence = true;
  capabilities.evidence = EvidenceClass::Synthetic;
  return capabilities;
}

AdapterIssueResult SyntheticAdapter::issue(const AdapterPlan& plan) {
  AdapterIssueResult result;
  if (!capabilities().supports(plan.command)) {
    result.outcome = AckOutcome::Unsupported;
    result.detail = "the synthetic device does not implement " + std::string(to_string(plan.command));
    return result;
  }
  ++issue_count_;
  if (!script_.effects_log.empty()) {
    std::ofstream stream(script_.effects_log, std::ios::binary | std::ios::app);
    if (stream) {
      stream << "issue attempt=" << plan.attempt.value() << " ups=" << plan.ups.value()
             << " hardware=" << plan.hardware.value() << " command=" << to_string(plan.command)
             << " at=" << plan.issued_at.value() << "\n";
      stream.flush();
    }
  }
  const auto found = script_.acknowledgements.find(plan.command);
  result.outcome = found == script_.acknowledgements.end() ? AckOutcome::Accepted : found->second;
  result.detail = script_.detail.empty() ? std::string("synthetic adapter acknowledgement")
                                         : script_.detail;
  return result;
}

}  // namespace ups_control
