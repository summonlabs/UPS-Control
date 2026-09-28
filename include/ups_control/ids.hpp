#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ups_control/status.hpp"

namespace ups_control {

/// Identity families. Each family is a distinct C++ type; there is no implicit
/// conversion between them, and no implicit conversion to or from the underlying
/// string.
enum class IdFamily {
  Ups,
  Load,
  Obligation,
  Evidence,
  Source,
  Authority,
  Idempotency,
  Adapter,
};

/// Validates the textual form of an identifier.
///
/// Accepted: 1..\c max_bytes bytes drawn from \c A-Z a-z 0-9 . _ - : @ .
/// Rejected: empty, over-long, NUL, whitespace, control characters, path
/// separators, non-ASCII bytes, and every other punctuation character.
Status validate_identifier(std::string_view text, std::size_t max_bytes);

/// Validates free-text metadata such as a UPS label. Accepted: up to
/// \c max_bytes bytes of UTF-8 without NUL, C0 controls, or DEL.
Status validate_label(std::string_view text, std::size_t max_bytes);

/// A strongly typed identifier.
template <IdFamily Family>
class BasicId {
 public:
  BasicId() = default;

  static Result<BasicId> parse(std::string_view text, std::size_t max_bytes = 128) {
    const Status status = validate_identifier(text, max_bytes);
    if (!status.ok()) {
      return status;
    }
    BasicId id;
    id.value_.assign(text);
    return id;
  }

  bool empty() const noexcept { return value_.empty(); }
  const std::string& value() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }

  friend bool operator==(const BasicId&, const BasicId&) = default;
  friend std::strong_ordering operator<=>(const BasicId&, const BasicId&) = default;

 private:
  std::string value_{};
};

using UpsId = BasicId<IdFamily::Ups>;
using LoadId = BasicId<IdFamily::Load>;
using ObligationRef = BasicId<IdFamily::Obligation>;
using EvidenceId = BasicId<IdFamily::Evidence>;
using SourceId = BasicId<IdFamily::Source>;
using AuthorityRef = BasicId<IdFamily::Authority>;
using IdempotencyKey = BasicId<IdFamily::Idempotency>;
using AdapterId = BasicId<IdFamily::Adapter>;

/// A 128-bit opaque fingerprint. Used for store identity and engine session
/// identity. A zero fingerprint means "unset" and is never a valid identity.
enum class FingerprintFamily { StoreIdentity, SessionId };

template <FingerprintFamily Family>
class BasicFingerprint {
 public:
  BasicFingerprint() = default;

  /// Non-deterministic 128-bit identity drawn from the platform entropy source.
  static BasicFingerprint generate();

  /// Parses exactly 32 lowercase hexadecimal characters.
  static Result<BasicFingerprint> parse(std::string_view hex);

  /// Builds a fingerprint from its two halves. Used by the serializer and by the
  /// generator; it performs no validation beyond rejecting the all-zero value.
  static Result<BasicFingerprint> from_components(std::uint64_t high, std::uint64_t low);

  bool is_zero() const noexcept { return high_ == 0 && low_ == 0; }
  std::uint64_t high() const noexcept { return high_; }
  std::uint64_t low() const noexcept { return low_; }
  std::string to_hex() const;

  friend bool operator==(const BasicFingerprint&, const BasicFingerprint&) = default;
  friend std::strong_ordering operator<=>(const BasicFingerprint&, const BasicFingerprint&) = default;

 private:
  std::uint64_t high_ = 0;
  std::uint64_t low_ = 0;
};

using StoreIdentity = BasicFingerprint<FingerprintFamily::StoreIdentity>;
using SessionId = BasicFingerprint<FingerprintFamily::SessionId>;

// ---------------------------------------------------------------------------
// Ordinals
// ---------------------------------------------------------------------------
//
// Each ordinal is a distinct type. A store generation is not a control epoch, an
// epoch is not a controller incarnation, an incarnation is not a per-device state
// revision, and none of them is an attempt identifier or an instant. They are
// deliberately not interchangeable and are never collapsed into a generic
// integer.

struct StoreGenerationTag {};
struct HardwareGenerationTag {};
struct ControlEpochTag {};
struct IncarnationTag {};
struct StateRevisionTag {};
struct SourceRevisionTag {};
struct ObligationRevisionTag {};
struct GrantRevisionTag {};
struct AttemptTag {};
struct TickTag {};
struct TickSpanTag {};

