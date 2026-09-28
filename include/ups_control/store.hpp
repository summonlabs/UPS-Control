#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/state.hpp"
#include "ups_control/status.hpp"

namespace ups_control {

enum class StoreAccess : std::uint8_t { ReadOnly = 0, ReadWrite = 1 };

/// A durable stage at which the current process may be terminated, so that crash
/// behaviour can be exercised against real process death.
///
/// This exists solely so the crash-recovery proof obligations can be discharged.
/// Any value other than \c None terminates the current process at the named
/// stage, without unwinding, without a signal handler, and without any modal or
/// interactive error path. A production caller never sets it.
enum class DurableCrashPoint : std::uint8_t {
  None = 0,
  /// Before the staging payload is created.
  BeforeStagingWrite = 1,
  /// After the staging payload bytes are written, before they are flushed.
  AfterStagingWrite = 2,
  /// After the staging payload is flushed to the device, before read-back.
  AfterStagingFlush = 3,
  /// After the staging payload has been read back and verified, before the
  /// authoritative head is published.
  AfterStagingVerify = 4,
  /// After the head marker has been durably published, before retired residue is
  /// removed.
  AfterHeadCommit = 5,
};

const char* to_string(DurableCrashPoint point) noexcept;

/// The complete set of preconditions for reading a store.
struct StoreReadOptions {
  ResourceLimits limits = ResourceLimits::defaults();
  /// When present, the store must carry exactly this identity or the read fails
  /// with \c StatusCode::StaleAuthority. This is the primary defense against
  /// silently adopting a swapped or unrelated store.
  std::optional<StoreIdentity> expected_store_identity;
  /// When present, the store must be committed at or above this generation or the
  /// read fails with \c StatusCode::StaleGeneration. The head marker already
  /// names one generation; this is the durable floor that detects a wholesale
  /// replacement of the store by an older copy of itself.
  std::optional<StoreGeneration> minimum_generation;
  /// When true, refuse to read a store whose recorded creation path differs from
  /// the path being read, with \c StatusCode::Conflict.
  bool enforce_path_binding = false;
};

/// The complete set of preconditions for opening a store.
struct StoreOpenOptions {
  std::filesystem::path path;
  StoreAccess access = StoreAccess::ReadWrite;
  /// Create the store when it does not exist. With \c ReadOnly access a missing
  /// store is always \c NotFound. A missing parent directory is never created
  /// implicitly.
  bool create_if_missing = false;
  std::optional<StoreIdentity> expected_store_identity;
  std::optional<StoreGeneration> minimum_generation;
  bool enforce_path_binding = false;
  /// Recorded in the store when it is created. Ignored for an existing store.
  Tick created_at;
  /// Authority binding recorded in the store when it is created. Ignored for an
  /// existing store.
  ControlEpoch epoch;
  Incarnation incarnation;
  /// Validate the state after every read and before every commit. Enabled by
  /// default; it is the check that a malformed but well-checksummed artifact
  /// cannot be adopted.
  bool validate_state = true;
  ResourceLimits limits = ResourceLimits::defaults();

