#include "ups_control/engine.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "detail/digest.hpp"
#include "detail/evaluate.hpp"
#include "detail/platform_io.hpp"
#include "ups_control/ups.hpp"

namespace ups_control {
namespace {

/// The deterministic fingerprint of a control request's intent.
///
/// The fingerprint deliberately covers only what the caller is asking for: the
/// device, the command, its parameters, and the cited authority. It does not
/// cover the instant, the epoch, the incarnation, or the planned revision,
/// because a retry after a lost response naturally re-reads those fences. That is
/// what lets a retry of an already accepted attempt return the prior accepted
/// result instead of being rejected by a staleness check that no longer applies.
std::uint64_t plan_digest(const ControlCommand& command) {
  detail::Digest64 digest;
  digest.update_text(command.ref.ups.value());
  digest.update_u64(command.ref.hardware.value());
  digest.update_text(to_string(command.kind));
  digest.update_bool(command.parameters.asserted_target.has_value());
  if (command.parameters.asserted_target.has_value()) {
    digest.update_text(to_string(command.parameters.asserted_target.value()));
  }
  digest.update_bool(command.parameters.reserve_requirement.has_value());
  if (command.parameters.reserve_requirement.has_value()) {
    digest.update_text(to_string(command.parameters.reserve_requirement->minimum.unit));
    digest.update_i64(command.parameters.reserve_requirement->minimum.value);
  }
  digest.update_i64(command.parameters.verification_dwell.value());
  digest.update_bool(command.authority_ref.has_value());
  if (command.authority_ref.has_value()) {
    digest.update_text(command.authority_ref->value());
  }
  return digest.value();
}

/// Appends one attempt to the bounded journal.
///
/// Eviction from the front also drops every idempotency binding that named an
/// evicted attempt. Leaving an orphaned binding behind would make the state fail
/// its own structural validation, because a binding must always name an attempt
/// that is still in the journal. The observable consequence is exactly the
/// documented one: an evicted key stops being recognized, so a retry carrying it
/// is treated as a new request and is then refused by the ordinary staleness
/// checks rather than silently re-applied.

/// Everything one read path needs, captured under the mutex so that the path can
/// then compute on an immutable snapshot with no lock held.
struct ReadView {
  std::shared_ptr<const UpsState> state;
  bool revalidated = false;
  bool adapter_bound = false;
  std::uint32_t adapter_mask = 0;
  AdapterId adapter;
  bool writable = false;
  /// Units whose state basis was established in this engine session. A unit that
  /// is absent has only been read back from a store, so its reported basis is
  /// \c Recovered.
  std::set<std::string> confirmed;
  StoreInfo store;
};

void append_attempt(UpsState& state, const AttemptRecord& record, const ResourceLimits& limits) {
  state.attempts.push_back(record);
  if (state.attempts.size() <= limits.max_attempt_journal) {
    return;
  }
  while (state.attempts.size() > limits.max_attempt_journal) {
    state.attempts.erase(state.attempts.begin());
  }
  const AttemptId oldest = state.attempts.front().id;
  state.idempotency.erase(
      std::remove_if(state.idempotency.begin(), state.idempotency.end(),
                     [oldest](const IdempotencyEntry& entry) {
                       return entry.attempt.value() < oldest.value();
                     }),
      state.idempotency.end());
}

void bind_idempotency(UpsState& state, const IdempotencyEntry& entry,
                      const ResourceLimits& limits) {
  const auto found = std::find_if(state.idempotency.begin(), state.idempotency.end(),
                                  [&entry](const IdempotencyEntry& candidate) {
                                    return candidate.key == entry.key;
                                  });
  if (found != state.idempotency.end()) {
    *found = entry;
  } else {
    state.idempotency.push_back(entry);
  }
  while (state.idempotency.size() > limits.max_idempotency_records) {
    state.idempotency.erase(state.idempotency.begin());
  }
}

}  // namespace

/// The engine's private implementation.
///
/// Ownership: one mutex protects the in-memory authoritative snapshot, and one
/// operating-system writer lock (held by the store from open to close) is the
/// cross-process exclusion primitive. The mutex is the only C++ lock in this
/// runtime, so there is exactly one lock level and no lock-order question. The
/// mutex is never held across an adapter call.
struct UpsControlEngine::Impl {
  std::shared_ptr<UpsStore> store;
  std::shared_ptr<UpsAdapter> adapter;
  AdapterCapabilities adapter_capabilities;
  ResourceLimits limits = ResourceLimits::defaults();
  SessionId session;
  /// The immutable authoritative snapshot. Readers copy the shared pointer under
  /// the mutex and then compute outside it; mutation replaces the pointer.
  std::shared_ptr<const UpsState> state;
  mutable std::mutex mutex;
  EngineLifecycle lifecycle = EngineLifecycle::Unopened;
  bool writable = false;
  bool closed = true;
  /// Units whose state basis was established in this session rather than merely
  /// read back from the store. Never persisted: persistence must not carry a
  /// freshness claim across a restart.
  std::set<std::string> confirmed;

  Status ensure_open() const {
    if (closed || !store || !store->is_open()) {
      return Status::error(StatusCode::Closed, "the engine is closed");
    }
    return Status::success();
  }

  Status ensure_mutable() const {
    const Status open = ensure_open();
    if (!open.ok()) {
      return open;
    }
    if (!writable) {
      return Status::error(StatusCode::ReadOnly, "the engine was opened read-only");
    }
    return Status::success();
  }

  /// Captures the read view. The caller holds the mutex; the returned view stays
  /// valid after it is released because the state snapshot is immutable.
  void capture(ReadView& view) const {
    view.state = state;
    view.revalidated = lifecycle == EngineLifecycle::Revalidated;
    view.adapter_bound = adapter != nullptr;
    view.adapter_mask = adapter_capabilities.supported_commands;
    view.adapter = adapter_capabilities.id;
    view.writable = writable;
    view.confirmed = confirmed;
    if (store) {
      view.store = store->info();
    }
  }

  /// True when this session established the unit's state basis itself.
  bool is_confirmed(const UpsId& id) const { return confirmed.count(id.value()) != 0; }

  /// Records that this session established the unit's state basis.
  void confirm(const UpsId& id) { confirmed.insert(id.value()); }

  /// Common tail of every mutation: advance the generation, stamp the update
  /// instant, record the commit, and publish durably. Nothing is published in
  /// memory unless the durable commit succeeded.
  ///
  /// The caller holds the mutex. The durable commit is performed while it is
  /// held, because the persisted state and the in-memory snapshot have to advance
  /// together: releasing the lock between them would let a second mutation read
  /// the older snapshot and silently overwrite the first.
  Status publish(UpsState& next, Tick now, OperationKind operation, const UpsId& ups,
                 StateRevision revision, const IdempotencyKey& key, AttemptId attempt) {
    if (now.value() < next.updated_at.value()) {
      return Status::error(StatusCode::PreconditionFailed,
                           "the requested instant " + std::to_string(now.value()) +
                               " precedes the last committed instant " +
                               std::to_string(next.updated_at.value()));
    }
    next.generation = StoreGeneration{next.generation.value() + 1};
    next.updated_at = now;
    next.operation_count += 1;

    CommitLogEntry entry;
    entry.generation = next.generation;
    entry.attempt = attempt;
    entry.key = key;
    entry.operation = operation;
    entry.ups = ups;
    entry.revision = revision;
    entry.at = now;
    next.commit_log.push_back(entry);
    while (next.commit_log.size() > limits.max_commit_log) {
      next.commit_log.erase(next.commit_log.begin());
    }

    const Result<CommitResult> committed = store->commit(next);
    if (!committed.ok()) {
      return committed.status();
    }
    state = std::make_shared<const UpsState>(std::move(next));
    return Status::success();
  }
};

namespace {

/// Applies the adapter acknowledgement to one attempt record, honouring the
/// single-device ordering gate: a terminal attempt is never reopened.
void apply_acknowledgement(AttemptRecord& record, AckOutcome outcome, const std::string& detail,
                           Tick now, const ResourceLimits& limits) {
  record.ack = outcome;
  record.ack_detail = detail.size() > limits.max_detail_bytes
                          ? detail.substr(0, limits.max_detail_bytes)
                          : detail;
  if (is_terminal(record.phase)) {
    // The operator resolved the attempt while the adapter call was in flight. The
    // terminal phase wins; the acknowledgement is still recorded so the fact that
    // the device was addressed is not lost.
    return;
  }
  switch (outcome) {
    case AckOutcome::Accepted:
      record.phase = AttemptPhase::Acknowledged;
      record.acknowledged_at = now;
      break;
    case AckOutcome::Rejected:
    case AckOutcome::Malformed:
      record.phase = AttemptPhase::Failed;
      record.acknowledged_at = now;
      record.terminal_detail = "the adapter did not accept the command";
      break;
    case AckOutcome::Unsupported:
      record.phase = AttemptPhase::Unsupported;
      record.acknowledged_at = now;
      record.terminal_detail = "the adapter reports the command as unimplemented";
      break;
    case AckOutcome::NoResponse:
    case AckOutcome::None:
      record.phase = AttemptPhase::Issued;
      record.terminal_detail =
          "the adapter returned no response; the attempt stays unresolved because the device may "
          "have acted, and it is never reissued";
      break;
  }
}

std::string bounded(const std::string& text, const ResourceLimits& limits) {
  return text.size() > limits.max_detail_bytes ? text.substr(0, limits.max_detail_bytes) : text;
}

}  // namespace

const char* to_string(EngineLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case EngineLifecycle::Unopened: return "unopened";
    case EngineLifecycle::Recovered: return "recovered";
    case EngineLifecycle::Revalidated: return "revalidated";
    case EngineLifecycle::Closed: return "closed";
  }
  return "unknown";
}

