#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ups_control {

/// Stable machine-readable outcome codes.
///
/// The numeric values are part of the public contract and are never reused. The
/// textual tokens returned by \c to_string are also stable and are safe to match
/// on. \c StatusCode describes why a call failed; the modeled control refusal
/// vocabulary lives in \c RefusalCode and is mapped onto these codes by
/// \c status_code_of.
enum class StatusCode : std::int32_t {
  Ok = 0,
  /// A caller-supplied argument is malformed, out of range, or self-inconsistent.
  InvalidArgument = 1,
  /// A referenced identity does not exist in the authoritative state.
  NotFound = 2,
  /// A create-only mutation found the identity already present.
  AlreadyExists = 3,
  /// The same identity was declared twice inside one input.
  DuplicateIdentity = 4,
  /// The operation conflicts with another authoritative fact or binding.
  Conflict = 5,
  /// An explicit precondition supplied by the caller does not hold.
  PreconditionFailed = 6,
  /// The caller cited a store generation that is no longer current.
  StaleGeneration = 7,
  /// The caller cited a control epoch, controller incarnation, or session that is
  /// no longer authoritative.
  StaleAuthority = 8,
  /// The caller cited a source revision (telemetry, obligation, grant, policy)
  /// that has since advanced.
  StaleSourceGeneration = 9,
  /// The artifact is a recognizable but unsupported format version.
  IncompatibleVersion = 10,
  /// The artifact failed an integrity or structural check.
  Corruption = 11,
  /// A declared or observed size exceeds a configured bound.
  LimitExceeded = 12,
  /// The capability is deliberately outside this runtime's systems boundary, or
  /// the adapter does not implement it. Never a silent remap to a nearby
  /// operation.
  Unsupported = 13,
  /// The referenced object exists but cannot serve its function now.
  Unavailable = 14,
  /// The value cannot be determined from the available authority or evidence.
  Unknown = 15,
  /// Evidence exists but is contradictory or incomplete in a way that leaves the
  /// value indeterminate; it must not be treated as zero or as a known value.
  Indeterminate = 16,
  /// The operating system refused access to a required resource.
  PermissionDenied = 17,
  /// A file, directory, or device operation failed.
  IoFailure = 18,
  /// Another process or engine instance holds the writer authority.
  LockConflict = 19,
  /// An internal consistency guarantee of the UPS control model was violated.
  InvariantViolation = 20,
  /// A control request was refused by deterministic transition evaluation. The
  /// primary reason is in the accompanying \c RefusalCode.
  Refused = 21,
  /// The requested lifecycle or operating transition is not a legal edge of the
  /// documented transition graph.
  IllegalTransition = 22,
  /// A protected-load obligation that must remain protected would be dropped.
  ObligationUnreleased = 23,
  /// Battery reserve evidence does not satisfy the required floor.
  ReserveInsufficient = 24,
  /// Battery reserve evidence exists but cannot decide the precondition.
  ReserveIndeterminate = 25,
  /// No evidence record was supplied for a quantity that requires one.
  EvidenceMissing = 26,
  /// The evidence record is present but outside its freshness window at the
  /// requested instant.
  EvidenceStale = 27,
  /// Recovered state has not been revalidated against the current instant, so it
  /// may not be used to authorize control.
  NotRevalidated = 28,
  /// The engine, store, or handle has been closed.
  Closed = 29,
  /// The artifact encodes multi-byte integers in the opposite byte order.
  EndianMismatch = 30,
  /// A mutating operation was refused because the handle is read-only.
  ReadOnly = 31,
  /// No attempt with that identifier exists in the journal.
  AttemptNotFound = 32,
  /// The attempt exists but is not in a phase from which the requested stage may
  /// be applied.
  AttemptStateConflict = 33,
  /// The idempotency key was already used for a different request.
  IdempotencyConflict = 34,
  /// Another unresolved control attempt holds the single-device ordering gate.
  TransitionInProgress = 35,
  /// The adapter reports that it does not implement the requested operation.
  CapabilityUnsupported = 36,
  /// The adapter failed or returned a malformed acknowledgement.
  AdapterFailure = 37,
  /// A declared authority window or grant has expired at the requested instant.
  AuthorityExpired = 38,
  /// No authority covers the requested operation.
  AuthorityMissing = 39,
  /// An authority explicitly denies the requested operation.
  AuthorityDenied = 40,
};

/// Stable textual token for a status code, using lower_snake_case.
const char* to_string(StatusCode code) noexcept;

/// A status value: \c Ok or a typed error with a human-readable detail message.
class Status {
 public:
  Status() noexcept = default;

  static Status success() noexcept { return Status{}; }
  static Status error(StatusCode code, std::string message);

  bool ok() const noexcept { return code_ == StatusCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }
  StatusCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  /// \c token or \c token\ :\ message.
  std::string to_string() const;

 private:
  StatusCode code_ = StatusCode::Ok;
  std::string message_;
};

/// A value or a typed error. Constructing from a non-Ok \c Status yields an error
/// result; the value is never observable in that case.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }
  const Status& status() const noexcept { return status_; }

  const T& value() const {
    if (!status_.ok()) {
      throw std::logic_error("ups_control::Result::value() on error: " + status_.to_string());
    }
    return *value_;
  }
  T& value() {
    if (!status_.ok()) {
      throw std::logic_error("ups_control::Result::value() on error: " + status_.to_string());
    }
    return *value_;
  }
  const T& operator*() const { return value(); }
  T& operator*() { return value(); }
  const T* operator->() const { return &value(); }
  T* operator->() { return &value(); }

 private:
  std::optional<T> value_{};
  Status status_{};
};

/// Renders a value that has a \c to_string overload, for diagnostics.
template <typename T>
std::string describe(const T& value) {
  return std::string(to_string(value));
}

}  // namespace ups_control
