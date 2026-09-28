#include "detail/file_lock.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "detail/platform_io.hpp"

namespace ups_control::detail {
namespace {

std::string describe_pid() {
  return "pid=" + std::to_string(current_process_id()) + "\n";
}

/// Best-effort record of who holds the lock, for inspection. A failure to write
/// the annotation never affects whether the lock is held.
void annotate(int descriptor) {
  const std::string text = describe_pid();
#if defined(_WIN32)
  if (_chsize_s(descriptor, 0) != 0) {
    return;
  }
  if (_write(descriptor, text.data(), static_cast<unsigned>(text.size())) < 0) {
    return;
  }
#else
  if (::ftruncate(descriptor, 0) != 0) {
    return;
  }
  const ssize_t written = ::write(descriptor, text.data(), text.size());
  (void)written;
#endif
}

}  // namespace

ExclusiveFileLock::~ExclusiveFileLock() {
  if (descriptor_ >= 0) {
    release();
  }
}

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : descriptor_(other.descriptor_), path_(std::move(other.path_)) {
  other.descriptor_ = -1;
  other.path_.clear();
}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0) {
      release();
    }
    descriptor_ = other.descriptor_;
    path_ = std::move(other.path_);
    other.descriptor_ = -1;
    other.path_.clear();
  }
  return *this;
}

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& path) {
  if (path.empty()) {
    return Status::error(StatusCode::InvalidArgument, "lock path must not be empty");
  }
  ExclusiveFileLock lock;
  lock.path_ = path;

#if defined(_WIN32)
  int descriptor = -1;
  const errno_t open_error = _wsopen_s(&descriptor, path.wstring().c_str(),
                                       _O_CREAT | _O_RDWR | _O_BINARY, _SH_DENYRW,
                                       _S_IREAD | _S_IWRITE);
  if (open_error != 0 || descriptor < 0) {
    if (open_error == EACCES || open_error == EPERM) {
      return Status::error(StatusCode::LockConflict,
                           "another process holds writer authority for '" + path_utf8(path) + "'");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not open the lock file '" + path_utf8(path) +
                             "': " + std::generic_category().message(open_error == 0 ? EIO : open_error));
  }
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return Status::error(errno == EACCES ? StatusCode::PermissionDenied : StatusCode::IoFailure,
                         "could not open the lock file '" + path_utf8(path) +
                             "': " + std::generic_category().message(errno));
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int lock_error = errno;
    ::close(descriptor);
    if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
      return Status::error(StatusCode::LockConflict,
                           "another process holds writer authority for '" + path_utf8(path) + "'");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not lock '" + path_utf8(path) + "': " + std::generic_category().message(lock_error));
  }
#endif

  lock.descriptor_ = descriptor;
  annotate(descriptor);
  return lock;
}

Status ExclusiveFileLock::release() {
  if (descriptor_ < 0) {
    return Status::success();
  }
#if defined(_WIN32)
  const int descriptor = descriptor_;
  descriptor_ = -1;
  if (_close(descriptor) != 0) {
    return Status::error(StatusCode::IoFailure,
                         "could not close the lock file '" + path_utf8(path_) + "'");
  }
#else
  const int descriptor = descriptor_;
  descriptor_ = -1;
  if (::flock(descriptor, LOCK_UN) != 0) {
    ::close(descriptor);
    return Status::error(StatusCode::IoFailure,
                         "could not unlock '" + path_utf8(path_) + "'");
  }
  if (::close(descriptor) != 0) {
    return Status::error(StatusCode::IoFailure,
                         "could not close the lock file '" + path_utf8(path_) + "'");
  }
#endif
  return Status::success();
}

}  // namespace ups_control::detail
