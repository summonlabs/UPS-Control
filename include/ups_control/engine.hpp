#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ups_control/adapter.hpp"
#include "ups_control/authority.hpp"
#include "ups_control/battery.hpp"
#include "ups_control/command.hpp"
#include "ups_control/evaluation.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/obligation.hpp"
#include "ups_control/state.hpp"
#include "ups_control/store.hpp"
#include "ups_control/telemetry.hpp"
#include "ups_control/ups.hpp"

namespace ups_control {

struct EngineOpenOptions {
  StoreOpenOptions store;
  /// Adapter bound to this engine. Absent means no adapter, so every control
  /// attempt is refused with \c RefusalCode::AdapterUnavailable. There is no
  /// implicit adapter and no hidden autonomous control path.
  std::shared_ptr<UpsAdapter> adapter;
};

/// Where the engine is in its own lifecycle.
///
/// A freshly opened engine is always \c Recovered, never \c Revalidated: state
/// read from a store is not current evidence, and no control decision may rest on
/// it until \c revalidate has been called with an explicit instant.
enum class EngineLifecycle : std::uint8_t {
  Unopened = 0,
  Recovered = 1,
  Revalidated = 2,
  Closed = 3,
};

const char* to_string(EngineLifecycle lifecycle) noexcept;

struct EngineInfo {
  std::filesystem::path path;
  SessionId session;
  StoreIdentity store_identity;
  StoreGeneration generation;
  ControlEpoch epoch;
  Incarnation incarnation;
  EngineLifecycle lifecycle = EngineLifecycle::Unopened;
  Tick revalidated_at;
  Tick created_at;
  Tick updated_at;
  std::size_t unit_count = 0;
  std::size_t attempt_count = 0;
  std::size_t unresolved_attempt_count = 0;
  std::uint64_t commit_count = 0;
  bool writable = false;
  bool adapter_bound = false;
  AdapterCapabilities adapter;
  EvidenceClass evidence = EvidenceClass::Unsupported;
};

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
//
// Every request that depends on current state carries the authority it was
// planned against (\c ControlContext) and, where it targets one device, the
// exact hardware generation and state revision (\c UpsRef). A mismatched
// generation, epoch, incarnation, or revision is refused; it is never merged.

struct RegisterUpsRequest {
  ControlContext authority;
  Tick now;
  UpsId id;
  std::string label;
  HardwareGeneration hardware;
  LifecycleState lifecycle = LifecycleState::Standby;
  OperatingState operating = OperatingState::Unknown;
  ReservePolicy policy;
  std::string note;
};

struct RecordTelemetryRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  TelemetryReport report;
};

/// Adopts an externally validated operating state, for example after service.
/// This is the only way a state leaves \c Unknown, and it is recorded as an
/// adopted observation rather than a control effect.
struct AdoptOperatingStateRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  OperatingState operating = OperatingState::Unknown;
  std::string reason;
};

struct SetLifecycleRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  LifecycleState lifecycle = LifecycleState::Standby;
  std::string reason;
};

struct SetReservePolicyRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  ReservePolicy policy;
};

struct BindObligationRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  ProtectedLoadObligation obligation;
};

struct ReleaseObligationRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  ObligationRef obligation;
  /// The revision of the binding the caller read. A mismatch is refused so that a
  /// release cannot be applied to a binding that changed since it was read.
  ObligationRevision obligation_revision;
  AuthorityRef release_authority;
  std::string reason;
  /// Either \c Released (the protection requirement is permanently withdrawn and
  /// remains in the audit trail) or \c Suspended (temporarily not in force).
  /// \c Active is refused: a release never reinstates a binding.
  ObligationState outcome = ObligationState::Released;
};

struct IssueGrantRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  AuthorityGrant grant;
};

struct RevokeGrantRequest {
  ControlContext authority;
  UpsRef ref;
  Tick now;
  AuthorityRef grant;
  AuthorityRef revocation_authority;
  std::string reason;
};

