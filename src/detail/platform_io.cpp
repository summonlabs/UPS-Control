#include "detail/platform_io.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "detail/crash.hpp"

namespace ups_control::detail {
namespace {

Status error_from_errno(const std::filesystem::path& path, const char* what, int error) {
  StatusCode code = StatusCode::IoFailure;
  if (error == EACCES || error == EPERM) {
    code = StatusCode::PermissionDenied;
  } else if (error == ENOENT) {
    code = StatusCode::NotFound;
  }
  return Status::error(code, std::string(what) + " '" + path_utf8(path) + "' failed: " +
                                 std::generic_category().message(error));
}

}  // namespace

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string path_utf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    if (error.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
      return Status::error(StatusCode::NotFound, "file not found: '" + path_utf8(path) + "'");
    }
    return Status::error(StatusCode::IoFailure,
                         "could not read the size of '" + path_utf8(path) + "': " + error.message());
  }
  return static_cast<std::uint64_t>(size);
}

Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                 std::uint64_t max_bytes) {
  const Result<std::uint64_t> size = ups_control::detail::file_size(path);
  if (!size.ok()) {
    return size.status();
  }
  if (size.value() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact '" + path_utf8(path) + "' is " + std::to_string(size.value()) +
                             " bytes, above the configured limit of " +
                             std::to_string(max_bytes) + " bytes");
  }

  std::vector<std::byte> buffer(static_cast<std::size_t>(size.value()));
#if defined(_WIN32)
  FILE* file = nullptr;
  const errno_t open_error = _wfopen_s(&file, path.wstring().c_str(), L"rb");
  if (open_error != 0 || file == nullptr) {
    return error_from_errno(path, "opening", open_error == 0 ? EIO : open_error);
  }
  std::size_t read_bytes = 0;
  if (!buffer.empty()) {
    read_bytes = std::fread(buffer.data(), 1, buffer.size(), file);
  }
  const bool short_read = read_bytes != buffer.size();
  const int read_error = short_read ? errno : 0;
  std::fclose(file);
  if (short_read) {
    return error_from_errno(path, "reading", read_error == 0 ? EIO : read_error);
  }
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return error_from_errno(path, "opening", errno);
  }
  std::size_t total = 0;
  while (total < buffer.size()) {
    const ssize_t read_bytes = ::read(descriptor, buffer.data() + total, buffer.size() - total);
    if (read_bytes < 0) {
      const int read_error = errno;
      ::close(descriptor);
      return error_from_errno(path, "reading", read_error);
    }
    if (read_bytes == 0) {
      ::close(descriptor);
      return Status::error(StatusCode::Corruption,
                           "artifact '" + path_utf8(path) + "' shrank while being read");
    }
    total += static_cast<std::size_t>(read_bytes);
  }
  ::close(descriptor);
#endif
  return buffer;
}

Status write_file(const std::filesystem::path& path, std::span<const std::byte> data,
                  std::uint64_t max_bytes, bool flush_to_device) {
  if (data.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "refusing to write " + std::to_string(data.size()) +
                             " bytes, above the configured limit of " +
                             std::to_string(max_bytes) + " bytes");
  }
