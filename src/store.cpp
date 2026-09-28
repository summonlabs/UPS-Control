#include "ups_control/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "detail/codec.hpp"
#include "detail/crash.hpp"
#include "detail/crc32c.hpp"
#include "detail/file_lock.hpp"
#include "detail/platform_io.hpp"
#include "detail/serialization.hpp"

namespace ups_control {
namespace {

constexpr std::array<char, 8> kPayloadMagic = {'U', 'P', 'S', 'P', 'A', 'Y', '0', '1'};
constexpr std::array<char, 8> kTrailerMagic = {'U', 'P', 'S', 'E', 'N', 'D', '0', '1'};
constexpr std::uint32_t kStoreFormatVersion = 1;
constexpr std::uint32_t kByteOrderMarker = 0x01020304u;
constexpr std::size_t kPayloadHeaderBytes = 72;
constexpr std::size_t kTrailerBytes = 12;
/// Bounded directory scan used to retire residue. A directory with more entries
/// than this is left untouched rather than walked without limit.
constexpr std::size_t kMaxResidueScanEntries = 1u << 20;

std::string hex16(std::uint64_t value) {
  static const char* digits = "0123456789abcdef";
  std::string text(16, '0');
  for (int index = 0; index < 16; ++index) {
    const unsigned shift = static_cast<unsigned>(60 - 4 * index);
    text[static_cast<std::size_t>(index)] = digits[(value >> shift) & 0xFull];
  }
  return text;
}

std::string generation_suffix(StoreGeneration generation) {
  return ".g" + hex16(generation.value());
}

std::uint64_t now_nanos() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

Status corruption(const std::filesystem::path& path, const std::string& detail) {
  return Status::error(StatusCode::Corruption, "'" + detail::path_utf8(path) + "': " + detail);
}

bool path_exists(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (error) {
    return false;
  }
  return status.type() != std::filesystem::file_type::not_found;
}

/// The full decoded content of one store generation.
struct StoreImage {
  detail::HeadRecord head;
  UpsState state;
  std::string recorded_path;
  std::uint64_t head_bytes = 0;
  std::uint64_t payload_bytes = 0;
  std::filesystem::path payload_path;
};

Status read_store(const std::filesystem::path& raw_path, const StoreReadOptions& options,
                  StoreImage& image) {
  const Status raw = detail::validate_raw_path(raw_path, options.limits.max_path_bytes);
  if (!raw.ok()) {
    return raw;
  }
  const Status shape = detail::validate_store_path(raw_path);
  if (!shape.ok()) {
    return shape;
  }

  const Result<std::uint64_t> head_size = detail::file_size(raw_path);
  if (!head_size.ok()) {
    return head_size.status();
  }
  if (head_size.value() != detail::kHeadBytes) {
    return corruption(raw_path,
                      "a store head marker must be exactly " + std::to_string(detail::kHeadBytes) +
                          " bytes; this file is " + std::to_string(head_size.value()) +
                          " bytes, so it is not a store this runtime can adopt");
  }
  const Result<std::vector<std::byte>> head_bytes =
      detail::read_file_bounded(raw_path, detail::kHeadBytes);
  if (!head_bytes.ok()) {
    return head_bytes.status();
  }
  const Result<detail::HeadRecord> head = detail::decode_head(head_bytes.value());
  if (!head.ok()) {
    return head.status();
  }
  image.head = head.value();
  image.head_bytes = head_size.value();

  if (options.expected_store_identity.has_value() &&
      !(options.expected_store_identity.value() == image.head.identity)) {
    return Status::error(StatusCode::StaleAuthority,
                         "the store at '" + detail::path_utf8(raw_path) +
                             "' carries identity " + image.head.identity.to_hex() +
                             " but the caller requires " +
                             options.expected_store_identity->to_hex());
  }
  if (options.minimum_generation.has_value() &&
      image.head.generation.value() < options.minimum_generation->value()) {
    return Status::error(StatusCode::StaleGeneration,
                         "the store at '" + detail::path_utf8(raw_path) +
                             "' is committed at generation " +
                             std::to_string(image.head.generation.value()) +
                             " which is below the required floor of " +
                             std::to_string(options.minimum_generation->value()) +
                             "; an older valid copy has been substituted");
  }
  if (image.head.payload_bytes > options.limits.max_payload_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "the head marker declares a payload of " +
                             std::to_string(image.head.payload_bytes) +
                             " bytes, above the configured limit of " +
                             std::to_string(options.limits.max_payload_bytes));
  }

