#pragma once

// Minimal deterministic test harness. No third-party dependency, no timeouts, no
// watchdog logic: a check either passes, fails, or the program does not finish,
// and an unfinished program is a defect to diagnose.

#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#include "ups_control/command.hpp"
#include "ups_control/engine.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/obligation.hpp"
#include "ups_control/state.hpp"
#include "ups_control/status.hpp"
#include "ups_control/units.hpp"

namespace ups_control {

// Display helpers so that a failing check prints a readable value. These exist
// only for test diagnostics and are not part of the library.
std::ostream& operator<<(std::ostream& stream, StatusCode value);
std::ostream& operator<<(std::ostream& stream, RefusalCode value);
std::ostream& operator<<(std::ostream& stream, ProtectionImpact value);
std::ostream& operator<<(std::ostream& stream, ObligationTier value);
std::ostream& operator<<(std::ostream& stream, ProtectionRequirement value);
std::ostream& operator<<(std::ostream& stream, ObligationState value);
std::ostream& operator<<(std::ostream& stream, GrantScope value);
std::ostream& operator<<(std::ostream& stream, GrantVerdict value);
std::ostream& operator<<(std::ostream& stream, ReserveOutcome value);
std::ostream& operator<<(std::ostream& stream, ReserveIndeterminacy value);
std::ostream& operator<<(std::ostream& stream, ReserveUnit value);
std::ostream& operator<<(std::ostream& stream, PowerUnit value);
std::ostream& operator<<(std::ostream& stream, ReserveState value);
std::ostream& operator<<(std::ostream& stream, UnknownReason value);
std::ostream& operator<<(std::ostream& stream, EvidenceQuality value);
std::ostream& operator<<(std::ostream& stream, Provenance value);
std::ostream& operator<<(std::ostream& stream, BatteryActivity value);
std::ostream& operator<<(std::ostream& stream, Bool3 value);
std::ostream& operator<<(std::ostream& stream, FreshnessVerdict value);
std::ostream& operator<<(std::ostream& stream, LifecycleState value);
std::ostream& operator<<(std::ostream& stream, OperatingState value);
std::ostream& operator<<(std::ostream& stream, StateBasis value);
std::ostream& operator<<(std::ostream& stream, BypassKind value);
std::ostream& operator<<(std::ostream& stream, CommandKind value);
std::ostream& operator<<(std::ostream& stream, CommandFamily value);
std::ostream& operator<<(std::ostream& stream, AttemptPhase value);
std::ostream& operator<<(std::ostream& stream, AckOutcome value);
std::ostream& operator<<(std::ostream& stream, ObservedOutcome value);
std::ostream& operator<<(std::ostream& stream, VerificationVerdict value);
std::ostream& operator<<(std::ostream& stream, EvidenceClass value);
std::ostream& operator<<(std::ostream& stream, EngineLifecycle value);
std::ostream& operator<<(std::ostream& stream, OperationKind value);
std::ostream& operator<<(std::ostream& stream, DurableCrashPoint value);
std::ostream& operator<<(std::ostream& stream, const Status& value);
std::ostream& operator<<(std::ostream& stream, const ReserveQuantity& value);
std::ostream& operator<<(std::ostream& stream, const ReserveAssessment& value);
std::ostream& operator<<(std::ostream& stream, const ProtectedImpactReport& value);
std::ostream& operator<<(std::ostream& stream, const EvaluationReport& value);
std::ostream& operator<<(std::ostream& stream, const UpsId& value);
std::ostream& operator<<(std::ostream& stream, const LoadId& value);
std::ostream& operator<<(std::ostream& stream, const ObligationRef& value);
std::ostream& operator<<(std::ostream& stream, const EvidenceId& value);
std::ostream& operator<<(std::ostream& stream, const SourceId& value);
std::ostream& operator<<(std::ostream& stream, const AuthorityRef& value);
std::ostream& operator<<(std::ostream& stream, const IdempotencyKey& value);
std::ostream& operator<<(std::ostream& stream, const AdapterId& value);
std::ostream& operator<<(std::ostream& stream, StoreGeneration value);
std::ostream& operator<<(std::ostream& stream, HardwareGeneration value);
std::ostream& operator<<(std::ostream& stream, ControlEpoch value);
std::ostream& operator<<(std::ostream& stream, Incarnation value);
std::ostream& operator<<(std::ostream& stream, StateRevision value);
std::ostream& operator<<(std::ostream& stream, SourceRevision value);
std::ostream& operator<<(std::ostream& stream, ObligationRevision value);
std::ostream& operator<<(std::ostream& stream, GrantRevision value);
std::ostream& operator<<(std::ostream& stream, AttemptId value);
std::ostream& operator<<(std::ostream& stream, Tick value);
std::ostream& operator<<(std::ostream& stream, TickSpan value);
std::ostream& operator<<(std::ostream& stream, const StoreIdentity& value);
std::ostream& operator<<(std::ostream& stream, const SessionId& value);

}  // namespace ups_control