UpsControlEngine::UpsControlEngine(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

UpsControlEngine::~UpsControlEngine() {
  if (impl_ != nullptr) {
    (void)close();
  }
}

bool UpsControlEngine::is_open() const noexcept {
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return !impl_->closed && impl_->store && impl_->store->is_open();
}

const ResourceLimits& UpsControlEngine::limits() const noexcept { return impl_->limits; }

Result<std::shared_ptr<UpsControlEngine>> UpsControlEngine::open(const EngineOpenOptions& options) {
  Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options.store);
  if (!store.ok()) {
    return store.status();
  }
  auto impl = std::make_shared<Impl>();
  impl->store = store.value();
  impl->adapter = options.adapter;
  impl->limits = options.store.limits;
  impl->session = SessionId::generate();
  impl->writable = store.value()->writable();
  impl->state = store.value()->opened_state();
  if (impl->state == nullptr) {
    return Status::error(StatusCode::InvariantViolation,
                         "the store produced no state for the engine to recover");
  }
  if (impl->adapter != nullptr) {
    impl->adapter_capabilities = impl->adapter->capabilities();
  }
  impl->lifecycle = EngineLifecycle::Recovered;
  impl->closed = false;
  return std::shared_ptr<UpsControlEngine>(new UpsControlEngine(std::move(impl)));
}

Status UpsControlEngine::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  std::shared_ptr<UpsStore> store;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->closed) {
      return Status::success();
    }
    impl_->closed = true;
    impl_->lifecycle = EngineLifecycle::Closed;
    store = impl_->store;
    impl_->store.reset();
    impl_->adapter.reset();
  }
  if (store != nullptr) {
    return store->close();
  }
  return Status::success();
}

EngineInfo UpsControlEngine::info() const {
  EngineInfo info;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->store) {
    const StoreInfo& store = impl_->store->info();
    info.path = store.path;
    info.store_identity = store.store_identity;
    info.generation = store.generation;
    info.epoch = store.epoch;
    info.incarnation = store.incarnation;
    info.commit_count = store.commit_count;
    info.writable = store.writable;
  }
  info.session = impl_->session;
  info.lifecycle = impl_->lifecycle;
  if (impl_->state != nullptr) {
    info.revalidated_at = impl_->state->revalidated_at;
    info.created_at = impl_->state->created_at;
    info.updated_at = impl_->state->updated_at;
    info.unit_count = impl_->state->units.size();
    info.attempt_count = impl_->state->attempts.size();
    for (const UpsRecord& unit : impl_->state->units) {
      if (unit.in_flight.has_value()) {
        ++info.unresolved_attempt_count;
      }
    }
  }
  info.adapter_bound = impl_->adapter != nullptr;
  info.adapter = impl_->adapter_capabilities;
  info.evidence = impl_->adapter != nullptr ? impl_->adapter_capabilities.evidence
                                            : EvidenceClass::Unsupported;
  return info;
}

// ---------------------------------------------------------------------------
// Registration and model mutation
// ---------------------------------------------------------------------------

Result<UpsRecord> UpsControlEngine::register_ups(const RegisterUpsRequest& request) {
  Impl& impl = *impl_;
  Status status = validate_identifier(request.id.value(), impl.limits.max_identifier_bytes);
  if (!status.ok()) {
    return status;
  }
  status = validate_label(request.label, impl.limits.max_label_bytes);
  if (!status.ok()) {
    return status;
  }
  status = validate_label(request.note, impl.limits.max_detail_bytes);
  if (!status.ok()) {
    return status;
  }
  if (request.hardware.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a registered unit must carry a positive hardware generation");
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument,
                         "registration requires a positive instant");
  }
  if (!is_consistent(request.lifecycle, request.operating)) {
    return Status::error(StatusCode::Conflict,
                         std::string("lifecycle ") + to_string(request.lifecycle) +
                             " is inconsistent with operating state " +
                             to_string(request.operating));
  }
  status = validate_reserve_policy(request.policy, impl.limits);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority,
                         "registration cites epoch " +
                             std::to_string(request.authority.epoch.value()) + " incarnation " +
                             std::to_string(request.authority.incarnation.value()) +
                             " but the store is at epoch " +
                             std::to_string(impl.state->epoch.value()) + " incarnation " +
                             std::to_string(impl.state->incarnation.value()));
  }

  UpsState next = *impl.state;
  UpsRecord record;
  record.id = request.id;
  record.label = request.label;
  record.hardware = request.hardware;
  record.lifecycle = request.lifecycle;
  record.operating = request.operating;
  record.basis = request.operating == OperatingState::Unknown ? StateBasis::Recovered
                                                             : StateBasis::Observed;
  record.revision = StateRevision{1};
  record.state_since = request.now;
  record.registered_at = request.now;
  record.registration_note = request.note;
  record.reserve_policy = request.policy;

  status = insert_sorted_by_id(next.units, record, impl.limits);
  if (!status.ok()) {
    return status;
  }
  status = impl.publish(next, request.now, OperationKind::UpsRegistered, record.id,
                   record.revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  impl.confirm(record.id);
  const UpsRecord* stored = find_ups(*impl.state, record.id);
  if (stored == nullptr) {
    return Status::error(StatusCode::InvariantViolation,
                         "the registered unit vanished from the published state");
  }
  return *stored;
}

Result<UpsRecord> UpsControlEngine::set_lifecycle(const SetLifecycleRequest& request) {
  Impl& impl = *impl_;
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "a lifecycle change requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "the lifecycle change cites stale authority");
  }
  if (!(current->hardware == request.ref.hardware)) {
    return Status::error(StatusCode::StaleAuthority,
                         "the lifecycle change cites hardware generation " +
                             std::to_string(request.ref.hardware.value()) + " but the unit is at " +
                             std::to_string(current->hardware.value()));
  }
  if (!(current->revision == request.ref.revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "the lifecycle change cites revision " +
                             std::to_string(request.ref.revision.value()) + " but the unit is at " +
                             std::to_string(current->revision.value()));
  }
  if (current->lifecycle == request.lifecycle) {
    return Status::error(StatusCode::Conflict,
                         std::string("the unit is already in lifecycle ") +
                             to_string(request.lifecycle));
  }
  if (!is_legal_lifecycle_transition(current->lifecycle, request.lifecycle)) {
    return Status::error(StatusCode::IllegalTransition,
                         std::string("the lifecycle transition from ") + to_string(current->lifecycle) +
                             " to " + to_string(request.lifecycle) +
                             " is not an edge of the documented lifecycle graph");
  }
  if (!is_consistent(request.lifecycle, current->operating)) {
    return Status::error(StatusCode::Conflict,
                         std::string("lifecycle ") + to_string(request.lifecycle) +
                             " is inconsistent with the current operating state " +
                             to_string(current->operating) +
                             "; change the operating state first, through an observation or an "
                             "adopted state");
  }

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  unit->lifecycle = request.lifecycle;
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  const Status status = impl.publish(next, request.now, OperationKind::LifecycleChanged,
                               request.ref.ups, revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  return *find_ups(*impl.state, request.ref.ups);
}