  image.payload_path = detail::derived_path(raw_path, generation_suffix(image.head.generation));
  const Result<std::uint64_t> payload_size = detail::file_size(image.payload_path);
  if (!payload_size.ok()) {
    return Status::error(payload_size.status().code(),
                         "the head marker names generation " +
                             std::to_string(image.head.generation.value()) +
                             " but its payload is not readable: " +
                             payload_size.status().message());
  }
  if (payload_size.value() != image.head.payload_bytes) {
    return corruption(image.payload_path,
                      "the payload is " + std::to_string(payload_size.value()) +
                          " bytes but the head marker declares " +
                          std::to_string(image.head.payload_bytes) +
                          "; a partial generation is never stitched together with a whole one");
  }
  // The generation payload is held to the same path rules as the head marker. A
  // payload that is a symbolic link or a reparse point is judged before its
  // content: it names a file somewhere else, and a store is a pair of files in one
  // directory rather than a directory of whatever a link happens to point at.
  const Status payload_shape = detail::validate_store_path(image.payload_path);
  if (!payload_shape.ok()) {
    return Status::error(payload_shape.code(),
                         "the payload of generation " +
                             std::to_string(image.head.generation.value()) + ": " +
                             payload_shape.message());
  }
  const Result<std::vector<std::byte>> payload =
      detail::read_file_bounded(image.payload_path, options.limits.max_payload_bytes);
  if (!payload.ok()) {
    return payload.status();
  }
  image.payload_bytes = payload_size.value();

  const std::vector<std::byte>& bytes = payload.value();
  if (bytes.size() < kPayloadHeaderBytes + kTrailerBytes) {
    return corruption(image.payload_path,
                      "the payload is shorter than its own header and trailer");
  }
  for (std::size_t index = 0; index < kPayloadMagic.size(); ++index) {
    if (bytes[index] != static_cast<std::byte>(kPayloadMagic[index])) {
      return corruption(image.payload_path,
                        "the payload magic does not match; this is not a UPS Control payload");
    }
  }
  const std::size_t trailer_start = bytes.size() - kTrailerBytes;
  for (std::size_t index = 0; index < kTrailerMagic.size(); ++index) {
    if (bytes[trailer_start + index] != static_cast<std::byte>(kTrailerMagic[index])) {
      return corruption(image.payload_path, "the payload trailer magic does not match");
    }
  }
  detail::ByteReader trailer(
      std::span<const std::byte>(bytes).subspan(trailer_start + kTrailerMagic.size(), 4));
  const Result<std::uint32_t> trailer_crc = trailer.u32();
  if (!trailer_crc.ok()) {
    return trailer_crc.status();
  }
  const std::uint32_t computed_trailer_crc =
      detail::crc32c(std::span<const std::byte>(bytes.data(), trailer_start));
  if (trailer_crc.value() != computed_trailer_crc) {
    return corruption(image.payload_path,
                      "the payload fails its trailer integrity check: stored " +
                          std::to_string(trailer_crc.value()) + ", computed " +
                          std::to_string(computed_trailer_crc));
  }
  const std::uint32_t computed_payload_crc =
      detail::crc32c(std::span<const std::byte>(bytes.data() + kPayloadHeaderBytes,
                                                bytes.size() - kPayloadHeaderBytes));
  if (computed_payload_crc != image.head.payload_crc32c) {
    return corruption(image.payload_path,
                      "the payload fails the integrity check bound into the head marker: head " +
                          std::to_string(image.head.payload_crc32c) + ", computed " +
                          std::to_string(computed_payload_crc));
  }