  // --- durable-stage crash injection (test-only; see DurableCrashPoint) ---
  DurableCrashPoint crash_at = DurableCrashPoint::None;
  /// Terminate at \c crash_at during the n-th commit performed by this handle.
  /// Zero disables the counter, so every commit terminates.
  std::uint64_t crash_after_commits = 0;
};

/// Inspection view of an open store.
struct StoreInfo {
  std::filesystem::path path;
  StoreIdentity store_identity;
  StoreGeneration generation;
  ControlEpoch epoch;
  Incarnation incarnation;
  /// Monotonic commit ordinal recorded in the head marker.
  std::uint64_t head_sequence = 0;
  Tick created_at;
  Tick updated_at;
  Tick revalidated_at;
  Tick committed_at;
  std::string recorded_path;
  bool path_binding_matches = true;
  bool writable = false;
  bool created = false;
  std::uint32_t format_version = 0;
  std::uint64_t head_bytes = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc32c = 0;
  std::size_t unit_count = 0;
  std::size_t attempt_count = 0;
  std::size_t idempotency_count = 0;
  std::uint64_t commit_count = 0;
};

/// Timings of one durable commit, in nanoseconds, from \c std::chrono::steady_clock.
/// These are local measurements for the caller's own diagnostics and benchmarks,
/// and are not persisted and not transmitted anywhere.
struct CommitMetrics {
  std::uint64_t encode_nanos = 0;
  std::uint64_t staging_write_nanos = 0;
  std::uint64_t verify_nanos = 0;
  std::uint64_t publish_nanos = 0;
  std::uint64_t cleanup_nanos = 0;
  std::uint64_t total_nanos = 0;
  std::uint64_t bytes_written = 0;
};

struct CommitResult {
  StoreGeneration generation;
  AttemptId attempt;
  CommitMetrics metrics;
  /// True when the same idempotency key had already been applied and the commit
  /// was therefore skipped.
  bool already_applied = false;
};

/// UTF-8 rendering of a path, safe for any path the platform accepts.
std::string path_to_utf8(const std::filesystem::path& path);

/// Versioned, integrity-checked persistence for one UPS control state.
///
/// The store is a pair of files that share the caller's path:
///
///   * the head marker at \c path, a fixed 128-byte checksummed record naming the
///     committed generation and binding the payload by length and checksum;
///   * the generation payload at \c path + ".g" + generation in hexadecimal,
///     which carries the canonical logical state.
///
/// Commit protocol, in order, with the commit point stated explicitly:
///
///   plan -> validate -> reserve generation -> write staging payload ->
///   flush staging to the device -> read the staging payload back and verify it
///   -> publish the generation payload under its generation name ->
///   atomically replace the head marker  <-- THE COMMIT POINT ->
///   retire the superseded payload and any staging residue.
///
/// Nothing is visible at the store path until the head marker is replaced, and no
/// state is published in memory unless that replacement succeeded. After a crash
/// the head marker names exactly one generation; the payload it names must verify
/// in full, or the open is refused. A partial generation is never stitched
/// together with a whole one, and recovery never falls back to an older
/// generation.
class UpsStore {
 public:
  UpsStore() = default;
  ~UpsStore();
  UpsStore(const UpsStore&) = delete;
  UpsStore& operator=(const UpsStore&) = delete;
  UpsStore(UpsStore&&) noexcept;
  UpsStore& operator=(UpsStore&&) noexcept;

  /// Opens or creates a store. A read-write open takes an operating-system level
  /// exclusive lock, so a second read-write open of the same path fails with
  /// \c StatusCode::LockConflict. The operating system releases the lock when the
  /// holding process exits, including abnormal exit.
  static Result<std::shared_ptr<UpsStore>> open(const StoreOpenOptions& options);

  /// Strict read path used by \c open, \c verify_file, and every inspection
  /// tool. Rejects truncated, oversized, wrong-version, wrong-endian, corrupt, and
  /// structurally invalid artifacts, and enforces the caller's identity, path
  /// binding, and generation floor.
  static Result<std::shared_ptr<const UpsState>> read_file(const std::filesystem::path& path,
                                                           const StoreReadOptions& options);

  /// Equivalent to \c read_file, with the same strictness, plus a structural
  /// state validation. Provided so that audit and inspection commands cannot
  /// drift away from the normal read path.
  static Status verify_file(const std::filesystem::path& path,
                            const StoreReadOptions& options = StoreReadOptions{});

  const StoreInfo& info() const noexcept { return info_; }
  bool writable() const noexcept { return info_.writable; }
  bool is_open() const noexcept { return open_; }
  const std::filesystem::path& path() const noexcept { return path_; }

  /// The state that was read when this store was opened. It is the exact snapshot
  /// the store's own metadata describes, so a caller that needs both does not have
  /// to read the file a second time and cannot observe a different generation in
  /// each. It is never updated by a later commit; the engine owns the published
  /// state.
  const std::shared_ptr<const UpsState>& opened_state() const noexcept { return opened_state_; }

  /// Durably publishes a new state. The state's generation must be exactly one
  /// greater than the current committed generation, its store identity must
  /// match, and its epoch and incarnation must not move backwards. On any failure
  /// before the head replacement the previous committed state is untouched and
  /// the in-memory state is not published by the caller.
  Result<CommitResult> commit(const UpsState& state);

  /// Releases the writer lock. Idempotent.
  Status close();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  std::shared_ptr<const UpsState> opened_state_;
  StoreInfo info_;
  std::filesystem::path path_;
  bool open_ = false;
};

}  // namespace ups_control
