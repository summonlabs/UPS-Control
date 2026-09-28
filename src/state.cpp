#include "ups_control/state.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "detail/digest.hpp"
#include "detail/serialization.hpp"

namespace ups_control {

const char* to_string(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::None: return "none";
    case OperationKind::StoreCreated: return "store_created";
    case OperationKind::UpsRegistered: return "ups_registered";
    case OperationKind::TelemetryRecorded: return "telemetry_recorded";
    case OperationKind::OperatingStateAdopted: return "operating_state_adopted";
    case OperationKind::LifecycleChanged: return "lifecycle_changed";
    case OperationKind::ObligationBound: return "obligation_bound";
    case OperationKind::ObligationReleased: return "obligation_released";
    case OperationKind::GrantIssued: return "grant_issued";
    case OperationKind::GrantRevoked: return "grant_revoked";
    case OperationKind::PolicyChanged: return "policy_changed";
    case OperationKind::AuthorityAdopted: return "authority_adopted";
    case OperationKind::AttemptPlanned: return "attempt_planned";
    case OperationKind::AttemptAcknowledged: return "attempt_acknowledged";
    case OperationKind::AttemptObserved: return "attempt_observed";
    case OperationKind::AttemptVerified: return "attempt_verified";
    case OperationKind::AttemptFailed: return "attempt_failed";
    case OperationKind::Revalidated: return "revalidated";
  }
  return "unknown";
}

const UpsRecord* find_ups(const UpsState& state, const UpsId& id) noexcept {
  const auto found = std::lower_bound(state.units.begin(), state.units.end(), id,
                                       [](const UpsRecord& unit, const UpsId& value) {
                                         return unit.id < value;
                                       });
  if (found == state.units.end() || !(found->id == id)) {
    return nullptr;
  }
  return &*found;
}

UpsRecord* find_ups(UpsState& state, const UpsId& id) noexcept {
  const auto found = std::lower_bound(state.units.begin(), state.units.end(), id,
                                       [](const UpsRecord& unit, const UpsId& value) {
                                         return unit.id < value;
                                       });
  if (found == state.units.end() || !(found->id == id)) {
    return nullptr;
  }
  return &*found;
}

const AttemptRecord* find_attempt(const UpsState& state, AttemptId id) noexcept {
  const auto found = std::lower_bound(state.attempts.begin(), state.attempts.end(), id,
                                       [](const AttemptRecord& attempt, AttemptId value) {
                                         return attempt.id < value;
                                       });
  if (found == state.attempts.end() || !(found->id == id)) {
    return nullptr;
  }
  return &*found;
}

AttemptRecord* find_attempt(UpsState& state, AttemptId id) noexcept {
  const auto found = std::lower_bound(state.attempts.begin(), state.attempts.end(), id,
                                       [](const AttemptRecord& attempt, AttemptId value) {
                                         return attempt.id < value;
                                       });
  if (found == state.attempts.end() || !(found->id == id)) {
    return nullptr;
  }
  return &*found;
}