  detail::ByteReader reader(std::span<const std::byte>(bytes.data(), kPayloadHeaderBytes));
  for (std::size_t index = 0; index < kPayloadMagic.size(); ++index) {
    const Result<std::uint8_t> byte = reader.u8();
    if (!byte.ok()) {
      return byte.status();
    }
  }
  const Result<std::uint32_t> version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != kStoreFormatVersion) {
    return Status::error(StatusCode::IncompatibleVersion,
                         "the payload at '" + detail::path_utf8(image.payload_path) +
                             "' declares format version " + std::to_string(version.value()) +
                             "; this build understands version " +
                             std::to_string(kStoreFormatVersion));
  }
  const Result<std::uint32_t> marker = reader.u32();
  if (!marker.ok()) {
    return marker.status();
  }
  if (marker.value() != kByteOrderMarker) {
    return Status::error(StatusCode::EndianMismatch,
                         "the payload at '" + detail::path_utf8(image.payload_path) +
                             "' was written in the opposite byte order");
  }
  const Result<std::uint64_t> generation = reader.u64();
  if (!generation.ok()) {
    return generation.status();
  }
  if (generation.value() != image.head.generation.value()) {
    return corruption(image.payload_path,
                      "the payload declares generation " + std::to_string(generation.value()) +
                          " but the head marker names " +
                          std::to_string(image.head.generation.value()));
  }
  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.status();
  }
  if (epoch.value() != image.head.epoch.value()) {
    return corruption(image.payload_path, "the payload epoch disagrees with the head marker");
  }
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  if (incarnation.value() != image.head.incarnation.value()) {
    return corruption(image.payload_path, "the payload incarnation disagrees with the head marker");
  }
  const Result<std::uint64_t> identity_high = reader.u64();
  if (!identity_high.ok()) {
    return identity_high.status();
  }
  const Result<std::uint64_t> identity_low = reader.u64();
  if (!identity_low.ok()) {
    return identity_low.status();
  }
  if (identity_high.value() != image.head.identity.high() ||
      identity_low.value() != image.head.identity.low()) {
    return corruption(image.payload_path,
                      "the payload store identity disagrees with the head marker");
  }
  const Result<std::uint32_t> path_bytes = reader.u32();
  if (!path_bytes.ok()) {
    return path_bytes.status();
  }
  const Result<std::uint32_t> state_bytes = reader.u32();
  if (!state_bytes.ok()) {
    return state_bytes.status();
  }
  const Result<std::uint32_t> state_crc = reader.u32();
  if (!state_crc.ok()) {
    return state_crc.status();
  }
  const Result<std::uint32_t> reserved = reader.u32();
  if (!reserved.ok()) {
    return reserved.status();
  }
  if (reserved.value() != 0u) {
    return corruption(image.payload_path, "a reserved payload field is not zero");
  }
  if (path_bytes.value() > options.limits.max_path_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "the payload declares a recorded path of " +
                             std::to_string(path_bytes.value()) + " bytes, above the limit of " +
                             std::to_string(options.limits.max_path_bytes));
  }
  const std::size_t expected_total = kPayloadHeaderBytes + path_bytes.value() + state_bytes.value() +
                                     kTrailerBytes;
  if (expected_total != bytes.size()) {
    return corruption(image.payload_path,
                      "the declared sections total " + std::to_string(expected_total) +
                          " bytes but the payload is " + std::to_string(bytes.size()) +
                          " bytes; truncation and extension are both refused");
  }
  image.recorded_path.assign(
      reinterpret_cast<const char*>(bytes.data() + kPayloadHeaderBytes), path_bytes.value());
  // The recorded path is untrusted input like every other field. It is checked
  // before it is used for anything, including the identity comparison, so a store
  // that records a malformed path is refused whether or not binding is enforced.
  if (image.recorded_path.find('\0') != std::string::npos) {
    return corruption(image.payload_path,
                      "the recorded path contains an embedded NUL, so it cannot name a real file");
  }
  const Status recorded_shape =
      detail::validate_raw_path(std::filesystem::path(image.recorded_path), options.limits.max_path_bytes);
  if (!recorded_shape.ok()) {
    return corruption(image.payload_path,
                      "the recorded path is not a usable path: " + recorded_shape.message());
  }
  const std::span<const std::byte> state_span(
      bytes.data() + kPayloadHeaderBytes + path_bytes.value(), state_bytes.value());
  const std::uint32_t computed_state_crc = detail::crc32c(state_span);
  if (computed_state_crc != state_crc.value()) {
    return corruption(image.payload_path,
                      "the canonical state section fails its integrity check");
  }
  const Result<UpsState> state = detail::decode_state(state_span, options.limits);
  if (!state.ok()) {
    return state.status();
  }
  image.state = state.value();
  if (image.state.generation.value() != image.head.generation.value()) {
    return corruption(image.payload_path, "the decoded state generation disagrees with its envelope");
  }

  const std::uint32_t path_crc =
      detail::crc32c(std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(image.recorded_path.data()), image.recorded_path.size()));
  if (path_crc != image.head.path_crc32c) {
    return corruption(raw_path,
                      "the recorded path does not match the path check bound into the head marker");
  }

  if (options.enforce_path_binding) {
    const Result<std::filesystem::path> canonical = detail::canonical_store_path(raw_path);
    if (!canonical.ok()) {
      return canonical.status();
    }
    if (image.recorded_path != detail::path_utf8(canonical.value())) {
      return Status::error(StatusCode::Conflict,
                           "the store at '" + detail::path_utf8(raw_path) +
                               "' was created at '" + image.recorded_path +
                               "' and path binding is enforced; refusing to adopt it");
    }
  }
  return Status::success();
}

