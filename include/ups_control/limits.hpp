#pragma once

#include <cstddef>
#include <cstdint>

namespace ups_control {

/// Hard bounds applied to every external and persisted input before allocation.
///
/// Limits are part of the authority model: a declaration that exceeds a bound is
/// refused with \c StatusCode::LimitExceeded before any memory is reserved for it.
struct ResourceLimits {
  /// Largest accepted store file, checked against the on-disk size before reading.
  std::uint64_t max_store_bytes = 64ull * 1024ull * 1024ull;
  /// Largest accepted payload section, checked against the declared length.
  std::uint64_t max_payload_bytes = 64ull * 1024ull * 1024ull;
  std::size_t max_ups_units = 20000;
  std::size_t max_obligations_per_ups = 4096;
  std::size_t max_grants_per_ups = 512;
  std::size_t max_attempt_journal = 20000;
  std::size_t max_idempotency_records = 20000;
  std::size_t max_commit_log = 4096;
  std::size_t max_findings = 256;
  std::size_t max_obligations_at_risk = 4096;
  std::size_t max_identifier_bytes = 128;
  std::size_t max_label_bytes = 256;
  std::size_t max_detail_bytes = 512;
  std::size_t max_path_bytes = 4096;
  std::size_t max_idempotency_key_bytes = 64;
  std::size_t max_history_results = 4096;
  /// Largest accepted reserve magnitude in the smallest supported reserve unit
  /// (milliwatt-hours) and the largest accepted duration in ticks.
  std::int64_t max_reserve_milliwatt_hours = 1'000'000'000'000'000;
  std::int64_t max_reserve_seconds = 1'000'000'000'000;
  std::int64_t max_tick_span = 1'000'000'000'000;

  static ResourceLimits defaults() { return ResourceLimits{}; }
};

}  // namespace ups_control