Result<UpsRecord> UpsControlEngine::adopt_operating_state(const AdoptOperatingStateRequest& request) {
  Impl& impl = *impl_;
  Status status = validate_label(request.reason, impl.limits.max_detail_bytes);
  if (!status.ok()) {
    return status;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "state adoption requires a positive instant");
  }
  if (request.operating == OperatingState::Unknown) {
    return Status::error(StatusCode::InvalidArgument,
                         "an adopted state must be a concrete operating state; Unknown is the "
                         "absence of an established state and is never adopted");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "state adoption cites stale authority");
  }
  if (!(current->hardware == request.ref.hardware)) {
    return Status::error(StatusCode::StaleAuthority, "state adoption cites a stale hardware generation");
  }
  if (!(current->revision == request.ref.revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "state adoption cites revision " +
                             std::to_string(request.ref.revision.value()) + " but the unit is at " +
                             std::to_string(current->revision.value()));
  }
  if (!is_consistent(current->lifecycle, request.operating)) {
    return Status::error(StatusCode::Conflict,
                         std::string("operating state ") + to_string(request.operating) +
                             " is inconsistent with lifecycle " + to_string(current->lifecycle));
  }

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  if (!(unit->operating == request.operating)) {
    unit->state_since = request.now;
  }
  unit->operating = request.operating;
  unit->basis = StateBasis::Observed;
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  status = impl.publish(next, request.now, OperationKind::OperatingStateAdopted, request.ref.ups,
                   revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  impl.confirm(request.ref.ups);
  return *find_ups(*impl.state, request.ref.ups);
}