std::vector<std::byte> build_payload(const StoreIdentity& identity, StoreGeneration generation,
                                     ControlEpoch epoch, Incarnation incarnation,
                                     const std::string& recorded_path,
                                     std::span<const std::byte> state_bytes,
                                     std::uint32_t& payload_crc_out) {
  detail::ByteWriter writer;
  for (const char byte : kPayloadMagic) {
    writer.u8(static_cast<std::uint8_t>(byte));
  }
  writer.u32(kStoreFormatVersion);
  writer.u32(kByteOrderMarker);
  writer.u64(generation.value());
  writer.u64(epoch.value());
  writer.u64(incarnation.value());
  writer.u64(identity.high());
  writer.u64(identity.low());
  writer.u32(static_cast<std::uint32_t>(recorded_path.size()));
  writer.u32(static_cast<std::uint32_t>(state_bytes.size()));
  writer.u32(detail::crc32c(state_bytes));
  writer.u32(0);
  writer.raw(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(recorded_path.data()), recorded_path.size()));
  writer.raw(state_bytes);
  std::vector<std::byte> body = writer.take();
  const std::uint32_t trailer_crc = detail::crc32c(body);
  detail::ByteWriter trailer;
  for (const char byte : kTrailerMagic) {
    trailer.u8(static_cast<std::uint8_t>(byte));
  }
  trailer.u32(trailer_crc);
  const std::vector<std::byte> trailer_bytes = trailer.take();
  body.insert(body.end(), trailer_bytes.begin(), trailer_bytes.end());
  payload_crc_out = detail::crc32c(std::span<const std::byte>(body.data() + kPayloadHeaderBytes,
                                                              body.size() - kPayloadHeaderBytes));
  return body;
}

/// Retires residue that is not the committed generation. Best effort: a file that
/// cannot be removed is reported, never silently ignored into a false success.
void retire_residue(const std::filesystem::path& path, StoreGeneration keep,
                    std::vector<std::string>* failures) {
  std::error_code error;
  const std::filesystem::path parent =
      path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
  std::filesystem::directory_iterator iterator(parent, error);
  if (error) {
    if (failures != nullptr) {
      failures->push_back("could not scan '" + detail::path_utf8(parent) + "' for residue: " +
                          error.message());
    }
    return;
  }
  const std::string base = detail::path_utf8(path.filename());
  const std::string keep_name = base + generation_suffix(keep);
  std::size_t examined = 0;
  for (const std::filesystem::directory_entry& entry : iterator) {
    if (++examined > kMaxResidueScanEntries) {
      break;
    }
    const std::string name = detail::path_utf8(entry.path().filename());
    if (name == keep_name) {
      continue;
    }
    const bool is_payload_candidate =
        name.size() > base.size() + 2 && name.compare(0, base.size(), base) == 0 &&
        name.compare(base.size(), 2, ".g") == 0;
    const bool is_staging =
        name.size() > base.size() + 9 && name.compare(0, base.size(), base) == 0 &&
        name.compare(base.size(), 9, ".staging-") == 0;
    const bool is_head_tmp = name == base + ".head-tmp";
    if (!is_payload_candidate && !is_staging && !is_head_tmp) {
      continue;
    }
    std::error_code remove_error;
    const bool removed = std::filesystem::remove(entry.path(), remove_error);
    if (remove_error && failures != nullptr) {
      failures->push_back("could not retire residue '" + detail::path_utf8(entry.path()) +
                          "': " + remove_error.message());
    }
    (void)removed;
  }
}

}  // namespace