/// Moves the store to a new control epoch or controller incarnation.
///
/// The move must be strictly forward: a greater epoch, or the same epoch and a
/// greater incarnation. An equal or older pair is refused with
/// \c StatusCode::StaleAuthority, which is the rollback fence for authority.
struct AdoptAuthorityRequest {
  ControlContext authority;
  Tick now;
  ControlEpoch epoch;
  Incarnation incarnation;
};

struct AcknowledgeRequest {
  ControlContext authority;
  Tick now;
  AttemptId attempt;
  AckOutcome outcome = AckOutcome::Accepted;
  std::string adapter_detail;
};

struct VerifyRequest {
  ControlContext authority;
  Tick now;
  AttemptId attempt;
};

struct AbandonRequest {
  ControlContext authority;
  Tick now;
  AttemptId attempt;
  std::string reason;
};

struct RevalidateRequest {
  ControlContext authority;
  Tick now;
};

struct UpsQuery {
  UpsId ups;
  Tick now;
};

struct HistoryQuery {
  std::optional<UpsId> ups;
  std::optional<AttemptPhase> phase;
  /// Maximum entries returned. Bounded by \c ResourceLimits::max_history_results.
  std::size_t limit = 256;
};

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

struct UpsRevalidation {
  UpsId ups;
  StateRevision revision;
  FreshnessVerdict reserve_freshness = FreshnessVerdict::NeverObserved;
  ReserveOutcome reserve_outcome = ReserveOutcome::Indeterminate;
  bool operating_state_recovered = true;
  std::string detail;
};

struct RevalidationReport {
  Tick now;
  StoreGeneration generation;
  std::size_t unit_count = 0;
  /// Units whose reserve precondition is satisfied, or for which no floor is
  /// declared at all.
  std::size_t units_with_fresh_reserve = 0;
  /// Units whose reserve could not be decided because the evidence is outside its
  /// freshness window. This is a statement about the evidence, not about the
  /// reserve.
  std::size_t units_with_stale_reserve = 0;
  /// Units whose reserve is known and below the declared floor.
  std::size_t units_with_insufficient_reserve = 0;
  /// Units whose reserve could not be decided for any other reason: no evidence,
  /// an explicit unknown, incomparable units, or evidence below the required
  /// quality.
  std::size_t units_with_unknown_reserve = 0;
  std::size_t unresolved_attempts = 0;
  std::vector<UpsRevalidation> units;
};

struct StatusReport {
  UpsId ups;
  std::string label;
  HardwareGeneration hardware;
  ControlEpoch epoch;
  Incarnation incarnation;
  LifecycleState lifecycle = LifecycleState::Commissioning;
  OperatingState operating = OperatingState::Unknown;
  StateBasis basis = StateBasis::Recovered;
  StateRevision revision;
  Tick state_since;
  Tick registered_at;
  Tick last_verified_at;
  bool revalidated = false;

  std::optional<ObservationRecord> observation;
  FreshnessVerdict observation_freshness = FreshnessVerdict::NeverObserved;
  TickSpan observation_age;
  ReserveAssessment reserve;

  ReservePolicy policy;
  std::vector<ProtectedLoadObligation> obligations;
  std::vector<AuthorityGrant> grants;
  std::optional<AttemptId> in_flight;
  std::optional<AttemptRecord> in_flight_attempt;
  ReadinessReport readiness;
};