Result<UpsRecord> UpsControlEngine::set_reserve_policy(const SetReservePolicyRequest& request) {
  Impl& impl = *impl_;
  const Status valid = validate_reserve_policy(request.policy, impl.limits);
  if (!valid.ok()) {
    return valid;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "a policy change requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "the policy change cites stale authority");
  }
  if (!(current->hardware == request.ref.hardware) || !(current->revision == request.ref.revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "the policy change cites a superseded generation or revision");
  }

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  unit->reserve_policy = request.policy;
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  const Status status = impl.publish(next, request.now, OperationKind::PolicyChanged, request.ref.ups,
                                revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  return *find_ups(*impl.state, request.ref.ups);
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

Result<ControlContext> UpsControlEngine::adopt_authority(const AdoptAuthorityRequest& request) {
  Impl& impl = *impl_;
  if (request.epoch.is_zero() || request.incarnation.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "an adopted authority must carry a positive epoch and incarnation");
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "an authority handoff requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority,
                         "the handoff must be requested by the currently authoritative epoch and "
                         "incarnation");
  }
  const bool forward =
      request.epoch.value() > impl.state->epoch.value() ||
      (request.epoch.value() == impl.state->epoch.value() &&
       request.incarnation.value() > impl.state->incarnation.value());
  if (!forward) {
    return Status::error(StatusCode::StaleAuthority,
                         "the handoff to epoch " + std::to_string(request.epoch.value()) +
                             " incarnation " + std::to_string(request.incarnation.value()) +
                             " is not strictly forward from epoch " +
                             std::to_string(impl.state->epoch.value()) + " incarnation " +
                             std::to_string(impl.state->incarnation.value()) +
                             "; an authority rollback is refused");
  }

  UpsState next = *impl.state;
  next.epoch = request.epoch;
  next.incarnation = request.incarnation;
  // A handoff never resolves an unresolved attempt: doing so would erase the fact
  // that a device may have acted. The new authority must verify or abandon it.
  const Status status = impl.publish(next, request.now, OperationKind::AuthorityAdopted, UpsId{},
                                StateRevision{}, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  return ControlContext{impl.state->epoch, impl.state->incarnation};
}

Result<AuthorityGrant> UpsControlEngine::issue_grant(const IssueGrantRequest& request) {
  Impl& impl = *impl_;
  AuthorityGrant grant = request.grant;
  if (grant.epoch.is_zero() && grant.incarnation.is_zero()) {
    grant.epoch = request.authority.epoch;
    grant.incarnation = request.authority.incarnation;
  }
  const Status valid = validate_grant(grant, impl.limits);
  if (!valid.ok()) {
    return valid;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "issuing a grant requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "issuing a grant cites stale authority");
  }
  if (!(grant.epoch == impl.state->epoch) || !(grant.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority,
                         "the grant is bound to epoch " + std::to_string(grant.epoch.value()) +
                             " incarnation " + std::to_string(grant.incarnation.value()) +
                             " but the store is at epoch " +
                             std::to_string(impl.state->epoch.value()) + " incarnation " +
                             std::to_string(impl.state->incarnation.value()) +
                             "; a grant never crosses an authority handoff");
  }
  if (!(current->hardware == request.ref.hardware) || !(current->revision == request.ref.revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "issuing a grant cites a superseded generation or revision");
  }
  const AuthorityGrant* existing = find_grant(*current, grant.ref);
  if (existing != nullptr && !existing->revoked_at.has_value()) {
    return Status::error(StatusCode::AlreadyExists,
                         "grant '" + grant.ref.value() +
                             "' is already in force; a live grant is never silently replaced");
  }
  grant.revision = GrantRevision{existing == nullptr ? 1 : existing->revision.value() + 1};

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  const Status upsert = upsert_grant(*unit, grant, impl.limits);
  if (!upsert.ok()) {
    return upsert;
  }
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  const Status status = impl.publish(next, request.now, OperationKind::GrantIssued, request.ref.ups,
                                revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  const AuthorityGrant* stored = find_grant(*find_ups(*impl.state, request.ref.ups), grant.ref);
  if (stored == nullptr) {
    return Status::error(StatusCode::InvariantViolation, "the issued grant vanished");
  }
  return *stored;
}

Result<AuthorityGrant> UpsControlEngine::revoke_grant(const RevokeGrantRequest& request) {
  Impl& impl = *impl_;
  Status status = validate_identifier(request.revocation_authority.value(), impl.limits.max_identifier_bytes);
  if (!status.ok()) {
    return status;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "revoking a grant requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "revoking a grant cites stale authority");
  }
  if (!(current->hardware == request.ref.hardware) || !(current->revision == request.ref.revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "revoking a grant cites a superseded generation or revision");
  }
  const AuthorityGrant* existing = find_grant(*current, request.grant);
  if (existing == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no grant '" + request.grant.value() + "' is bound to unit '" +
                             request.ref.ups.value() + "'");
  }
  if (existing->revoked_at.has_value()) {
    return Status::error(StatusCode::Conflict,
                         "grant '" + request.grant.value() + "' is already revoked");
  }

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  AuthorityGrant* grant = nullptr;
  for (AuthorityGrant& candidate : unit->grants) {
    if (candidate.ref == request.grant) {
      grant = &candidate;
      break;
    }
  }
  grant->revoked_by = request.revocation_authority;
  grant->revoked_at = request.now;
  grant->revision = GrantRevision{grant->revision.value() + 1};
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  status = impl.publish(next, request.now, OperationKind::GrantRevoked, request.ref.ups, revision,
                   IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  return *find_grant(*find_ups(*impl.state, request.ref.ups), request.grant);
}

// ---------------------------------------------------------------------------
// Protected-load obligations
// ---------------------------------------------------------------------------

Result<ProtectedLoadObligation> UpsControlEngine::bind_obligation(const BindObligationRequest& request) {
  Impl& impl = *impl_;
  ProtectedLoadObligation obligation = request.obligation;
  if (obligation.epoch.is_zero() && obligation.incarnation.is_zero()) {
    obligation.epoch = request.authority.epoch;
    obligation.incarnation = request.authority.incarnation;
  }
  const Status valid = validate_obligation(obligation, impl.limits);
  if (!valid.ok()) {
    return valid;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "binding an obligation requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "binding an obligation cites stale authority");
  }
  if (obligation.epoch.value() > impl.state->epoch.value()) {
    return Status::error(StatusCode::InvalidArgument,
                         "the obligation was asserted under epoch " +
                             std::to_string(obligation.epoch.value()) +
                             " which is ahead of the current epoch " +
                             std::to_string(impl.state->epoch.value()));
  }
  if (obligation.asserted_at.value() > request.now.value()) {
    return Status::error(StatusCode::InvalidArgument,
                         "the obligation asserts a future instant");
  }
  if (!(current->hardware == request.ref.hardware) || !(current->revision == request.ref.revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "binding an obligation cites a superseded generation or revision");
  }
  const ProtectedLoadObligation* existing = find_obligation(*current, obligation.ref);
  if (existing != nullptr && existing->state == ObligationState::Released) {
    return Status::error(StatusCode::AlreadyExists,
                         "obligation '" + obligation.ref.value() +
                             "' was released and remains part of the audit trail; bind a new "
                             "reference instead of resurrecting it");
  }
  if (existing != nullptr && existing->protection != obligation.protection) {
    return Status::error(StatusCode::Conflict,
                         "obligation '" + obligation.ref.value() +
                             "' already asserts a different protection requirement; a binding is "
                             "never weakened in place");
  }
  obligation.state = ObligationState::Active;
  obligation.released_by.reset();
  obligation.released_at.reset();
  obligation.release_reason.clear();
  obligation.revision = ObligationRevision{existing == nullptr ? 1 : existing->revision.value() + 1};

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  const Status upsert = upsert_obligation(*unit, obligation, impl.limits);
  if (!upsert.ok()) {
    return upsert;
  }
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  const Status status = impl.publish(next, request.now, OperationKind::ObligationBound,
                                request.ref.ups, revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  return *find_obligation(*find_ups(*impl.state, request.ref.ups), obligation.ref);
}

Result<ProtectedLoadObligation> UpsControlEngine::release_obligation(
    const ReleaseObligationRequest& request) {
  Impl& impl = *impl_;
  Status status = validate_identifier(request.release_authority.value(), impl.limits.max_identifier_bytes);
  if (!status.ok()) {
    return status;
  }
  status = validate_label(request.reason, impl.limits.max_detail_bytes);
  if (!status.ok()) {
    return status;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "releasing an obligation requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "releasing an obligation cites stale authority");
  }
  const ProtectedLoadObligation* existing = find_obligation(*current, request.obligation);
  if (existing == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no obligation '" + request.obligation.value() + "' is bound to unit '" +
                             request.ref.ups.value() + "'");
  }
  if (!(existing->revision == request.obligation_revision)) {
    return Status::error(StatusCode::StaleSourceGeneration,
                         "the release cites revision " +
                             std::to_string(request.obligation_revision.value()) +
                             " but the binding is at revision " +
                             std::to_string(existing->revision.value()));
  }
  if (existing->state == ObligationState::Released) {
    return Status::error(StatusCode::Conflict,
                         "obligation '" + request.obligation.value() + "' is already released");
  }
  if (request.outcome == ObligationState::Active) {
    return Status::error(StatusCode::InvalidArgument,
                         "a release either releases or suspends a binding; it never reinstates one");
  }

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  ProtectedLoadObligation* obligation = nullptr;
  for (ProtectedLoadObligation& candidate : unit->obligations) {
    if (candidate.ref == request.obligation) {
      obligation = &candidate;
      break;
    }
  }
  obligation->state = request.outcome;
  obligation->released_by = request.release_authority;
  obligation->released_at = request.now;
  obligation->release_reason = request.reason;
  obligation->revision = ObligationRevision{obligation->revision.value() + 1};
  unit->revision = StateRevision{unit->revision.value() + 1};
  const StateRevision revision = unit->revision;
  status = impl.publish(next, request.now, OperationKind::ObligationReleased, request.ref.ups,
                   revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  return *find_obligation(*find_ups(*impl.state, request.ref.ups), request.obligation);
}

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------

Result<ObservationRecord> UpsControlEngine::record_telemetry(const RecordTelemetryRequest& request) {
  Impl& impl = *impl_;
  TelemetryReport report = request.report;
  const Status valid = validate_telemetry_report(report, impl.limits);
  if (!valid.ok()) {
    return valid;
  }
  if (!(report.ups == request.ref.ups)) {
    return Status::error(StatusCode::InvalidArgument,
                         "the telemetry report names unit '" + report.ups.value() +
                             "' but the request targets '" + request.ref.ups.value() + "'");
  }
  if (!(report.hardware == request.ref.hardware)) {
    return Status::error(StatusCode::Conflict,
                         "the telemetry report carries hardware generation " +
                             std::to_string(report.hardware.value()) +
                             " but the request cites " +
                             std::to_string(request.ref.hardware.value()) +
                             "; telemetry from a superseded device generation is refused");
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "recording telemetry requires a positive instant");
  }
  // A future observation is recorded but never satisfies a freshness test. It is
  // not refused here: the record is the honest account of what the device
  // reported, and the freshness policy is what refuses to use it.
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  const UpsRecord* current = find_ups(*impl.state, request.ref.ups);
  if (current == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "no unit '" + request.ref.ups.value() + "' is registered");
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "recording telemetry cites stale authority");
  }
  if (!(current->hardware == request.ref.hardware)) {
    return Status::error(StatusCode::StaleAuthority,
                         "the request cites hardware generation " +
                             std::to_string(request.ref.hardware.value()) + " but the unit is at " +
                             std::to_string(current->hardware.value()));
  }
  if (current->observation.has_value()) {
    const ObservationRecord& previous = current->observation.value();
    if (report.observed_at.value() < previous.observed_at.value()) {
      return Status::error(StatusCode::StaleSourceGeneration,
                           "the report claims instant " + std::to_string(report.observed_at.value()) +
                               " which precedes the recorded observation instant " +
                               std::to_string(previous.observed_at.value()) +
                               "; reordered telemetry is refused rather than merged");
    }
    if (report.observed_at.value() == previous.observed_at.value() &&
        report.revision.value() <= previous.revision.value()) {
      return Status::error(StatusCode::StaleSourceGeneration,
                           "the report claims instant " + std::to_string(report.observed_at.value()) +
                               " with source revision " + std::to_string(report.revision.value()) +
                               ", which is not newer than the recorded revision " +
                               std::to_string(previous.revision.value()));
    }
  }

  ObservationRecord record;
  record.evidence = report.evidence;
  record.source = report.source;
  record.provenance = report.provenance;
  record.revision = report.revision;
  record.observed_at = report.observed_at;
  record.received_at = report.received_at;
  record.operating = report.operating;
  record.transfer = report.transfer;
  record.capability = report.capability;
  record.battery = report.battery;
  record.fault_present = report.fault_present;

  std::vector<std::string> contradictions = find_contradictions(report);
  if (!is_consistent(current->lifecycle, report.operating)) {
    contradictions.push_back(std::string("the reported operating state ") + to_string(report.operating) +
                             " is inconsistent with lifecycle " + to_string(current->lifecycle));
  }

  UpsState next = *impl.state;
  UpsRecord* unit = find_ups(next, request.ref.ups);
  unit->revision = StateRevision{unit->revision.value() + 1};

  if (!contradictions.empty()) {
    std::string detail;
    for (std::size_t index = 0; index < contradictions.size(); ++index) {
      if (index > 0) {
        detail += "; ";
      }
      detail += contradictions[index];
    }
    record.contradictory = true;
    record.contradiction_detail = bounded(detail, impl.limits);
    // A contradictory report is recorded, but it never moves the authoritative
    // operating state: contradictory evidence is not promoted to a fact.
    unit->observation = record;
  } else {
    if (!(unit->operating == report.operating)) {
      unit->state_since = report.observed_at;
    }
    unit->operating = report.operating;
    unit->basis = StateBasis::Observed;
    unit->observation = record;
  }

  // Advance an unresolved attempt to the observed stage. An observation of the
  // effect is a distinct stage from the acknowledgement, and neither of them is
  // the verified effect.
  if (unit->in_flight.has_value()) {
    AttemptRecord* attempt = find_attempt(next, unit->in_flight.value());
    if (attempt != nullptr && !is_terminal(attempt->phase)) {
      const Tick reference = attempt->acknowledged_at.value() > 0 ? attempt->acknowledged_at
                                                                  : attempt->submitted_at;
      if (record.observed_at.value() > reference.value()) {
        attempt->phase = AttemptPhase::Observed;
        attempt->observed_at = record.observed_at;
        attempt->observed_state = record.operating;
        attempt->observed_capability = record.capability;
        if (record.contradictory) {
          attempt->observed = ObservedOutcome::Contradictory;
        } else if (record.operating == OperatingState::Unknown) {
          attempt->observed = ObservedOutcome::Unavailable;
        } else {
          bool matches = false;
          for (const OperatingState candidate : accepted_effect_states(attempt->command)) {
            if (candidate == record.operating) {
              matches = true;
              break;
            }
          }
          if (command_family(attempt->command) == CommandFamily::Capability) {
            attempt->observed = ObservedOutcome::DifferentState;
          } else {
            attempt->observed = matches ? ObservedOutcome::MatchesTarget
                                        : ObservedOutcome::DifferentState;
          }
        }
      }
    }
  }

  const Status status = impl.publish(next, request.now, OperationKind::TelemetryRecorded,
                                request.ref.ups, unit->revision, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  impl.confirm(request.ref.ups);
  const UpsRecord* stored = find_ups(*impl.state, request.ref.ups);
  if (stored == nullptr || !stored->observation.has_value()) {
    return Status::error(StatusCode::InvariantViolation, "the recorded observation vanished");
  }
  return stored->observation.value();
}

// ---------------------------------------------------------------------------
// Read paths
// ---------------------------------------------------------------------------

Result<TransitionEvaluation> UpsControlEngine::evaluate(const ControlCommand& command) const {
  Impl& impl = *impl_;
  std::shared_ptr<const UpsState> snapshot;
  bool revalidated = false;
  bool adapter_bound = false;
  std::uint32_t mask = 0;
  AdapterId adapter;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    snapshot = impl.state;
    revalidated = impl.lifecycle == EngineLifecycle::Revalidated;
    adapter_bound = impl.adapter != nullptr;
    mask = impl.adapter_capabilities.supported_commands;
    adapter = impl.adapter_capabilities.id;
  }
  if (!is_valid_instant(command.now)) {
    return Status::error(StatusCode::InvalidArgument, "evaluation requires a positive instant");
  }
  detail::EvaluationInputs inputs;
  inputs.state = snapshot.get();
  inputs.record = find_ups(*snapshot, command.ref.ups);
  inputs.command = &command;
  inputs.limits = &impl.limits;
  inputs.revalidated = revalidated;
  inputs.adapter_bound = adapter_bound;
  inputs.adapter_supported_commands = mask;
  inputs.adapter = adapter;
  inputs.basis_confirmed =
      find_ups(*snapshot, command.ref.ups) != nullptr && impl.is_confirmed(command.ref.ups);
  const Result<EvaluationReport> report = detail::evaluate_transition(inputs);
  if (!report.ok()) {
    return report.status();
  }
  TransitionEvaluation evaluation;
  evaluation.report = report.value();
  evaluation.command = command;
  return evaluation;
}