struct UpsStore::Impl {
  std::filesystem::path path;
  std::filesystem::path canonical;
  std::string recorded_path;
  StoreAccess access = StoreAccess::ReadWrite;
  bool writable = false;
  bool validate = true;
  ResourceLimits limits;
  detail::ExclusiveFileLock lock;
  detail::HeadRecord head;
  detail::CrashInjection crash;
  std::uint64_t commit_count = 0;
  std::uint64_t staging_serial = 0;
  std::vector<std::string> residue_failures;
};

UpsStore::~UpsStore() {
  if (open_) {
    (void)close();
  }
}

UpsStore::UpsStore(UpsStore&& other) noexcept
    : impl_(std::move(other.impl_)),
      opened_state_(std::move(other.opened_state_)),
      info_(std::move(other.info_)),
      path_(std::move(other.path_)),
      open_(other.open_) {
  other.open_ = false;
}

UpsStore& UpsStore::operator=(UpsStore&& other) noexcept {
  if (this != &other) {
    if (open_) {
      (void)close();
    }
    impl_ = std::move(other.impl_);
    opened_state_ = std::move(other.opened_state_);
    info_ = std::move(other.info_);
    path_ = std::move(other.path_);
    open_ = other.open_;
    other.open_ = false;
  }
  return *this;
}

const char* to_string(DurableCrashPoint point) noexcept {
  switch (point) {
    case DurableCrashPoint::None: return "none";
    case DurableCrashPoint::BeforeStagingWrite: return "before_staging_write";
    case DurableCrashPoint::AfterStagingWrite: return "after_staging_write";
    case DurableCrashPoint::AfterStagingFlush: return "after_staging_flush";
    case DurableCrashPoint::AfterStagingVerify: return "after_staging_verify";
    case DurableCrashPoint::AfterHeadCommit: return "after_head_commit";
  }
  return "unknown";
}

std::string path_to_utf8(const std::filesystem::path& path) { return detail::path_utf8(path); }

Result<std::shared_ptr<const UpsState>> UpsStore::read_file(const std::filesystem::path& path,
                                                            const StoreReadOptions& options) {
  StoreImage image;
  const Status status = read_store(path, options, image);
  if (!status.ok()) {
    return status;
  }
  return std::shared_ptr<const UpsState>(new UpsState(std::move(image.state)));
}

Status UpsStore::verify_file(const std::filesystem::path& path, const StoreReadOptions& options) {
  StoreImage image;
  Status status = read_store(path, options, image);
  if (!status.ok()) {
    return status;
  }
  return Status::success();
}