const IdempotencyEntry* find_idempotency(const UpsState& state, const IdempotencyKey& key) noexcept {
  for (const IdempotencyEntry& entry : state.idempotency) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

Status insert_sorted_by_id(std::vector<UpsRecord>& units, UpsRecord record,
                           const ResourceLimits& limits) {
  const auto found = std::lower_bound(units.begin(), units.end(), record.id,
                                       [](const UpsRecord& unit, const UpsId& value) {
                                         return unit.id < value;
                                       });
  if (found != units.end() && found->id == record.id) {
    return Status::error(StatusCode::AlreadyExists,
                         "unit '" + record.id.value() + "' is already registered");
  }
  if (units.size() >= limits.max_ups_units) {
    return Status::error(StatusCode::LimitExceeded,
                         "the store already holds " + std::to_string(units.size()) +
                             " units, the limit is " + std::to_string(limits.max_ups_units));
  }
  units.insert(found, std::move(record));
  return Status::success();
}

Status validate_state(const UpsState& state, const ResourceLimits& limits) {
  if (state.units.size() > limits.max_ups_units) {
    return Status::error(StatusCode::LimitExceeded, "too many units in the state");
  }
  if (state.attempts.size() > limits.max_attempt_journal) {
    return Status::error(StatusCode::LimitExceeded, "too many attempts in the journal");
  }
  if (state.idempotency.size() > limits.max_idempotency_records) {
    return Status::error(StatusCode::LimitExceeded, "too many idempotency bindings");
  }
  if (state.commit_log.size() > limits.max_commit_log) {
    return Status::error(StatusCode::LimitExceeded, "too many commit log entries");
  }

  if (state.generation.is_zero()) {
    if (!state.units.empty() || !state.attempts.empty() || !state.idempotency.empty() ||
        !state.commit_log.empty()) {
      return Status::error(StatusCode::InvariantViolation,
                           "generation zero is the never-committed state and must be empty");
    }
    return Status::success();
  }
  if (state.epoch.is_zero() || state.incarnation.is_zero()) {
    return Status::error(StatusCode::InvariantViolation,
                         "a committed state must carry a positive control epoch and incarnation");
  }
  if (!is_valid_instant(state.created_at) || !is_valid_instant(state.updated_at)) {
    return Status::error(StatusCode::InvalidArgument,
                         "a committed state must carry positive creation and update instants");
  }
  if (state.revalidated_at.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "the revalidation instant must not be negative");
  }
  if (state.updated_at.value() < state.created_at.value()) {
    return Status::error(StatusCode::InvariantViolation,
                         "the update instant precedes the creation instant");
  }

  for (std::size_t index = 0; index < state.units.size(); ++index) {
    const Status status = validate_ups_record(state.units[index], limits);
    if (!status.ok()) {
      return status;
    }
    if (index > 0 && !(state.units[index - 1].id < state.units[index].id)) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "units must be strictly ordered by identifier: '" +
                               state.units[index].id.value() + "'");
    }
  }

  for (std::size_t index = 0; index < state.attempts.size(); ++index) {
    const AttemptRecord& attempt = state.attempts[index];
    if (attempt.id.is_zero()) {
      return Status::error(StatusCode::InvariantViolation,
                           "an attempt identifier must be positive");
    }
    if (index > 0 && !(state.attempts[index - 1].id < attempt.id)) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "attempts must be strictly ordered by identifier: " +
                               std::to_string(attempt.id.value()));
    }
    if (attempt.planned_revision.is_zero()) {
      return Status::error(StatusCode::InvariantViolation,
                           "an attempt must name the positive state revision it was planned against");
    }
    const UpsRecord* unit = find_ups(state, attempt.ups);
    if (unit == nullptr) {
      return Status::error(StatusCode::InvariantViolation,
                           "attempt " + std::to_string(attempt.id.value()) +
                               " names unit '" + attempt.ups.value() +
                               "' which does not exist");
    }
    // The planned revision is deliberately NOT bounded above by the unit revision.
    // An attempt record is a durable fact about a request, and a request that cited
    // a revision the unit never had is exactly the kind of request whose refusal
    // has to be recorded: refusing to persist it would turn a recorded
    // StaleRevision refusal into an unrecorded invariant violation, and would leave
    // no journal entry and no idempotency binding behind.
    if (is_terminal(attempt.phase) && attempt.refusal.has_value() &&
        attempt.phase != AttemptPhase::Refused) {
      return Status::error(StatusCode::InvariantViolation,
                           "only a refused attempt may carry a refusal record");
    }
    if (attempt.phase == AttemptPhase::Refused && !attempt.refusal.has_value()) {
      return Status::error(StatusCode::InvariantViolation,
                           "a refused attempt must record why it was refused");
    }
    if (attempt.phase == AttemptPhase::Verified &&
        attempt.verification != VerificationVerdict::Verified) {
      return Status::error(StatusCode::InvariantViolation,
                           "a verified attempt must carry the verified verdict");
    }
    if (attempt.phase == AttemptPhase::Verified && attempt.ack != AckOutcome::Accepted &&
        attempt.ack != AckOutcome::NoResponse) {
      return Status::error(StatusCode::InvariantViolation,
                           "a verified attempt must have been accepted or have lost its "
                           "acknowledgement");
    }
  }
  if (state.last_attempt.value() != 0 && !state.attempts.empty() &&
      state.last_attempt.value() < state.attempts.back().id.value()) {
    return Status::error(StatusCode::InvariantViolation,
                         "the last attempt ordinal is behind the journal");
  }

  std::set<std::string> keys;
  for (const IdempotencyEntry& entry : state.idempotency) {
    if (!keys.insert(entry.key.value()).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "the idempotency key '" + entry.key.value() + "' is bound twice");
    }
    if (find_attempt(state, entry.attempt) == nullptr) {
      return Status::error(StatusCode::InvariantViolation,
                           "the idempotency binding for '" + entry.key.value() +
                               "' names attempt " + std::to_string(entry.attempt.value()) +
                               " which is not in the journal");
    }
  }
  if (!state.idempotency.empty() && !state.attempts.empty()) {
    for (const IdempotencyEntry& entry : state.idempotency) {
      if (entry.attempt.value() > state.last_attempt.value()) {
        return Status::error(StatusCode::InvariantViolation,
                             "an idempotency binding names an attempt above the last ordinal");
      }
    }
  }

  for (std::size_t index = 0; index < state.commit_log.size(); ++index) {
    if (index > 0 && state.commit_log[index].generation.value() <=
                         state.commit_log[index - 1].generation.value()) {
      return Status::error(StatusCode::InvariantViolation,
                           "the commit log must be strictly ordered by generation");
    }
    if (state.commit_log[index].generation.value() > state.generation.value()) {
      return Status::error(StatusCode::InvariantViolation,
                           "the commit log names a generation ahead of the committed head");
    }
  }

  // --- single-device ordering, enforced structurally ---
  for (const UpsRecord& unit : state.units) {
    std::size_t unresolved = 0;
    std::optional<AttemptId> unresolved_id;
    for (const AttemptRecord& attempt : state.attempts) {
      if (!(attempt.ups == unit.id) || is_terminal(attempt.phase)) {
        continue;
      }
      ++unresolved;
      unresolved_id = attempt.id;
    }
    if (unresolved > 1) {
      return Status::error(StatusCode::InvariantViolation,
                           "unit '" + unit.id.value() + "' has " + std::to_string(unresolved) +
                               " unresolved control attempts; at most one device transition may be "
                               "in flight at a time");
    }
    if (unit.in_flight.has_value()) {
      if (unresolved == 0) {
        return Status::error(StatusCode::InvariantViolation,
                             "unit '" + unit.id.value() + "' holds the in-flight gate for attempt " +
                                 std::to_string(unit.in_flight->value()) +
                                 " but no such attempt is unresolved");
      }
      if (!(unresolved_id.value() == unit.in_flight.value())) {
        return Status::error(StatusCode::InvariantViolation,
                             "unit '" + unit.id.value() +
                                 "' holds the in-flight gate for a different attempt than the "
                                 "unresolved one in the journal");
      }
    }
  }

  // Every state-dependent mutation must record a commit, except the commit that
  // created the store.
  if (!state.commit_log.empty() &&
      state.commit_log.back().generation.value() != state.generation.value()) {
    return Status::error(StatusCode::InvariantViolation,
                         "the commit log does not describe the committed generation");
  }
  return Status::success();
}

Result<std::vector<std::byte>> encode_canonical_state(const UpsState& state,
                                                      const ResourceLimits& limits) {
  return detail::encode_state(state, limits);
}

std::uint64_t canonical_state_digest(const UpsState& state) {
  const Result<std::vector<std::byte>> encoded =
      encode_canonical_state(state, ResourceLimits::defaults());
  if (!encoded.ok()) {
    return 0;
  }
  detail::Digest64 digest;
  digest.update(encoded.value());
  return digest.value();
}

}  // namespace ups_control