Result<StatusReport> UpsControlEngine::status(const UpsQuery& query) const {
  Impl& impl = *impl_;
  if (!is_valid_instant(query.now)) {
    return Status::error(StatusCode::InvalidArgument, "a status query requires a positive instant");
  }
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const UpsRecord* record = find_ups(*view.state, query.ups);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound, "no unit '" + query.ups.value() + "' is registered");
  }
  const bool revalidated = view.revalidated;

  StatusReport report;
  report.ups = record->id;
  report.label = record->label;
  report.hardware = record->hardware;
  report.epoch = view.state->epoch;
  report.incarnation = view.state->incarnation;
  report.lifecycle = record->lifecycle;
  report.operating = record->operating;
  report.basis = detail::reported_basis(*record, view.confirmed.count(record->id.value()) != 0);
  report.revision = record->revision;
  report.state_since = record->state_since;
  report.registered_at = record->registered_at;
  report.last_verified_at = record->last_verified_at;
  report.revalidated = revalidated;
  report.observation = record->observation;
  report.policy = record->reserve_policy;
  report.obligations = record->obligations;
  report.grants = record->grants;
  report.in_flight = record->in_flight;
  if (record->in_flight.has_value()) {
    const AttemptRecord* attempt = find_attempt(*view.state, record->in_flight.value());
    if (attempt != nullptr) {
      report.in_flight_attempt = *attempt;
    }
  }
  const FreshnessPolicy freshness = detail::unit_freshness_policy(record->reserve_policy);
  if (record->observation.has_value()) {
    report.observation_freshness =
        evaluate_freshness(record->observation->observed_at, query.now, freshness);
    const Result<TickSpan> age = observation_age(record->observation->observed_at, query.now);
    if (age.ok()) {
      report.observation_age = age.value();
    }
  }
  report.reserve = evaluate_reserve(std::nullopt, record->reserve_policy,
                                    record->observation.has_value() ? &record->observation.value()
                                                                    : nullptr,
                                    std::nullopt, query.now, revalidated);
  report.readiness = detail::compute_readiness(
      *view.state, *record, impl.limits, query.now, revalidated, view.adapter_bound,
      view.adapter_mask, view.confirmed.count(record->id.value()) != 0);
  return report;
}

Result<std::vector<UpsRecord>> UpsControlEngine::units() const {
  Impl& impl = *impl_;
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  // The listing applies the same session-relative basis rule as every other read
  // path, so the single-unit view and the listing can never disagree about one
  // unit.
  std::vector<UpsRecord> units = view.state->units;
  for (UpsRecord& unit : units) {
    unit.basis = detail::reported_basis(unit, view.confirmed.count(unit.id.value()) != 0);
  }
  return units;
}

Result<ReadinessReport> UpsControlEngine::readiness(const UpsQuery& query) const {
  Impl& impl = *impl_;
  if (!is_valid_instant(query.now)) {
    return Status::error(StatusCode::InvalidArgument, "a readiness query requires a positive instant");
  }
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const UpsRecord* record = find_ups(*view.state, query.ups);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound, "no unit '" + query.ups.value() + "' is registered");
  }
  return detail::compute_readiness(*view.state, *record, impl.limits, query.now, view.revalidated,
                                   view.adapter_bound, view.adapter_mask,
                                   view.confirmed.count(record->id.value()) != 0);
}

Result<ReserveAssessment> UpsControlEngine::battery(const UpsQuery& query) const {
  Impl& impl = *impl_;
  if (!is_valid_instant(query.now)) {
    return Status::error(StatusCode::InvalidArgument, "a reserve query requires a positive instant");
  }
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const UpsRecord* record = find_ups(*view.state, query.ups);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound, "no unit '" + query.ups.value() + "' is registered");
  }
  return evaluate_reserve(std::nullopt, record->reserve_policy,
                          record->observation.has_value() ? &record->observation.value() : nullptr,
                          std::nullopt, query.now, view.revalidated);
}

Result<std::vector<ProtectedLoadObligation>> UpsControlEngine::obligations(const UpsQuery& query) const {
  Impl& impl = *impl_;
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const UpsRecord* record = find_ups(*view.state, query.ups);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound, "no unit '" + query.ups.value() + "' is registered");
  }
  return record->obligations;
}

Result<AttemptRecord> UpsControlEngine::attempt(AttemptId id) const {
  Impl& impl = *impl_;
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const AttemptRecord* record = find_attempt(*view.state, id);
  if (record == nullptr) {
    return Status::error(StatusCode::AttemptNotFound,
                         "no attempt " + std::to_string(id.value()) + " exists in the journal");
  }
  return *record;
}