Result<std::shared_ptr<UpsStore>> UpsStore::open(const StoreOpenOptions& options) {
  const Status raw = detail::validate_raw_path(options.path, options.limits.max_path_bytes);
  if (!raw.ok()) {
    return raw;
  }
  const Status shape = detail::validate_store_path(options.path);
  if (!shape.ok()) {
    return shape;
  }
  const Result<std::filesystem::path> canonical = detail::canonical_store_path(options.path);
  if (!canonical.ok()) {
    return canonical.status();
  }

  auto store = std::shared_ptr<UpsStore>(new UpsStore());
  store->impl_ = std::make_shared<Impl>();
  Impl& impl = *store->impl_;
  impl.path = options.path;
  impl.canonical = canonical.value();
  impl.recorded_path = detail::path_utf8(canonical.value());
  impl.access = options.access;
  impl.writable = options.access == StoreAccess::ReadWrite;
  impl.validate = options.validate_state;
  impl.limits = options.limits;
  impl.crash.point = options.crash_at;
  impl.crash.after_commits = options.crash_after_commits;
  // The path check is bound into the head marker, so it must be known before the
  // first commit that writes a head. It is recomputed here from the resolved path.
  impl.head.path_crc32c = detail::crc32c(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(impl.recorded_path.data()), impl.recorded_path.size()));

  const bool exists = path_exists(options.path);
  if (!exists && options.access == StoreAccess::ReadOnly) {
    return Status::error(StatusCode::NotFound,
                         "no store exists at '" + detail::path_utf8(options.path) + "'");
  }
  if (!exists && !options.create_if_missing) {
    return Status::error(StatusCode::NotFound,
                         "no store exists at '" + detail::path_utf8(options.path) +
                             "' and creation was not requested");
  }

  if (impl.writable) {
    const std::filesystem::path lock_path = detail::derived_path(options.path, ".lock");
    Result<detail::ExclusiveFileLock> lock = detail::ExclusiveFileLock::acquire(lock_path);
    if (!lock.ok()) {
      return lock.status();
    }
    impl.lock = std::move(lock.value());
  }

  store->path_ = options.path;
  store->open_ = true;

  if (exists) {
    StoreReadOptions read_options;
    read_options.limits = options.limits;
    read_options.expected_store_identity = options.expected_store_identity;
    read_options.minimum_generation = options.minimum_generation;
    read_options.enforce_path_binding = options.enforce_path_binding;
    StoreImage image;
    const Status status = read_store(options.path, read_options, image);
    if (!status.ok()) {
      return status;
    }
    impl.head = image.head;
    impl.head.payload_bytes = image.payload_bytes;
    store->opened_state_ = std::shared_ptr<const UpsState>(new UpsState(image.state));
  } else {
    if (options.epoch.is_zero() || options.incarnation.is_zero()) {
      return Status::error(StatusCode::InvalidArgument,
                           "creating a store requires a positive control epoch and incarnation");
    }
    if (!is_valid_instant(options.created_at)) {
      return Status::error(StatusCode::InvalidArgument,
                           "creating a store requires a positive creation instant");
    }
    UpsState state;
    state.generation = StoreGeneration{1};
    state.epoch = options.epoch;
    state.incarnation = options.incarnation;
    state.created_at = options.created_at;
    state.updated_at = options.created_at;
    CommitLogEntry entry;
    entry.generation = state.generation;
    entry.operation = OperationKind::StoreCreated;
    entry.at = options.created_at;
    state.commit_log.push_back(entry);

    impl.head.identity = StoreIdentity::generate();
    impl.head.generation = StoreGeneration{0};
    impl.head.epoch = options.epoch;
    impl.head.incarnation = options.incarnation;
    impl.head.created_at = options.created_at;
    impl.head.committed_at = Tick{0};

    const Result<CommitResult> committed = store->commit(state);
    if (!committed.ok()) {
      return committed.status();
    }
    // The committed generation is also the state this handle opened at, so the
    // store exposes exactly what is on disk rather than a second read of it.
    store->opened_state_ = std::shared_ptr<const UpsState>(new UpsState(state));
    store->info_.created = true;
  }

  if (store->opened_state_ == nullptr) {
    return Status::error(StatusCode::InvariantViolation,
                         "the store produced no state while opening");
  }
  const UpsState& state = *store->opened_state_;
  StoreInfo& info = store->info_;
  info.path = options.path;
  info.store_identity = impl.head.identity;
  info.generation = impl.head.generation;
  info.epoch = impl.head.epoch;
  info.incarnation = impl.head.incarnation;
  info.head_sequence = impl.head.head_sequence;
  info.created_at = state.created_at;
  info.updated_at = state.updated_at;
  info.revalidated_at = state.revalidated_at;
  info.committed_at = impl.head.committed_at;
  info.recorded_path = impl.recorded_path;
  info.path_binding_matches = impl.recorded_path == impl.recorded_path;
  info.writable = impl.writable;
  info.format_version = kStoreFormatVersion;
  info.head_bytes = detail::kHeadBytes;
  info.payload_bytes = impl.head.payload_bytes;
  info.payload_crc32c = impl.head.payload_crc32c;
  info.unit_count = state.units.size();
  info.attempt_count = state.attempts.size();
  info.idempotency_count = state.idempotency.size();
  info.commit_count = impl.commit_count;

  if (options.enforce_path_binding) {
    info.path_binding_matches = impl.recorded_path == detail::path_utf8(impl.canonical);
    if (!info.path_binding_matches) {
      (void)store->close();
      return Status::error(StatusCode::Conflict,
                           "the store at '" + detail::path_utf8(options.path) +
                               "' was created at '" + impl.recorded_path + "'");
    }
  }

  if (impl.writable) {
    retire_residue(options.path, impl.head.generation, &impl.residue_failures);
  }
  return store;
}

