#pragma once

// Internal durable-stage crash injection. Not installed.
//
// The store consults this at each durable stage of the commit protocol. When a
// crash point is armed the process is terminated at the operating-system level,
// which is what makes the crash-recovery proof obligations real rather than
// simulated: no destructor, no flush-on-exit, and no error-reporting dialog.

#include <cstdint>

#include "ups_control/store.hpp"

namespace ups_control::detail {

/// The process exit code used by the crash injection. It is deliberately not a
/// value any ordinary failure path produces, so a test can distinguish a real
/// injected crash from an ordinary error exit.
inline constexpr int kCrashExitCode = 0x00C0DE01;

struct CrashInjection {
  DurableCrashPoint point = DurableCrashPoint::None;
  /// The 1-based ordinal of the commit during which the process terminates. Zero
  /// means every commit terminates.
  std::uint64_t after_commits = 0;

  bool armed() const noexcept { return point != DurableCrashPoint::None; }
};

/// Terminates the process when `point` matches the armed stage and the commit
/// ordinal matches. Never returns when it fires.
void maybe_crash(const CrashInjection& injection, DurableCrashPoint point,
                 std::uint64_t commit_ordinal) noexcept;

}  // namespace ups_control::detail