/// A strongly typed monotonically increasing ordinal.
template <typename Tag, typename Rep>
class BasicOrdinal {
 public:
  using rep_type = Rep;

  constexpr BasicOrdinal() = default;
  explicit constexpr BasicOrdinal(Rep value) noexcept : value_(value) {}

  constexpr Rep value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == Rep{0}; }

  /// Convenience arithmetic for constructing instants and durations. Authoritative
  /// calculations inside this runtime use the checked helpers in arith.hpp; these
  /// operators exist so that a caller can write the next logical instant without
  /// reaching for the raw representation, and they are ordinary unchecked integer
  /// arithmetic on the caller's own values.
  friend constexpr BasicOrdinal operator+(BasicOrdinal ordinal, Rep delta) noexcept {
    return BasicOrdinal{static_cast<Rep>(ordinal.value_ + delta)};
  }
  friend constexpr BasicOrdinal operator-(BasicOrdinal ordinal, Rep delta) noexcept {
    return BasicOrdinal{static_cast<Rep>(ordinal.value_ - delta)};
  }
  friend constexpr Rep operator-(BasicOrdinal left, BasicOrdinal right) noexcept {
    return static_cast<Rep>(left.value_ - right.value_);
  }
  constexpr BasicOrdinal& operator+=(Rep delta) noexcept {
    value_ = static_cast<Rep>(value_ + delta);
    return *this;
  }

  friend constexpr bool operator==(BasicOrdinal, BasicOrdinal) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(BasicOrdinal, BasicOrdinal) noexcept = default;

 private:
  Rep value_ = 0;
};

/// Durable store generation. Generation 0 is the empty, never-committed state.
using StoreGeneration = BasicOrdinal<StoreGenerationTag, std::uint64_t>;
/// Vendor-neutral hardware generation of one physical UPS. A device swap or a
/// control-card replacement advances it and fences every request planned against
/// the previous generation.
using HardwareGeneration = BasicOrdinal<HardwareGenerationTag, std::uint32_t>;
/// Consumed control-plane epoch.
using ControlEpoch = BasicOrdinal<ControlEpochTag, std::uint64_t>;
/// Consumed controller incarnation within an epoch.
using Incarnation = BasicOrdinal<IncarnationTag, std::uint64_t>;
/// Monotonic revision of one UPS's authoritative state.
using StateRevision = BasicOrdinal<StateRevisionTag, std::uint64_t>;
/// Source-side sequence number of one telemetry report.
using SourceRevision = BasicOrdinal<SourceRevisionTag, std::uint64_t>;
/// Monotonic revision of one protected-load obligation binding.
using ObligationRevision = BasicOrdinal<ObligationRevisionTag, std::uint64_t>;
/// Monotonic revision of one authority grant.
using GrantRevision = BasicOrdinal<GrantRevisionTag, std::uint64_t>;
/// Control attempt identifier, unique within a store.
using AttemptId = BasicOrdinal<AttemptTag, std::uint64_t>;
/// A logical instant. This runtime never reads a wall clock for an authoritative
/// decision; callers supply the instant explicitly.
using Tick = BasicOrdinal<TickTag, std::int64_t>;
/// A signed duration in ticks.
using TickSpan = BasicOrdinal<TickSpanTag, std::int64_t>;

/// The instant \c Tick{0} is reserved and means "never". Every real observation,
/// request, and authority instant must be strictly positive; negative instants are
/// never valid and are refused wherever they can enter.
constexpr bool is_valid_instant(Tick instant) noexcept { return instant.value() > 0; }

/// The control authority a request was planned against.
struct ControlContext {
  ControlEpoch epoch;
  Incarnation incarnation;

  friend bool operator==(const ControlContext&, const ControlContext&) = default;
};

/// The exact device generation and state revision a request was planned against.
///
/// Carrying this triple is what makes stale authority refusable instead of
/// silently mergeable: an operation that names a superseded device generation or
/// a superseded state revision is refused rather than applied to the newer state.
struct UpsRef {
  UpsId ups;
  HardwareGeneration hardware;
  StateRevision revision;

  friend bool operator==(const UpsRef&, const UpsRef&) = default;
};

}  // namespace ups_control