struct StoreAuditReport {
  std::filesystem::path path;
  StoreIdentity store_identity;
  bool identity_present = false;
  StoreGeneration generation;
  ControlEpoch epoch;
  Incarnation incarnation;
  Tick created_at;
  Tick updated_at;
  Tick revalidated_at;
  std::uint32_t format_version = 0;
  std::uint64_t head_bytes = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc32c = 0;
  std::string recorded_path;
  bool path_binding_matches = false;
  std::size_t unit_count = 0;
  std::size_t attempt_count = 0;
  std::size_t unresolved_attempt_count = 0;
  std::size_t terminal_attempt_count = 0;
  std::size_t idempotency_count = 0;
  std::uint64_t operation_count = 0;
  std::uint64_t canonical_digest = 0;
  bool writable = false;
  std::vector<CommitLogEntry> commit_log;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

/// The UPS control engine.
///
/// Concurrency model. The engine owns exactly one mutex and one OS-level writer
/// lock. The mutex protects the in-memory authoritative state and the attempt
/// journal; it is never held across an adapter call, so no callback or user code
/// runs under it. Read paths copy an immutable snapshot under the mutex and then
/// compute outside it. The writer lock is taken once at open and released at
/// close; it is never nested inside the mutex, and the mutex is never taken while
/// the writer lock is being acquired. There is therefore a single lock level in
/// this runtime.
///
/// The engine is safe to call from several threads. It spawns no threads and uses
/// no background work.
class UpsControlEngine {
 public:
  ~UpsControlEngine();
  UpsControlEngine(const UpsControlEngine&) = delete;
  UpsControlEngine& operator=(const UpsControlEngine&) = delete;

  /// Opens a store and recovers its authoritative state. The returned engine is
  /// in \c EngineLifecycle::Recovered; call \c revalidate before any control
  /// decision.
  static Result<std::shared_ptr<UpsControlEngine>> open(const EngineOpenOptions& options);

  Status close();
  bool is_open() const noexcept;
  EngineInfo info() const;
  const ResourceLimits& limits() const noexcept;

  // --- registration and model mutation ---
  Result<UpsRecord> register_ups(const RegisterUpsRequest& request);
  Result<UpsRecord> set_lifecycle(const SetLifecycleRequest& request);
  Result<UpsRecord> adopt_operating_state(const AdoptOperatingStateRequest& request);
  Result<UpsRecord> set_reserve_policy(const SetReservePolicyRequest& request);

  // --- authority ---
  Result<ControlContext> adopt_authority(const AdoptAuthorityRequest& request);
  Result<AuthorityGrant> issue_grant(const IssueGrantRequest& request);
  Result<AuthorityGrant> revoke_grant(const RevokeGrantRequest& request);

  // --- protected-load obligations ---
  Result<ProtectedLoadObligation> bind_obligation(const BindObligationRequest& request);
  Result<ProtectedLoadObligation> release_obligation(const ReleaseObligationRequest& request);

  // --- telemetry ---
  Result<ObservationRecord> record_telemetry(const RecordTelemetryRequest& request);

  // --- read paths ---
  Result<StatusReport> status(const UpsQuery& query) const;
  /// Every registered unit, in identifier order.
  Result<std::vector<UpsRecord>> units() const;
  Result<ReadinessReport> readiness(const UpsQuery& query) const;
  Result<ReserveAssessment> battery(const UpsQuery& query) const;
  Result<std::vector<ProtectedLoadObligation>> obligations(const UpsQuery& query) const;
  Result<TransitionEvaluation> evaluate(const ControlCommand& command) const;
  Result<AttemptRecord> attempt(AttemptId id) const;
  Result<AttemptRecord> replay(const IdempotencyKey& key) const;
  Result<std::vector<AttemptRecord>> history(const HistoryQuery& query) const;
  Result<StoreAuditReport> store_audit() const;

  // --- control attempts ---
  //
  // \c submit records the plan durably, evaluates it, records a refusal, or
  // issues the command to the adapter and records the acknowledgement. It never
  // reports an effect: the effective state is established only by
  // \c record_telemetry followed by \c verify.
  Result<AttemptRecord> submit(const ControlCommand& command);
  Result<AttemptRecord> acknowledge(const AcknowledgeRequest& request);
  Result<AttemptRecord> verify(const VerifyRequest& request);
  Result<AttemptRecord> abandon(const AbandonRequest& request);

  // --- recovery ---
  Result<RevalidationReport> revalidate(const RevalidateRequest& request);

 private:
  struct Impl;
  explicit UpsControlEngine(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

/// Renders a status report as stable, line-oriented audit text.
std::string describe_status(const StatusReport& report);
/// Renders a revalidation report as stable, line-oriented audit text.
std::string describe_revalidation(const RevalidationReport& report);

}  // namespace ups_control
