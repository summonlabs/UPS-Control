#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/status.hpp"

namespace ups_control {

/// The criticality of a protected load as asserted by an external authority.
/// This runtime consumes the assertion; it does not derive criticality.
enum class ObligationTier : std::uint8_t {
  LifeSafety = 1,
  Critical = 2,
  Essential = 3,
  NonEssential = 4,
};

const char* to_string(ObligationTier tier) noexcept;
Result<ObligationTier> parse_obligation_tier(std::string_view text);

/// What the asserting authority requires of this runtime.
enum class ProtectionRequirement : std::uint8_t {
  /// The load must not lose UPS protection while this obligation binds.
  MustRemainProtected = 1,
  /// The load may lose UPS protection when an explicit release authority covers
  /// the change.
  MayBeInterruptedWithAuthority = 2,
  /// Recorded for audit; carries no protection requirement.
  Informational = 3,
};

const char* to_string(ProtectionRequirement requirement) noexcept;
Result<ProtectionRequirement> parse_protection_requirement(std::string_view text);

enum class ObligationState : std::uint8_t {
  Active = 1,
  /// Temporarily not in force, by explicit authority. Carries who suspended it.
  Suspended = 2,
  /// Explicitly released by an authority-bound release.
  Released = 3,
};

const char* to_string(ObligationState state) noexcept;

/// One external authority-bound protected-load obligation.
///
/// The obligation is a durable binding: it names the external load, the authority
/// that asserted it, and the epoch and incarnation that authority was operating
/// under. It is never created, released, or expired implicitly.
struct ProtectedLoadObligation {
  ObligationRef ref;
  LoadId load;
  ObligationTier tier = ObligationTier::Critical;
  ProtectionRequirement protection = ProtectionRequirement::MustRemainProtected;
  AuthorityRef asserted_by;
  ControlEpoch epoch;
  Incarnation incarnation;
  Tick asserted_at;
  /// Absent means the asserting authority declared no expiry.
  std::optional<Tick> expires_at;
  ObligationState state = ObligationState::Active;
  std::optional<AuthorityRef> released_by;
  std::optional<Tick> released_at;
  std::string release_reason;
  ObligationRevision revision;

  friend bool operator==(const ProtectedLoadObligation&, const ProtectedLoadObligation&) = default;
};

Status validate_obligation(const ProtectedLoadObligation& obligation, const ResourceLimits& limits);

/// True when the obligation still asserts protection at \c now: it is Active and
/// either has no expiry or has not yet reached it.
bool obligation_binds(const ProtectedLoadObligation& obligation, Tick now) noexcept;

/// True when the obligation is Active but its declared window has lapsed. Such an
/// obligation is unresolved, not void: it blocks a protection-dropping transition
/// until it is explicitly released or re-asserted.
bool obligation_lapsed(const ProtectedLoadObligation& obligation, Tick now) noexcept;

/// True when this obligation carries a real protection requirement that must be
/// honoured during a protection-dropping transition.
bool obligation_is_protective(const ProtectedLoadObligation& obligation) noexcept;

/// How a transition affects the protection of the bound loads.
enum class ProtectionImpact : std::uint8_t {
  /// The transition leaves every binding obligation protected.
  Preserves = 1,
  /// The transition removes UPS protection from at least one binding
  /// obligation, and every such obligation is explicitly released, suspended, or
  /// covered by a release authority.
  ReducedWithAuthority = 2,
  /// The transition would remove UPS protection from at least one binding
  /// obligation that has not been released.
  DropsUndeclared = 3,
  /// The impact cannot be established from current evidence.
  Unknown = 4,
};

const char* to_string(ProtectionImpact impact) noexcept;

/// A bounded, explainable report of what a transition does to protected loads.
struct ProtectedImpactReport {
  ProtectionImpact impact = ProtectionImpact::Unknown;
  /// Binding protective obligations, in reference order.
  std::vector<ObligationRef> binding;
  /// Binding obligations whose protection would be lost.
  std::vector<ObligationRef> at_risk;
  /// Of those, the ones with no covering release.
  std::vector<ObligationRef> unreleased;
  /// Active but lapsed obligations, which block until released or re-asserted.
  std::vector<ObligationRef> lapsed;

  friend bool operator==(const ProtectedImpactReport&, const ProtectedImpactReport&) = default;
};

/// Computes the protected-load impact of a transition that removes UPS protection
/// from the load.
///
/// \c release_authority is the authority cited by the request for the change. An
/// obligation is covered when it is Released, when it is Suspended by the cited
/// authority, or when it is a \c MayBeInterruptedWithAuthority obligation and the
/// cited authority matches its asserting authority. A \c MustRemainProtected
/// obligation is covered only by an explicit release or suspension, never by a
/// bare authority citation, so the obligation cannot be silently dropped.
ProtectedImpactReport assess_protected_impact(
    const std::vector<ProtectedLoadObligation>& obligations,
    const std::optional<AuthorityRef>& release_authority, Tick now,
    const ResourceLimits& limits);

}  // namespace ups_control
