#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ups_control/command.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/status.hpp"
#include "ups_control/ups.hpp"

namespace ups_control {

/// What one durable commit changed.
enum class OperationKind : std::uint8_t {
  None = 0,
  StoreCreated = 1,
  UpsRegistered = 2,
  TelemetryRecorded = 3,
  OperatingStateAdopted = 4,
  LifecycleChanged = 5,
  ObligationBound = 6,
  ObligationReleased = 7,
  GrantIssued = 8,
  GrantRevoked = 9,
  PolicyChanged = 10,
  AuthorityAdopted = 11,
  AttemptPlanned = 12,
  AttemptAcknowledged = 13,
  AttemptObserved = 14,
  AttemptVerified = 15,
  AttemptFailed = 16,
  Revalidated = 17,
};

const char* to_string(OperationKind kind) noexcept;

/// One durable commit, retained for audit and for bounded idempotency.
struct CommitLogEntry {
  StoreGeneration generation;
  AttemptId attempt;
  IdempotencyKey key;
  OperationKind operation = OperationKind::None;
  UpsId ups;
  StateRevision revision;
  Tick at;

  friend bool operator==(const CommitLogEntry&, const CommitLogEntry&) = default;
};

/// A retained idempotency binding.
///
/// Retention is bounded by \c ResourceLimits::max_idempotency_records; the oldest
/// binding is evicted first. An evicted key is no longer recognized, so a retry
/// carrying it is treated as a new request and is then refused by the ordinary
/// staleness checks rather than being silently re-applied. The bound and the
/// eviction order are part of the documented contract.
struct IdempotencyEntry {
  IdempotencyKey key;
  AttemptId attempt;
  std::uint64_t plan_digest = 0;
  Tick recorded_at;

  friend bool operator==(const IdempotencyEntry&, const IdempotencyEntry&) = default;
};

/// The complete authoritative state of one store.
struct UpsState {
  StoreGeneration generation;
  ControlEpoch epoch;
  Incarnation incarnation;
  Tick created_at;
  Tick updated_at;
  /// The instant of the most recent successful revalidation, or \c Tick{0} when
  /// the state has not been revalidated since it was loaded.
  Tick revalidated_at;

  /// UPS records sorted by identifier.
  std::vector<UpsRecord> units;
  /// Attempt journal ordered by attempt identifier, oldest first, bounded.
  std::vector<AttemptRecord> attempts;
  /// Idempotency bindings, oldest first, bounded.
  std::vector<IdempotencyEntry> idempotency;
  /// Commit log, oldest first, bounded.
  std::vector<CommitLogEntry> commit_log;
  AttemptId last_attempt;
  std::uint64_t operation_count = 0;

  friend bool operator==(const UpsState&, const UpsState&) = default;
};

const UpsRecord* find_ups(const UpsState& state, const UpsId& id) noexcept;
UpsRecord* find_ups(UpsState& state, const UpsId& id) noexcept;
const AttemptRecord* find_attempt(const UpsState& state, AttemptId id) noexcept;
AttemptRecord* find_attempt(UpsState& state, AttemptId id) noexcept;
const IdempotencyEntry* find_idempotency(const UpsState& state, const IdempotencyKey& key) noexcept;

/// Deterministic insertion into a sorted vector, refusing a duplicate identity.
Status insert_sorted_by_id(std::vector<UpsRecord>& units, UpsRecord record,
                           const ResourceLimits& limits);

/// Validates the complete state structurally: bounds, sortedness, uniqueness,
/// revision monotonicity, cross-references, and internal consistency.
Status validate_state(const UpsState& state, const ResourceLimits& limits);

/// Encodes the logical state canonically.
///
/// The encoding contains the logical state only: no store identity, no recorded
/// path, no wall clock, no process identifier, and no pointer value. Two states
/// that differ only in those values therefore produce identical bytes, and
/// equivalent logical states always produce equivalent bytes.
Result<std::vector<std::byte>> encode_canonical_state(const UpsState& state,
                                                      const ResourceLimits& limits);

/// The deterministic digest of the canonical logical state.
std::uint64_t canonical_state_digest(const UpsState& state);

}  // namespace ups_control