Result<AttemptRecord> UpsControlEngine::replay(const IdempotencyKey& key) const {
  Impl& impl = *impl_;
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const IdempotencyEntry* entry = find_idempotency(*view.state, key);
  if (entry == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "idempotency key '" + key.value() +
                             "' is not retained; the retained window is bounded and the oldest "
                             "bindings are evicted first");
  }
  const AttemptRecord* record = find_attempt(*view.state, entry->attempt);
  if (record == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "idempotency key '" + key.value() + "' names attempt " +
                             std::to_string(entry->attempt.value()) +
                             " which has been evicted from the journal");
  }
  return *record;
}

Result<std::vector<AttemptRecord>> UpsControlEngine::history(const HistoryQuery& query) const {
  Impl& impl = *impl_;
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const std::size_t limit = std::min(query.limit, impl.limits.max_history_results);
  std::vector<AttemptRecord> results;
  for (auto iterator = view.state->attempts.rbegin(); iterator != view.state->attempts.rend();
       ++iterator) {
    if (query.ups.has_value() && !(iterator->ups == query.ups.value())) {
      continue;
    }
    if (query.phase.has_value() && iterator->phase != query.phase.value()) {
      continue;
    }
    results.push_back(*iterator);
    if (results.size() >= limit) {
      break;
    }
  }
  return results;
}

Result<StoreAuditReport> UpsControlEngine::store_audit() const {
  Impl& impl = *impl_;
  ReadView view;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status open = impl.ensure_open();
    if (!open.ok()) {
      return open;
    }
    impl.capture(view);
  }
  const StoreInfo& store = view.store;
  StoreAuditReport audit;
  audit.path = store.path;
  audit.store_identity = store.store_identity;
  audit.identity_present = !store.store_identity.is_zero();
  audit.generation = store.generation;
  audit.epoch = store.epoch;
  audit.incarnation = store.incarnation;
  audit.created_at = store.created_at;
  audit.updated_at = store.updated_at;
  audit.revalidated_at = store.revalidated_at;
  audit.format_version = store.format_version;
  audit.head_bytes = store.head_bytes;
  audit.payload_bytes = store.payload_bytes;
  audit.payload_crc32c = store.payload_crc32c;
  audit.recorded_path = store.recorded_path;
  const Result<std::filesystem::path> canonical = detail::canonical_store_path(store.path);
  audit.path_binding_matches =
      canonical.ok() && store.recorded_path == detail::path_utf8(canonical.value());
  audit.unit_count = view.state->units.size();
  audit.attempt_count = view.state->attempts.size();
  for (const AttemptRecord& record : view.state->attempts) {
    if (is_terminal(record.phase)) {
      ++audit.terminal_attempt_count;
    } else {
      ++audit.unresolved_attempt_count;
    }
  }
  audit.idempotency_count = view.state->idempotency.size();
  audit.operation_count = view.state->operation_count;
  audit.canonical_digest = canonical_state_digest(*view.state);
  audit.writable = view.writable;
  audit.commit_log = view.state->commit_log;
  return audit;
}

// ---------------------------------------------------------------------------
// Control attempts
// ---------------------------------------------------------------------------

Result<AttemptRecord> UpsControlEngine::submit(const ControlCommand& command) {
  Impl& impl = *impl_;
  const Status shape = detail::validate_command_shape(command, impl.limits);
  if (!shape.ok()) {
    return shape;
  }
  if (!is_valid_instant(command.now)) {
    return Status::error(StatusCode::InvalidArgument, "a control request requires a positive instant");
  }
  const std::uint64_t digest = plan_digest(command);

  AdapterPlan plan;
  AttemptId reserved;
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const Status mutable_status = impl.ensure_mutable();
    if (!mutable_status.ok()) {
      return mutable_status;
    }
    const UpsState& current = *impl.state;

    // Idempotent replay is resolved before any staleness check, so a retry of an
    // already committed attempt returns the prior accepted result even though the
    // generation, epoch, or revision has moved on since.
    if (const IdempotencyEntry* entry = find_idempotency(current, command.key); entry != nullptr) {
      const AttemptRecord* prior = find_attempt(current, entry->attempt);
      if (prior != nullptr && prior->plan_digest == digest) {
        return *prior;
      }
      return Status::error(StatusCode::IdempotencyConflict,
                           "idempotency key '" + command.key.value() +
                               "' is already bound to a different request; the same key never "
                               "describes two intents");
    }

    detail::EvaluationInputs inputs;
    inputs.state = &current;
    inputs.record = find_ups(current, command.ref.ups);
    inputs.command = &command;
    inputs.limits = &impl.limits;
    inputs.revalidated = impl.lifecycle == EngineLifecycle::Revalidated;
    inputs.adapter_bound = impl.adapter != nullptr;
    inputs.adapter_supported_commands = impl.adapter_capabilities.supported_commands;
    inputs.adapter = impl.adapter_capabilities.id;
    inputs.basis_confirmed = impl.is_confirmed(command.ref.ups);
    const Result<EvaluationReport> evaluated = detail::evaluate_transition(inputs);
    if (!evaluated.ok()) {
      return evaluated.status();
    }
    const EvaluationReport& report = evaluated.value();

    UpsState next = current;
    const AttemptId id{next.last_attempt.value() + 1};
    next.last_attempt = id;

    AttemptRecord record;
    record.id = id;
    record.key = command.key;
    record.ups = command.ref.ups;
    record.hardware = command.ref.hardware;
    record.epoch = command.authority.epoch;
    record.incarnation = command.authority.incarnation;
    record.planned_revision = command.ref.revision;
    record.command = command.kind;
    record.parameters = command.parameters;
    record.target = report.target;
    record.submitted_at = command.now;
    record.plan_digest = digest;
    record.evidence = impl.adapter != nullptr ? impl.adapter_capabilities.evidence
                                              : EvidenceClass::Unsupported;
    record.adapter = impl.adapter_capabilities.id;

    if (!report.allowed) {
      record.phase = AttemptPhase::Refused;
      record.refusal = RefusalRecord{report.primary, report.primary_detail};
      record.terminal_detail = report.primary_detail;
      append_attempt(next, record, impl.limits);
      IdempotencyEntry entry;
      entry.key = command.key;
      entry.attempt = id;
      entry.plan_digest = digest;
      entry.recorded_at = command.now;
      bind_idempotency(next, entry, impl.limits);
      const Status status = impl.publish(next, command.now, OperationKind::AttemptFailed,
                                    command.ref.ups, report.current_revision, command.key, id);
      if (!status.ok()) {
        return status;
      }
      return *find_attempt(*impl.state, id);
    }

    UpsRecord* unit = find_ups(next, command.ref.ups);
    if (unit == nullptr) {
      return Status::error(StatusCode::NotFound,
                           "no unit '" + command.ref.ups.value() + "' is registered");
    }
    unit->in_flight = id;
    record.phase = AttemptPhase::Planned;
    append_attempt(next, record, impl.limits);
    IdempotencyEntry entry;
    entry.key = command.key;
    entry.attempt = id;
    entry.plan_digest = digest;
    entry.recorded_at = command.now;
    bind_idempotency(next, entry, impl.limits);

    // The durable plan. After this commit the command may be issued, and after a
    // crash the attempt is recovered unresolved and is never reissued as new.
    const Status status = impl.publish(next, command.now, OperationKind::AttemptPlanned,
                                  command.ref.ups, report.current_revision, command.key, id);
    if (!status.ok()) {
      return status;
    }
    reserved = id;
    plan.ups = command.ref.ups;
    plan.hardware = command.ref.hardware;
    plan.attempt = id;
    plan.command = command.kind;
    plan.issued_at = command.now;
  }

  // The adapter runs with no internal lock held. Nothing here is interpreted as an
  // effect: the result is an acknowledgement only.
  AdapterIssueResult issue;
  if (impl.adapter != nullptr) {
    issue = impl.adapter->issue(plan);
  } else {
    issue.outcome = AckOutcome::NoResponse;
    issue.detail = "no adapter is bound";
  }

  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status open = impl.ensure_open();
  if (!open.ok()) {
    return open;
  }
  UpsState next = *impl.state;
  AttemptRecord* stored = find_attempt(next, reserved);
  if (stored == nullptr) {
    return Status::error(StatusCode::InvariantViolation,
                         "the reserved attempt vanished from the journal");
  }
  const bool was_terminal = is_terminal(stored->phase);
  const UpsId ups = stored->ups;
  const StateRevision revision = stored->planned_revision;
  apply_acknowledgement(*stored, issue.outcome, issue.detail, plan.issued_at, impl.limits);
  if (!was_terminal && is_terminal(stored->phase) && stored->phase != AttemptPhase::Verified) {
    UpsRecord* unit = find_ups(next, ups);
    if (unit != nullptr && unit->in_flight.has_value() && unit->in_flight.value() == reserved) {
      unit->in_flight.reset();
    }
    if (unit != nullptr && stored->phase == AttemptPhase::Failed) {
      unit->basis = StateBasis::CommandedUnverified;
    }
  }
  const OperationKind operation = stored->phase == AttemptPhase::Failed
                                      ? OperationKind::AttemptFailed
                                      : OperationKind::AttemptAcknowledged;
  const Status status = impl.publish(next, plan.issued_at, operation, ups, revision,
                                stored->key, reserved);
  if (!status.ok()) {
    return status;
  }
  return *find_attempt(*impl.state, reserved);
}

