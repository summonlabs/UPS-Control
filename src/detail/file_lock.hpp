#pragma once

// Internal cross-process exclusive writer lock. Not installed.
//
// The lock is an operating-system level guarantee, not a convention: on Windows
// the lock file is opened with share mode zero, and on POSIX it is held with
// `flock(LOCK_EX | LOCK_NB)`. Either way the kernel releases the lock when the
// holding process exits, including abnormal exit, so process death relinquishes
// writer authority without any recovery step.

#include <filesystem>

#include "ups_control/status.hpp"

namespace ups_control::detail {

class ExclusiveFileLock {
 public:
  ExclusiveFileLock() = default;
  ~ExclusiveFileLock();
  ExclusiveFileLock(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
  ExclusiveFileLock(ExclusiveFileLock&& other) noexcept;
  ExclusiveFileLock& operator=(ExclusiveFileLock&& other) noexcept;

  /// Acquires the lock or fails with `StatusCode::LockConflict` when another
  /// process or handle holds it.
  static Result<ExclusiveFileLock> acquire(const std::filesystem::path& path);

  Status release();
  bool held() const noexcept { return descriptor_ >= 0; }
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  int descriptor_ = -1;
  std::filesystem::path path_;
};

}  // namespace ups_control::detail