Result<CommitResult> UpsStore::commit(const UpsState& state) {
  if (!open_ || impl_ == nullptr) {
    return Status::error(StatusCode::Closed, "the store is not open");
  }
  Impl& impl = *impl_;
  if (!impl.writable) {
    return Status::error(StatusCode::ReadOnly,
                         "the store at '" + detail::path_utf8(impl.path) +
                             "' was opened read-only");
  }
  if (!(state.generation.value() == impl.head.generation.value() + 1)) {
    return Status::error(StatusCode::StaleGeneration,
                         "the state declares generation " + std::to_string(state.generation.value()) +
                             " but the committed generation is " +
                             std::to_string(impl.head.generation.value()) +
                             "; a commit must advance the generation by exactly one");
  }
  if (state.epoch.is_zero() || state.incarnation.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "a committed state must carry a positive control epoch and incarnation");
  }
  if (state.epoch.value() < impl.head.epoch.value()) {
    return Status::error(StatusCode::StaleAuthority,
                         "the state moves the control epoch backwards, from " +
                             std::to_string(impl.head.epoch.value()) + " to " +
                             std::to_string(state.epoch.value()));
  }
  if (state.epoch.value() == impl.head.epoch.value() &&
      state.incarnation.value() < impl.head.incarnation.value()) {
    return Status::error(StatusCode::StaleAuthority,
                         "the state moves the controller incarnation backwards within epoch " +
                             std::to_string(state.epoch.value()));
  }
  if (impl.validate) {
    const Status valid = validate_state(state, impl.limits);
    if (!valid.ok()) {
      return valid;
    }
  }

  ++impl.commit_count;
  const std::uint64_t ordinal = impl.commit_count;
  const std::uint64_t start = now_nanos();
  CommitMetrics metrics;

  const Result<std::vector<std::byte>> encoded = detail::encode_state(state, impl.limits);
  if (!encoded.ok()) {
    return encoded.status();
  }
  std::uint32_t payload_crc = 0;
  const std::vector<std::byte> payload =
      build_payload(impl.head.identity, state.generation, state.epoch, state.incarnation,
                    impl.recorded_path, encoded.value(), payload_crc);
  if (payload.size() > impl.limits.max_store_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "the encoded payload is " + std::to_string(payload.size()) +
                             " bytes, above the configured store limit of " +
                             std::to_string(impl.limits.max_store_bytes));
  }

  detail::HeadRecord next = impl.head;
  next.generation = state.generation;
  next.epoch = state.epoch;
  next.incarnation = state.incarnation;
  next.payload_bytes = payload.size();
  next.payload_crc32c = payload_crc;
  next.head_sequence = impl.head.head_sequence + 1;
  next.committed_at = state.updated_at;
  const Result<std::vector<std::byte>> head_bytes = detail::encode_head(next);
  if (!head_bytes.ok()) {
    return head_bytes.status();
  }
  const std::uint64_t encoded_at = now_nanos();
  metrics.encode_nanos = encoded_at - start;

  const std::filesystem::path payload_path =
      detail::derived_path(impl.path, generation_suffix(state.generation));
  const std::filesystem::path staging_path =
      detail::derived_path(impl.path, ".staging-" + std::to_string(detail::current_process_id()) +
                                          "-" + std::to_string(++impl.staging_serial));
  const std::filesystem::path head_tmp = detail::derived_path(impl.path, ".head-tmp");

  // Every path this protocol writes through is checked before it is written. A
  // staging name is derived from the process identifier and a serial, so it is
  // guessable: without this check a pre-created symbolic link at that name would
  // redirect the durable write somewhere else entirely.
  for (const std::filesystem::path* target : {&staging_path, &head_tmp, &payload_path}) {
    const Status shape = detail::validate_store_path(*target);
    if (!shape.ok()) {
      return Status::error(shape.code(),
                           "refusing to publish through '" + detail::path_utf8(*target) +
                               "': " + shape.message());
    }
  }

  detail::maybe_crash(impl.crash, DurableCrashPoint::BeforeStagingWrite, ordinal);
  Status status = detail::write_file(staging_path, payload, impl.limits.max_store_bytes, false);
  if (!status.ok()) {
    return status;
  }
  detail::maybe_crash(impl.crash, DurableCrashPoint::AfterStagingWrite, ordinal);
  status = detail::flush_file(staging_path);
  if (!status.ok()) {
    (void)detail::remove_file(staging_path);
    return status;
  }
  detail::maybe_crash(impl.crash, DurableCrashPoint::AfterStagingFlush, ordinal);
  const std::uint64_t written_at = now_nanos();
  metrics.staging_write_nanos = written_at - encoded_at;

  const Result<std::vector<std::byte>> readback =
      detail::read_file_bounded(staging_path, impl.limits.max_store_bytes);
  if (!readback.ok()) {
    (void)detail::remove_file(staging_path);
    return readback.status();
  }
  if (readback.value() != payload) {
    (void)detail::remove_file(staging_path);
    return Status::error(StatusCode::Corruption,
                         "the staging payload at '" + detail::path_utf8(staging_path) +
                             "' did not read back as it was written");
  }

  status = detail::write_file_durable(head_tmp, head_bytes.value(), detail::kHeadBytes);
  if (!status.ok()) {
    (void)detail::remove_file(staging_path);
    return status;
  }
  const Result<std::vector<std::byte>> head_readback =
      detail::read_file_bounded(head_tmp, detail::kHeadBytes);
  if (!head_readback.ok()) {
    (void)detail::remove_file(staging_path);
    (void)detail::remove_file(head_tmp);
    return head_readback.status();
  }
  const Result<detail::HeadRecord> head_check = detail::decode_head(head_readback.value());
  if (!head_check.ok()) {
    (void)detail::remove_file(staging_path);
    (void)detail::remove_file(head_tmp);
    return head_check.status();
  }
  if (!(head_check.value().generation == next.generation) ||
      head_check.value().payload_bytes != next.payload_bytes ||
      head_check.value().payload_crc32c != next.payload_crc32c) {
    (void)detail::remove_file(staging_path);
    (void)detail::remove_file(head_tmp);
    return Status::error(StatusCode::Corruption,
                         "the staged head marker did not read back as it was written");
  }
  detail::maybe_crash(impl.crash, DurableCrashPoint::AfterStagingVerify, ordinal);
  const std::uint64_t verified_at = now_nanos();
  metrics.verify_nanos = verified_at - written_at;

  status = detail::atomic_replace(staging_path, payload_path);
  if (!status.ok()) {
    (void)detail::remove_file(staging_path);
    (void)detail::remove_file(head_tmp);
    return status;
  }

  const std::filesystem::path previous_payload =
      detail::derived_path(impl.path, generation_suffix(impl.head.generation));
  status = detail::atomic_replace(head_tmp, impl.path);
  if (!status.ok()) {
    (void)detail::remove_file(payload_path);
    (void)detail::remove_file(head_tmp);
    return status;
  }
  // ---- THE COMMIT POINT ----
  // The authoritative head now names the new generation. Everything before this
  // line is invisible; everything after it is housekeeping.
  impl.head = next;
  const std::uint64_t published_at = now_nanos();
  metrics.publish_nanos = published_at - verified_at;

  detail::maybe_crash(impl.crash, DurableCrashPoint::AfterHeadCommit, ordinal);

  std::vector<std::string> cleanup_failures;
  if (!(previous_payload == payload_path)) {
    std::error_code remove_error;
    std::filesystem::remove(previous_payload, remove_error);
    if (remove_error) {
      cleanup_failures.push_back("could not retire '" + detail::path_utf8(previous_payload) +
                                 "': " + remove_error.message());
    }
  }
  retire_residue(impl.path, state.generation, &cleanup_failures);
  const std::uint64_t cleaned_at = now_nanos();
  metrics.cleanup_nanos = cleaned_at - published_at;
  metrics.total_nanos = cleaned_at - start;
  metrics.bytes_written = payload.size() + head_bytes.value().size();

  info_.generation = impl.head.generation;
  info_.epoch = impl.head.epoch;
  info_.incarnation = impl.head.incarnation;
  info_.head_sequence = impl.head.head_sequence;
  info_.updated_at = state.updated_at;
  info_.revalidated_at = state.revalidated_at;
  info_.committed_at = impl.head.committed_at;
  info_.payload_bytes = impl.head.payload_bytes;
  info_.payload_crc32c = impl.head.payload_crc32c;
  info_.unit_count = state.units.size();
  info_.attempt_count = state.attempts.size();
  info_.idempotency_count = state.idempotency.size();
  info_.commit_count = impl.commit_count;

  CommitResult result;
  result.generation = state.generation;
  result.attempt = state.last_attempt;
  result.metrics = metrics;
  return result;
}

Status UpsStore::close() {
  if (!open_) {
    return Status::success();
  }
  open_ = false;
  if (impl_ != nullptr && impl_->writable) {
    const Status released = impl_->lock.release();
    if (!released.ok()) {
      return released;
    }
  }
  return Status::success();
}

}  // namespace ups_control