Result<AttemptRecord> UpsControlEngine::acknowledge(const AcknowledgeRequest& request) {
  Impl& impl = *impl_;
  Status status = validate_label(request.adapter_detail, impl.limits.max_detail_bytes);
  if (!status.ok()) {
    return status;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "an acknowledgement requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "the acknowledgement cites stale authority");
  }
  UpsState next = *impl.state;
  AttemptRecord* stored = find_attempt(next, request.attempt);
  if (stored == nullptr) {
    return Status::error(StatusCode::AttemptNotFound,
                         "no attempt " + std::to_string(request.attempt.value()) + " exists");
  }
  if (is_terminal(stored->phase)) {
    return Status::error(StatusCode::AttemptStateConflict,
                         std::string("attempt ") + std::to_string(request.attempt.value()) +
                             " is already terminal in phase " + to_string(stored->phase));
  }
  if (stored->phase != AttemptPhase::Planned && stored->phase != AttemptPhase::Issued &&
      stored->phase != AttemptPhase::Acknowledged) {
    return Status::error(StatusCode::AttemptStateConflict,
                         "the attempt is past the stage at which an acknowledgement may be recorded");
  }
  const UpsId ups = stored->ups;
  const StateRevision revision = stored->planned_revision;
  const IdempotencyKey key = stored->key;
  apply_acknowledgement(*stored, request.outcome, request.adapter_detail, request.now, impl.limits);
  if (is_terminal(stored->phase)) {
    UpsRecord* unit = find_ups(next, ups);
    if (unit != nullptr && unit->in_flight.has_value() && unit->in_flight.value() == request.attempt) {
      unit->in_flight.reset();
    }
    if (unit != nullptr && stored->phase == AttemptPhase::Failed) {
      unit->basis = StateBasis::CommandedUnverified;
    }
  }
  status = impl.publish(next, request.now, OperationKind::AttemptAcknowledged, ups, revision, key,
                   request.attempt);
  if (!status.ok()) {
    return status;
  }
  return *find_attempt(*impl.state, request.attempt);
}

Result<AttemptRecord> UpsControlEngine::verify(const VerifyRequest& request) {
  Impl& impl = *impl_;
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "a verification requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "the verification cites stale authority");
  }
  if (!impl.state->revalidated_at.is_zero() &&
      impl.state->revalidated_at.value() > request.now.value()) {
    return Status::error(StatusCode::PreconditionFailed,
                         "the verification instant precedes the last revalidation instant");
  }
  UpsState next = *impl.state;
  AttemptRecord* stored = find_attempt(next, request.attempt);
  if (stored == nullptr) {
    return Status::error(StatusCode::AttemptNotFound,
                         "no attempt " + std::to_string(request.attempt.value()) + " exists");
  }
  if (is_terminal(stored->phase)) {
    return Status::error(StatusCode::AttemptStateConflict,
                         std::string("attempt ") + std::to_string(request.attempt.value()) +
                             " is already terminal in phase " + to_string(stored->phase));
  }
  if (stored->phase != AttemptPhase::Acknowledged && stored->phase != AttemptPhase::Observed) {
    return Status::error(StatusCode::AttemptStateConflict,
                         std::string("attempt ") + std::to_string(request.attempt.value()) +
                             " is in phase " + to_string(stored->phase) +
                             " and cannot be verified yet; record telemetry or abandon it");
  }
  UpsRecord* unit = find_ups(next, stored->ups);
  if (unit == nullptr || !unit->observation.has_value()) {
    return Status::error(StatusCode::EvidenceMissing,
                         "no telemetry has been recorded for unit '" + stored->ups.value() +
                             "', so no effect can be verified");
  }
  const ObservationRecord observation = unit->observation.value();
  std::string detail;
  const VerificationVerdict verdict =
      verify_attempt(*stored, observation, request.now, default_verification_freshness(), detail);
  stored->verification = verdict;
  stored->verification_detail = bounded(detail, impl.limits);
  stored->verification_rounds += 1;
  stored->verification_evidence = observation.evidence;
  if (stored->phase == AttemptPhase::Acknowledged && observation.observed_at.value() > 0) {
    stored->phase = AttemptPhase::Observed;
    stored->observed_at = observation.observed_at;
    stored->observed_state = observation.operating;
    stored->observed_capability = observation.capability;
  }

  OperationKind operation = OperationKind::AttemptObserved;
  if (verdict == VerificationVerdict::Verified) {
    stored->phase = AttemptPhase::Verified;
    stored->verified_at = request.now;
    stored->terminal_detail = stored->verification_detail;
    unit->last_verified_at = request.now;
    unit->in_flight.reset();
    unit->basis = StateBasis::Verified;
    // The verified effect is adopted only when the observation is not older than
    // the instant the current state was established. An adopted state recorded at a
    // later instant is never rolled back by an older observation.
    if (!(unit->operating == observation.operating) && !observation.contradictory &&
        observation.operating != OperatingState::Unknown &&
        observation.observed_at.value() >= unit->state_since.value()) {
      unit->state_since = observation.observed_at;
      unit->operating = observation.operating;
    }
    operation = OperationKind::AttemptVerified;
  } else if (verdict == VerificationVerdict::Contradicted) {
    stored->phase = AttemptPhase::Failed;
    stored->terminal_detail = stored->verification_detail;
    unit->in_flight.reset();
    unit->basis = StateBasis::CommandedUnverified;
    operation = OperationKind::AttemptFailed;
  }
  const UpsId ups = stored->ups;
  const StateRevision revision = unit->revision;
  const IdempotencyKey key = stored->key;
  const Status status = impl.publish(next, request.now, operation, ups, revision, key,
                                request.attempt);
  if (!status.ok()) {
    return status;
  }
  if (operation == OperationKind::AttemptVerified) {
    impl.confirm(ups);
  }
  return *find_attempt(*impl.state, request.attempt);
}

