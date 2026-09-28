#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/status.hpp"
#include "ups_control/units.hpp"

namespace ups_control {

/// What an authority grant covers.
enum class GrantScope : std::uint8_t {
  /// Enter the static bypass path.
  BypassTransfer = 1,
  /// Enter the maintenance bypass path, which removes UPS protection.
  MaintenanceEntry = 2,
  /// Run a self test or a battery test.
  TestExecution = 3,
  /// Enable the recharge path.
  RechargeEnable = 4,
  /// Enable the discharge path.
  DischargeEnable = 5,
  /// Isolate or restore the output.
  IsolationControl = 6,
  /// Clear device faults.
  FaultClear = 7,
  /// Change the reserve policy.
  PolicyChange = 8,
};

const char* to_string(GrantScope scope) noexcept;
Result<GrantScope> parse_grant_scope(std::string_view text);

/// One authority grant consumed from an upstream authority.
///
/// A grant is policy input, never telemetry: no amount of telemetry creates,
/// widens, or renews one. A grant that explicitly denies an operation is recorded
/// so that a denial is distinguishable from a missing authority.
struct AuthorityGrant {
  AuthorityRef ref;
  GrantScope scope = GrantScope::BypassTransfer;
  AuthorityRef granted_by;
  ControlEpoch epoch;
  Incarnation incarnation;
  Tick issued_at;
  /// Absent means the granting authority declared no expiry.
  std::optional<Tick> expires_at;
  /// False records an explicit denial covering this scope.
  bool allowed = true;
  /// Optional floor that the granting authority requires for a discharge enable.
  std::optional<ReserveQuantity> reserve_floor;
  std::optional<AuthorityRef> revoked_by;
  std::optional<Tick> revoked_at;
  std::string note;
  GrantRevision revision;

  friend bool operator==(const AuthorityGrant&, const AuthorityGrant&) = default;
};

Status validate_grant(const AuthorityGrant& grant, const ResourceLimits& limits);

/// The outcome of assessing authority for one operation.
enum class GrantVerdict : std::uint8_t {
  Allowed = 1,
  /// An explicit denial covers the scope.
  Denied = 2,
  /// No grant covers the scope.
  Missing = 3,
  /// The grant's validity window has lapsed.
  Expired = 4,
  /// The grant was revoked.
  Revoked = 5,
  /// The grant was issued under a superseded epoch or incarnation.
  Fenced = 6,
  /// The grant exists but covers a different scope, so it does not authorize this
  /// operation. A neighbouring scope is never substituted.
  OutOfScope = 7,
};

const char* to_string(GrantVerdict verdict) noexcept;

/// An explainable authority assessment.
struct GrantAssessment {
  GrantVerdict verdict = GrantVerdict::Missing;
  std::optional<AuthorityRef> grant;
  std::string detail;

  friend bool operator==(const GrantAssessment&, const GrantAssessment&) = default;
};

/// Assesses the grant cited by a request.
///
/// Precedence is fixed: a cited grant that does not exist is \c Missing; a grant
/// whose recorded scope differs from the requested scope is \c OutOfScope and is
/// never substituted; a revoked grant is \c Revoked; a grant issued under a
/// superseded epoch or incarnation is \c Fenced; a lapsed grant is \c Expired;
/// an explicit denial is \c Denied; otherwise the grant must allow the scope.
GrantAssessment assess_grant(const std::vector<AuthorityGrant>& grants,
                             const std::optional<AuthorityRef>& cited, GrantScope scope,
                             const ControlContext& context, Tick now);

}  // namespace ups_control