namespace uc_test {

using Body = std::function<void()>;

struct TestCase {
  std::string suite;
  std::string name;
  Body body;
};

std::vector<TestCase>& registry();

class Registrar {
 public:
  Registrar(const char* suite, const char* name, Body body);
};

/// Records a failure for the currently running test.
void report_failure(const char* file, int line, const std::string& expression,
                    const std::string& detail);

/// Renders a value for a failure message. Types without a stream operator are
/// named by their RTTI name rather than silently omitted.
template <typename T>
std::string display(const T& value) {
  if constexpr (requires(std::ostream& stream, const T& item) { stream << item; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return std::string("<value of type ") + typeid(T).name() + ">";
  }
}

std::string display(const std::string& value);
std::string display(const char* value);
std::string display(bool value);
std::string display(std::nullptr_t value);

/// Extracts the status of either a \c Status or a \c Result<T>, so that a check
/// can be written the same way for both. Returns by value: returning a reference
/// would dangle as soon as the temporary \c Result<T> in the caller's full
/// expression is destroyed.
inline ::ups_control::Status status_of(const ::ups_control::Status& value) { return value; }

template <typename T>
::ups_control::Status status_of(const ::ups_control::Result<T>& value) {
  return value.status();
}

int run_all(int argc, char** argv);

}  // namespace uc_test

#define UC_TEST(suite, name)                                                          \
  static void uc_test_body_##suite##_##name();                                        \
  static const ::uc_test::Registrar uc_test_registrar_##suite##_##name(               \
      #suite, #name, uc_test_body_##suite##_##name);                                  \
  static void uc_test_body_##suite##_##name()

#define UC_CHECK(expression)                                                       \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::uc_test::report_failure(__FILE__, __LINE__, #expression, "");              \
    }                                                                              \
  } while (false)

#define UC_CHECK_MSG(expression, detail)                                           \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::uc_test::report_failure(__FILE__, __LINE__, #expression, (detail));        \
    }                                                                              \
  } while (false)

#define UC_CHECK_EQ(left, right)                                                   \
  do {                                                                             \
    const auto& uc_left_value = (left);                                            \
    const auto& uc_right_value = (right);                                          \
    if (!(uc_left_value == uc_right_value)) {                                      \
      ::uc_test::report_failure(__FILE__, __LINE__, #left " == " #right,           \
                                "left=" + ::uc_test::display(uc_left_value) +      \
                                    " right=" + ::uc_test::display(uc_right_value)); \
    }                                                                              \
  } while (false)

#define UC_CHECK_NE(left, right)                                                   \
  do {                                                                             \
    const auto& uc_left_value = (left);                                            \
    const auto& uc_right_value = (right);                                          \
    if (uc_left_value == uc_right_value) {                                         \
      ::uc_test::report_failure(__FILE__, __LINE__, #left " != " #right,           \
                                "both=" + ::uc_test::display(uc_left_value));      \
    }                                                                              \
  } while (false)

/// Requires the result to hold and returns its value.
#define UC_REQUIRE_OK(expression)                                                       \
  ([&]() {                                                                              \
    auto uc_result_value = (expression);                                                \
    if (!uc_result_value.ok()) {                                                        \
      ::uc_test::report_failure(__FILE__, __LINE__, #expression " is ok",               \
                                "status=" + ::uc_test::display(uc_result_value.status())); \
    }                                                                                   \
    return uc_result_value.value();                                                     \
  }())

/// Requires the status to fail with exactly the given code. Accepts either a
/// \c Status or a \c Result<T>.
#define UC_REQUIRE_STATUS(expression, expected_code)                                       \
  do {                                                                                     \
    const ::ups_control::Status uc_status_value = ::uc_test::status_of(expression);         \
    if (uc_status_value.code() != (expected_code)) {                                        \
      ::uc_test::report_failure(__FILE__, __LINE__, #expression " fails with " #expected_code, \
                                "actual=" + ::uc_test::display(uc_status_value));          \
    }                                                                                      \
  } while (false)

/// Requires a boolean condition and records a failure otherwise. Use this when the
/// rest of the test body cannot proceed without the fact.
#define UC_REQUIRE(expression)                                                     \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::uc_test::report_failure(__FILE__, __LINE__, #expression, "required");       \
    }                                                                              \
  } while (false)