#if defined(_WIN32)
  FILE* file = nullptr;
  const errno_t open_error = _wfopen_s(&file, path.wstring().c_str(), L"wb");
  if (open_error != 0 || file == nullptr) {
    return error_from_errno(path, "creating", open_error == 0 ? EIO : open_error);
  }
  if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file) != data.size()) {
    const int write_error = errno;
    std::fclose(file);
    remove_file(path);
    return error_from_errno(path, "writing", write_error == 0 ? EIO : write_error);
  }
  if (!flush_to_device) {
    if (std::fclose(file) != 0) {
      const int close_error = errno;
      remove_file(path);
      return error_from_errno(path, "closing", close_error == 0 ? EIO : close_error);
    }
    return Status::success();
  }
  if (std::fflush(file) != 0) {
    const int flush_error = errno;
    std::fclose(file);
    remove_file(path);
    return error_from_errno(path, "flushing", flush_error == 0 ? EIO : flush_error);
  }
  if (_commit(_fileno(file)) != 0) {
    const int commit_error = errno;
    std::fclose(file);
    remove_file(path);
    return error_from_errno(path, "committing", commit_error == 0 ? EIO : commit_error);
  }
  if (std::fclose(file) != 0) {
    const int close_error = errno;
    remove_file(path);
    return error_from_errno(path, "closing", close_error == 0 ? EIO : close_error);
  }
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    return error_from_errno(path, "creating", errno);
  }
  std::size_t total = 0;
  while (total < data.size()) {
    const ssize_t written = ::write(descriptor, data.data() + total, data.size() - total);
    if (written < 0) {
      const int write_error = errno;
      ::close(descriptor);
      remove_file(path);
      return error_from_errno(path, "writing", write_error);
    }
    total += static_cast<std::size_t>(written);
  }
  if (flush_to_device && ::fsync(descriptor) != 0) {
    const int sync_error = errno;
    ::close(descriptor);
    remove_file(path);
    return error_from_errno(path, "flushing", sync_error);
  }
  if (::close(descriptor) != 0) {
    const int close_error = errno;
    remove_file(path);
    return error_from_errno(path, "closing", close_error);
  }
#endif
  return Status::success();
}

Status write_file_durable(const std::filesystem::path& path, std::span<const std::byte> data,
                          std::uint64_t max_bytes) {
  return write_file(path, data, max_bytes, true);
}

Status flush_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  FILE* file = nullptr;
  const errno_t open_error = _wfopen_s(&file, path.wstring().c_str(), L"r+b");
  if (open_error != 0 || file == nullptr) {
    return error_from_errno(path, "opening for flush", open_error == 0 ? EIO : open_error);
  }
  if (std::fflush(file) != 0) {
    const int flush_error = errno;
    std::fclose(file);
    return error_from_errno(path, "flushing", flush_error == 0 ? EIO : flush_error);
  }
  if (_commit(_fileno(file)) != 0) {
    const int commit_error = errno;
    std::fclose(file);
    return error_from_errno(path, "committing", commit_error == 0 ? EIO : commit_error);
  }
  if (std::fclose(file) != 0) {
    const int close_error = errno;
    return error_from_errno(path, "closing", close_error == 0 ? EIO : close_error);
  }
#else
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (descriptor < 0) {
    return error_from_errno(path, "opening for flush", errno);
  }
  if (::fsync(descriptor) != 0) {
    const int sync_error = errno;
    ::close(descriptor);
    return error_from_errno(path, "flushing", sync_error);
  }
  if (::close(descriptor) != 0) {
    const int close_error = errno;
    return error_from_errno(path, "closing", close_error);
  }
#endif
  return Status::success();
}

Status sync_directory(const std::filesystem::path& directory) {
#if defined(_WIN32)
  (void)directory;
  return Status::error(StatusCode::Unsupported,
                       "directory flush is not available on this platform without opening a "
                       "directory handle with backup semantics");
#else
  const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return error_from_errno(directory, "opening directory", errno);
  }
  const int result = ::fsync(descriptor);
  const int sync_error = errno;
  ::close(descriptor);
  if (result != 0) {
    return error_from_errno(directory, "flushing directory", sync_error);
  }
  return Status::success();
#endif
}

Status atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::rename(from, to, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "atomic publish of '" + path_utf8(to) + "' from '" + path_utf8(from) +
                             "' failed: " + error.message());
  }
  return Status::success();
}

Status remove_file(const std::filesystem::path& path) noexcept {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error) {
    return Status::error(StatusCode::IoFailure,
                         "could not remove '" + path_utf8(path) + "': " + error.message());
  }
  if (!removed) {
    return Status::error(StatusCode::NotFound, "nothing to remove at '" + path_utf8(path) + "'");
  }
  return Status::success();
}

namespace {

/// True for a Windows reserved device name, with or without an extension. These
/// names address a device rather than a file on every Windows version that still
/// honours the legacy namespace, so a store must never be published through one.
bool is_reserved_device_name(const std::string& name) {
  std::string stem = name;
  const std::size_t dot = stem.find('.');
  if (dot != std::string::npos) {
    stem = stem.substr(0, dot);
  }
  for (char& character : stem) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$") {
    return true;
  }
  if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)) {
    return stem[3] >= '1' && stem[3] <= '9';
  }
  return false;
}

