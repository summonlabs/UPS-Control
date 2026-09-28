#include "test_harness.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <ostream>
#include <string>

namespace ups_control {

std::ostream& operator<<(std::ostream& stream, StatusCode value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, RefusalCode value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ProtectionImpact value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ObligationTier value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ProtectionRequirement value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ObligationState value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, GrantScope value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, GrantVerdict value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ReserveOutcome value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ReserveIndeterminacy value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ReserveUnit value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, PowerUnit value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ReserveState value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, UnknownReason value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, EvidenceQuality value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, Provenance value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, BatteryActivity value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, Bool3 value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, FreshnessVerdict value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, LifecycleState value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, OperatingState value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, StateBasis value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, BypassKind value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, CommandKind value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, CommandFamily value) {
  return stream << (value == CommandFamily::Transition ? "transition" : "capability");
}
std::ostream& operator<<(std::ostream& stream, AttemptPhase value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, AckOutcome value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ObservedOutcome value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, VerificationVerdict value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, EvidenceClass value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, EngineLifecycle value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, OperationKind value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, DurableCrashPoint value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, const Status& value) {
  return stream << value.to_string();
}
std::ostream& operator<<(std::ostream& stream, const ReserveQuantity& value) {
  return stream << format_reserve(value);
}
std::ostream& operator<<(std::ostream& stream, const ReserveAssessment& value) {
  return stream << "reserve(" << to_string(value.outcome) << ", " << to_string(value.reason) << ", "
                << value.detail << ")";
}
std::ostream& operator<<(std::ostream& stream, const ProtectedImpactReport& value) {
  return stream << "impact(" << to_string(value.impact) << ", at_risk=" << value.at_risk.size()
                << ", unreleased=" << value.unreleased.size() << ")";
}
std::ostream& operator<<(std::ostream& stream, const EvaluationReport& value) {
  return stream << "evaluation(" << (value.allowed ? "allowed" : "refused") << ", "
                << to_string(value.primary) << ")";
}
std::ostream& operator<<(std::ostream& stream, const UpsId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const LoadId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const ObligationRef& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const EvidenceId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const SourceId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const AuthorityRef& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const IdempotencyKey& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const AdapterId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, StoreGeneration value) {
  return stream << "generation(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, HardwareGeneration value) {
  return stream << "hardware(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, ControlEpoch value) {
  return stream << "epoch(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, Incarnation value) {
  return stream << "incarnation(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, StateRevision value) {
  return stream << "revision(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, SourceRevision value) {
  return stream << "source_revision(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, ObligationRevision value) {
  return stream << "obligation_revision(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, GrantRevision value) {
  return stream << "grant_revision(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, AttemptId value) {
  return stream << "attempt(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, Tick value) {
  return stream << "tick(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, TickSpan value) {
  return stream << "span(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, const StoreIdentity& value) {
  return stream << value.to_hex();
}
std::ostream& operator<<(std::ostream& stream, const SessionId& value) {
  return stream << value.to_hex();
}

}  // namespace ups_control

namespace uc_test {
namespace {

struct RunState {
  std::size_t checks_failed = 0;
  std::vector<std::string> failures;
  std::string current;
};

RunState& state() {
  static RunState instance;
  return instance;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> instance;
  return instance;
}

Registrar::Registrar(const char* suite, const char* name, Body body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

void report_failure(const char* file, int line, const std::string& expression,
                    const std::string& detail) {
  RunState& run = state();
  ++run.checks_failed;
  std::string message = std::string(file) + ":" + std::to_string(line) + ": " + expression;
  if (!detail.empty()) {
    message += " [" + detail + "]";
  }
  run.failures.push_back(message);
}

std::string display(const std::string& value) { return "\"" + value + "\""; }
std::string display(const char* value) { return std::string("\"") + value + "\""; }
std::string display(bool value) { return value ? "true" : "false"; }
std::string display(std::nullptr_t) { return "nullptr"; }

int run_all(int argc, char** argv) {
  std::string filter;
  if (argc > 1) {
    filter = argv[1];
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : registry()) {
    const std::string full_name = test.suite + "." + test.name;
    if (!filter.empty() && full_name.find(filter) == std::string::npos) {
      continue;
    }
    const std::size_t before = state().checks_failed;
    const std::size_t failures_before = state().failures.size();
    state().current = full_name;
    try {
      test.body();
    } catch (const std::exception& error) {
      report_failure("<test body>", 0, "threw std::exception", error.what());
    } catch (...) {
      report_failure("<test body>", 0, "threw a non-standard exception", "");
    }
    const bool ok = state().checks_failed == before;
    if (ok) {
      ++passed;
      std::cout << "[  PASS  ] " << full_name << "\n";
    } else {
      ++failed;
      std::cout << "[  FAIL  ] " << full_name << "\n";
      for (std::size_t index = failures_before; index < state().failures.size(); ++index) {
        std::cout << "           " << state().failures[index] << "\n";
      }
    }
    std::cout.flush();
  }
  std::cout << "\n" << passed << " passed, " << failed << " failed\n";
  std::cout.flush();
  return failed == 0 ? 0 : 1;
}

}  // namespace uc_test

int main(int argc, char** argv) { return uc_test::run_all(argc, argv); }