Result<AttemptRecord> UpsControlEngine::abandon(const AbandonRequest& request) {
  Impl& impl = *impl_;
  Status status = validate_label(request.reason, impl.limits.max_detail_bytes);
  if (!status.ok()) {
    return status;
  }
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "abandoning an attempt requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "the abandonment cites stale authority");
  }
  UpsState next = *impl.state;
  AttemptRecord* stored = find_attempt(next, request.attempt);
  if (stored == nullptr) {
    return Status::error(StatusCode::AttemptNotFound,
                         "no attempt " + std::to_string(request.attempt.value()) + " exists");
  }
  if (is_terminal(stored->phase)) {
    return Status::error(StatusCode::AttemptStateConflict,
                         std::string("attempt ") + std::to_string(request.attempt.value()) +
                             " is already terminal in phase " + to_string(stored->phase));
  }
  stored->phase = AttemptPhase::Failed;
  stored->terminal_detail = request.reason.empty()
                                ? std::string("the attempt was abandoned without a verified effect")
                                : request.reason;
  UpsRecord* unit = find_ups(next, stored->ups);
  if (unit != nullptr) {
    unit->in_flight.reset();
    // The operating state is no longer trustworthy: a command reached the device
    // and its effect was never established. Only a fresh observation restores it.
    unit->basis = StateBasis::CommandedUnverified;
  }
  const UpsId ups = stored->ups;
  const StateRevision revision = unit != nullptr ? unit->revision : StateRevision{};
  const IdempotencyKey key = stored->key;
  status = impl.publish(next, request.now, OperationKind::AttemptFailed, ups, revision, key,
                   request.attempt);
  if (!status.ok()) {
    return status;
  }
  return *find_attempt(*impl.state, request.attempt);
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Result<RevalidationReport> UpsControlEngine::revalidate(const RevalidateRequest& request) {
  Impl& impl = *impl_;
  if (!is_valid_instant(request.now)) {
    return Status::error(StatusCode::InvalidArgument, "revalidation requires a positive instant");
  }
  std::lock_guard<std::mutex> guard(impl.mutex);
  const Status mutable_status = impl.ensure_mutable();
  if (!mutable_status.ok()) {
    return mutable_status;
  }
  if (!(request.authority.epoch == impl.state->epoch) ||
      !(request.authority.incarnation == impl.state->incarnation)) {
    return Status::error(StatusCode::StaleAuthority, "revalidation cites stale authority");
  }

  UpsState next = *impl.state;
  RevalidationReport report;
  report.now = request.now;
  report.generation = next.generation;
  report.unit_count = next.units.size();
  for (const UpsRecord& unit : next.units) {
    UpsRevalidation entry;
    entry.ups = unit.id;
    entry.revision = unit.revision;
    entry.operating_state_recovered = unit.basis == StateBasis::Recovered;
    const ReserveAssessment assessment =
        evaluate_reserve(std::nullopt, unit.reserve_policy,
                         unit.observation.has_value() ? &unit.observation.value() : nullptr,
                         std::nullopt, request.now, true);
    entry.reserve_freshness = assessment.freshness;
    entry.reserve_outcome = assessment.outcome;
    entry.detail = assessment.detail;
    switch (assessment.outcome) {
      case ReserveOutcome::Sufficient:
      case ReserveOutcome::NotRequired:
        ++report.units_with_fresh_reserve;
        break;
      case ReserveOutcome::Insufficient:
        ++report.units_with_insufficient_reserve;
        break;
      case ReserveOutcome::Indeterminate:
        // Staleness is a statement about the evidence; everything else that leaves
        // the reserve undecided is counted as unknown.
        if (assessment.reason == ReserveIndeterminacy::EvidenceStale ||
            assessment.reason == ReserveIndeterminacy::EvidenceFuture) {
          ++report.units_with_stale_reserve;
        } else {
          ++report.units_with_unknown_reserve;
        }
        break;
    }
    report.units.push_back(std::move(entry));
  }
  for (const AttemptRecord& record : next.attempts) {
    if (!is_terminal(record.phase)) {
      ++report.unresolved_attempts;
    }
  }
  // Revalidation is always durable, and it never moves the recorded instant
  // backwards: revalidating at an instant earlier than the one already recorded
  // leaves the later record in place, because the durable record says the state
  // was examined at that later instant and that fact does not become false.
  if (next.revalidated_at.value() < request.now.value()) {
    next.revalidated_at = request.now;
  }
  const Status status = impl.publish(next, request.now, OperationKind::Revalidated, UpsId{},
                                StateRevision{}, IdempotencyKey{}, AttemptId{});
  if (!status.ok()) {
    return status;
  }
  impl.lifecycle = EngineLifecycle::Revalidated;
  report.generation = impl.state->generation;
  return report;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

std::string describe_status(const StatusReport& report) {
  std::string text;
  text += "unit=" + report.ups.value() + "\n";
  text += "label=" + report.label + "\n";
  text += "hardware_generation=" + std::to_string(report.hardware.value()) + "\n";
  text += "epoch=" + std::to_string(report.epoch.value()) + "\n";
  text += "incarnation=" + std::to_string(report.incarnation.value()) + "\n";
  text += std::string("lifecycle=") + to_string(report.lifecycle) + "\n";
  text += std::string("operating=") + to_string(report.operating) + "\n";
  text += std::string("state_basis=") + to_string(report.basis) + "\n";
  text += "state_revision=" + std::to_string(report.revision.value()) + "\n";
  text += "state_since=" + std::to_string(report.state_since.value()) + "\n";
  text += std::string("revalidated=") + (report.revalidated ? "true" : "false") + "\n";
  text += std::string("observation_freshness=") + to_string(report.observation_freshness) + "\n";
  if (report.observation.has_value()) {
    text += "observation_evidence=" + report.observation->evidence.value() + "\n";
    text += "observation_source=" + report.observation->source.value() + "\n";
    text += std::string("observation_provenance=") + to_string(report.observation->provenance) + "\n";
    text += "observation_observed_at=" + std::to_string(report.observation->observed_at.value()) + "\n";
    text += "observation_age=" + std::to_string(report.observation_age.value()) + "\n";
    text += std::string("observation_contradictory=") +
            (report.observation->contradictory ? "true" : "false") + "\n";
    if (report.observation->battery.reserve.state == ReserveState::Known) {
      text += "reserve=" + format_reserve(report.observation->battery.reserve.quantity) + "\n";
      text += std::string("reserve_quality=") +
              to_string(report.observation->battery.reserve.quality) + "\n";
    } else {
      text += std::string("reserve=unknown(") +
              to_string(report.observation->battery.reserve.unknown_reason) + ")\n";
    }
  } else {
    text += "observation=none\n";
  }
  text += std::string("reserve_outcome=") + to_string(report.reserve.outcome) + "\n";
  text += std::string("reserve_reason=") + to_string(report.reserve.reason) + "\n";
  text += "reserve_detail=" + report.reserve.detail + "\n";
  text += "obligations=" + std::to_string(report.obligations.size()) + "\n";
  for (const ProtectedLoadObligation& obligation : report.obligations) {
    text += "obligation=" + obligation.ref.value() + " load=" + obligation.load.value() +
            " tier=" + to_string(obligation.tier) + " requirement=" + to_string(obligation.protection) +
            " state=" + to_string(obligation.state) +
            " revision=" + std::to_string(obligation.revision.value()) + "\n";
  }
  for (const AuthorityGrant& grant : report.grants) {
    text += "grant=" + grant.ref.value() + " scope=" + to_string(grant.scope) +
            " allowed=" + (grant.allowed ? "true" : "false") +
            " revoked=" + (grant.revoked_at.has_value() ? "true" : "false") + "\n";
  }
  if (report.in_flight.has_value()) {
    text += "in_flight_attempt=" + std::to_string(report.in_flight->value()) + "\n";
  }
  text += describe_readiness(report.readiness);
  return text;
}

std::string describe_revalidation(const RevalidationReport& report) {
  std::string text;
  text += "revalidated_at=" + std::to_string(report.now.value()) + "\n";
  text += "generation=" + std::to_string(report.generation.value()) + "\n";
  text += "units=" + std::to_string(report.unit_count) + "\n";
  text += "units_with_fresh_reserve=" + std::to_string(report.units_with_fresh_reserve) + "\n";
  text += "units_with_stale_reserve=" + std::to_string(report.units_with_stale_reserve) + "\n";
  text += "units_with_insufficient_reserve=" +
          std::to_string(report.units_with_insufficient_reserve) + "\n";
  text += "units_with_unknown_reserve=" + std::to_string(report.units_with_unknown_reserve) + "\n";
  text += "unresolved_attempts=" + std::to_string(report.unresolved_attempts) + "\n";
  for (const UpsRevalidation& unit : report.units) {
    text += "unit=" + unit.ups.value() + " reserve=" + to_string(unit.reserve_outcome) +
            " freshness=" + to_string(unit.reserve_freshness) +
            " recovered_operating_state=" + (unit.operating_state_recovered ? "true" : "false") +
            " detail=" + unit.detail + "\n";
  }
  return text;
}

}  // namespace ups_control