#if defined(_WIN32)
bool has_reparse_attribute(const std::filesystem::path& path) {
  const DWORD attributes = ::GetFileAttributesW(path.wstring().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#endif

}  // namespace

Status validate_raw_path(const std::filesystem::path& target, std::size_t max_bytes) {
  if (target.empty()) {
    return Status::error(StatusCode::InvalidArgument, "a store path must not be empty");
  }
  const std::string text = path_utf8(target);
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a store path must not be empty once encoded as UTF-8");
  }
  if (text.find('\0') != std::string::npos) {
    return Status::error(StatusCode::InvalidArgument,
                         "a store path must not contain an embedded NUL; the operating system would "
                         "truncate it at that point");
  }
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "the store path is " + std::to_string(text.size()) +
                             " bytes, above the configured limit of " + std::to_string(max_bytes));
  }
  return Status::success();
}

Result<std::filesystem::path> canonical_store_path(const std::filesystem::path& target) {
  std::error_code error;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(target, error);
  if (error || resolved.empty()) {
    error.clear();
    resolved = std::filesystem::absolute(target, error);
    if (error || resolved.empty()) {
      return Status::error(StatusCode::IoFailure,
                           "could not resolve '" + path_utf8(target) + "' to an absolute path: " +
                               error.message());
    }
    resolved = resolved.lexically_normal();
  }
  return resolved;
}

std::filesystem::path derived_path(const std::filesystem::path& target, std::string_view suffix) {
  std::filesystem::path result = target;
  std::string name = path_utf8(target.filename());
  name.append(suffix.begin(), suffix.end());
  result.replace_filename(std::filesystem::path(std::u8string(name.begin(), name.end())));
  return result;
}

void terminate_process_now(int exit_code) noexcept {
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exit_code));
  // TerminateProcess is asynchronous with respect to this thread only in the
  // sense that it never returns control to it; the exit below is unreachable and
  // exists so that the function is well formed if the call is ever intercepted.
  ::_exit(exit_code);
#else
  ::_exit(exit_code);
#endif
}

void maybe_crash(const CrashInjection& injection, DurableCrashPoint point,
                 std::uint64_t commit_ordinal) noexcept {
  if (!injection.armed() || injection.point != point) {
    return;
  }
  if (injection.after_commits != 0 && injection.after_commits != commit_ordinal) {
    return;
  }
  terminate_process_now(kCrashExitCode);
}

Status validate_store_path(const std::filesystem::path& target) {
  if (target.empty() || !target.has_filename()) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path must name a file, got '" + path_utf8(target) + "'");
  }
  if (is_reserved_device_name(path_utf8(target.filename()))) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) +
                             "' names a reserved platform device; refusing to use it as a file");
  }
  const std::filesystem::path parent = target.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(parent, error);
    if (error) {
      return Status::error(StatusCode::NotFound,
                           "store directory '" + path_utf8(parent) + "' is not accessible: " +
                               error.message());
    }
    if (!std::filesystem::is_directory(status)) {
      return Status::error(StatusCode::InvalidArgument,
                           "store directory '" + path_utf8(parent) + "' is not a directory");
    }
  }

  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(target, error);
  if (error) {
    if (error.value() == static_cast<int>(std::errc::no_such_file_or_directory)) {
      return Status::success();
    }
    return Status::error(StatusCode::IoFailure,
                         "could not inspect '" + path_utf8(target) + "': " + error.message());
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return Status::success();
  }
  if (std::filesystem::is_symlink(status)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) +
                             "' is a symbolic link; refusing to publish through it");
  }
#if defined(_WIN32)
  if (has_reparse_attribute(target)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) +
                             "' carries a reparse attribute (a junction, mount point, or other "
                             "name surrogate); refusing to publish through it");
  }
#endif
  if (std::filesystem::is_directory(status)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) + "' is a directory");
  }
  if (!std::filesystem::is_regular_file(status)) {
    return Status::error(StatusCode::InvalidArgument,
                         "store path '" + path_utf8(target) + "' is not a regular file");
  }
  return Status::success();
}

}  // namespace ups_control::detail
