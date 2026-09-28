// Adversarial proof obligations: the parser, the store, and the boundary.
//
// Artifacts here are built with the same codec and the same checksum the library
// itself uses, so every refusal is a decision about content and structure rather
// than about a broken test fixture. Each crafted family is proven acceptable in
// its valid form before it is deformed.

#include "test_harness.hpp"

#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "detail/codec.hpp"
#include "detail/crc32c.hpp"
#include "detail/serialization.hpp"
#include "ups_control/engine.hpp"
#include "ups_control/state.hpp"
#include "ups_control/store.hpp"
#include "ups_control/units.hpp"

using namespace ups_control;

namespace {

constexpr std::array<char, 8> kPayloadMagic = {'U', 'P', 'S', 'P', 'A', 'Y', '0', '1'};
constexpr std::array<char, 8> kTrailerMagic = {'U', 'P', 'S', 'E', 'N', 'D', '0', '1'};
constexpr std::size_t kPayloadHeaderBytes = 72;
constexpr std::size_t kTrailerBytes = 12;
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kByteOrderMarker = 0x01020304u;

std::vector<std::byte> read_all(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  const std::vector<char> data((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes;
  bytes.reserve(data.size());
  for (const char value : data) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }
  return bytes;
}

void write_all(const std::filesystem::path& path, std::span<const std::byte> data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  stream.flush();
}

void write_text(const std::filesystem::path& path, std::string_view text) {
  write_all(path, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                             text.size()));
}

std::string hex16(std::uint64_t value) {
  static const char* digits = "0123456789abcdef";
  std::string text(16, '0');
  for (int index = 0; index < 16; ++index) {
    const unsigned shift = static_cast<unsigned>(60 - 4 * index);
    text[static_cast<std::size_t>(index)] =
        digits[static_cast<std::size_t>((value >> shift) & 0xFull)];
  }
  return text;
}

std::filesystem::path payload_path_for(const std::filesystem::path& store,
                                       StoreGeneration generation) {
  std::filesystem::path result = store;
  std::string name = path_to_utf8(store.filename());
  name += ".g" + hex16(generation.value());
  result.replace_filename(std::filesystem::path(std::u8string(name.begin(), name.end())));
  return result;
}

std::vector<std::string> directory_names(const std::filesystem::path& directory) {
  std::vector<std::string> names;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory)) {
    names.push_back(path_to_utf8(entry.path().filename()));
  }
  std::sort(names.begin(), names.end());
  return names;
}

// ---------------------------------------------------------------------------
// A crafted canonical state section, written with the library's own primitives
// ---------------------------------------------------------------------------

struct CraftedUnit {
  std::string id = "unit-a";
  std::string label = "unit a";
  std::uint32_t hardware = 1;
  std::uint8_t lifecycle = 3;  // InService
  std::uint8_t operating = 3;  // OnlineNormal
  std::uint8_t basis = 2;      // Observed
  std::uint64_t revision = 1;
  std::int64_t state_since = 1000;
  std::int64_t registered_at = 1000;
  std::string note;
  std::int64_t max_evidence_age = 600;
  std::uint8_t has_in_flight = 0;
  std::uint64_t in_flight = 0;
  std::int64_t last_verified_at = 0;
};

struct CraftedAttempt {
  std::uint64_t id = 1;
  std::string key = "key-1";
  std::string ups = "unit-a";
  std::uint64_t planned_revision = 1;
  std::uint8_t phase = 1;  // Planned
};

struct CraftedIdempotency {
  std::string key = "key-1";
  std::uint64_t attempt = 1;
};

struct CraftedCommit {
  std::uint64_t generation = 1;
  std::uint8_t operation = 1;  // StoreCreated
  std::int64_t at = 1000;
};

struct CraftedState {
  std::uint64_t generation = 1;
  std::uint64_t epoch = 1;
  std::uint64_t incarnation = 1;
  std::int64_t created_at = 1000;
  std::int64_t updated_at = 1000;
  std::int64_t revalidated_at = 0;
  std::uint64_t last_attempt = 0;
  std::uint64_t operation_count = 0;
  std::vector<CraftedUnit> units;
  std::vector<CraftedAttempt> attempts;
  std::vector<CraftedIdempotency> idempotency;
  std::vector<CraftedCommit> commit_log;
  std::optional<std::uint32_t> declare_unit_count;
  std::optional<std::uint32_t> declare_attempt_count;
  std::optional<std::uint32_t> declare_idempotency_count;
  std::optional<std::uint32_t> declare_commit_count;
  std::optional<std::uint32_t> declare_obligation_count;
  std::optional<std::uint32_t> declare_grant_count;
};

CraftedState crafted_state_with_one_unit() {
  CraftedState state;
  CraftedUnit unit;
  unit.id = "unit-a";
  unit.label = "crafted unit";
  state.units.push_back(unit);
  CraftedCommit commit;
  commit.generation = 1;
  commit.operation = 1;
  commit.at = 1000;
  state.commit_log.push_back(commit);
  return state;
}

StoreIdentity crafted_identity() {
  return StoreIdentity::from_components(0x0102030405060708ull, 0x1112131415161718ull).value();
}

void write_crafted_unit(detail::ByteWriter& writer, const CraftedUnit& unit,
                        const CraftedState& state) {
  (void)writer.text(unit.id, 128);
  (void)writer.text(unit.label, 256);
  writer.u32(unit.hardware);
  writer.u8(unit.lifecycle);
  writer.u8(unit.operating);
  writer.u8(unit.basis);
  writer.u64(unit.revision);
  writer.i64(unit.state_since);
  writer.i64(unit.registered_at);
  (void)writer.text(unit.note, 512);
  writer.u8(0);  // no observation
  writer.u8(0);  // no reserve floor
  writer.u8(0);
  writer.i64(0);
  writer.i64(unit.max_evidence_age);
  writer.i64(0);
  writer.u8(2);  // Estimated
  writer.u32(state.declare_obligation_count.value_or(0));
  writer.u32(state.declare_grant_count.value_or(0));
  writer.u8(unit.has_in_flight);
  writer.u64(unit.in_flight);
  writer.i64(unit.last_verified_at);
}

void write_crafted_attempt(detail::ByteWriter& writer, const CraftedAttempt& attempt) {
  writer.u64(attempt.id);
  (void)writer.text(attempt.key, 64);
  (void)writer.text(attempt.ups, 128);
  writer.u32(1);  // hardware
  writer.u64(1);  // epoch
  writer.u64(1);  // incarnation
  writer.u64(attempt.planned_revision);
  writer.u8(1);  // EnterStaticBypass
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.i64(0);
  writer.i64(0);
  writer.u8(5);  // target: StaticBypass
  writer.i64(1200);
  writer.u8(attempt.phase);
  writer.u64(0x1122334455667788ull);
  writer.u8(0);
  writer.u8(0);
  (void)writer.text(std::string(), 512);
  writer.u8(0);
  (void)writer.text(std::string(), 512);
  writer.i64(0);
  writer.u8(0);
  writer.i64(0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  (void)writer.text(std::string(), 512);
  writer.i64(0);
  writer.u8(0);
  (void)writer.text(std::string(), 128);
  writer.u32(0);
  writer.u8(0);
  (void)writer.text(std::string(), 128);
  (void)writer.text(std::string(), 512);
}

std::vector<std::byte> encode_crafted_state(const CraftedState& state) {
  detail::ByteWriter writer;
  writer.u32(kFormatVersion);
  writer.u32(kByteOrderMarker);
  writer.u64(state.generation);
  writer.u64(state.epoch);
  writer.u64(state.incarnation);
  writer.i64(state.created_at);
  writer.i64(state.updated_at);
  writer.i64(state.revalidated_at);
  writer.u64(state.last_attempt);
  writer.u64(state.operation_count);
  writer.u32(state.declare_unit_count.value_or(static_cast<std::uint32_t>(state.units.size())));
  for (const CraftedUnit& unit : state.units) {
    write_crafted_unit(writer, unit, state);
  }
  writer.u32(state.declare_attempt_count.value_or(static_cast<std::uint32_t>(state.attempts.size())));
  for (const CraftedAttempt& attempt : state.attempts) {
    write_crafted_attempt(writer, attempt);
  }
  writer.u32(state.declare_idempotency_count.value_or(
      static_cast<std::uint32_t>(state.idempotency.size())));
  for (const CraftedIdempotency& entry : state.idempotency) {
    (void)writer.text(entry.key, 64);
    writer.u64(entry.attempt);
    writer.u64(0);
    writer.i64(1200);
  }
  writer.u32(state.declare_commit_count.value_or(static_cast<std::uint32_t>(state.commit_log.size())));
  for (const CraftedCommit& entry : state.commit_log) {
    writer.u64(entry.generation);
    writer.u64(0);
    (void)writer.text(std::string(), 64);
    writer.u8(entry.operation);
    (void)writer.text(std::string(), 128);
    writer.u64(0);
    writer.i64(entry.at);
  }
  return writer.take();
}

/// Envelope overrides, so that a single crafted state can be framed with exactly
/// one malformed declaration at a time.
struct CraftedEnvelope {
  std::string recorded_path;
  std::optional<std::uint32_t> path_bytes;
  std::optional<std::uint32_t> state_bytes;
  std::optional<std::uint32_t> state_crc;
  std::optional<std::uint32_t> reserved;
  std::optional<std::uint64_t> head_payload_bytes;
  std::optional<std::uint64_t> head_generation;
  std::optional<std::uint32_t> truncate_state_to;
};

void write_crafted_store(const std::filesystem::path& path, const CraftedState& state,
                         const CraftedEnvelope& envelope) {
  const std::string recorded_path =
      envelope.recorded_path.empty() ? path_to_utf8(path) : envelope.recorded_path;
  std::vector<std::byte> state_bytes = encode_crafted_state(state);
  if (envelope.truncate_state_to.has_value()) {
    state_bytes.resize(std::min(state_bytes.size(),
                                static_cast<std::size_t>(envelope.truncate_state_to.value())));
  }

  detail::ByteWriter writer;
  for (const char byte : kPayloadMagic) {
    writer.u8(static_cast<std::uint8_t>(byte));
  }
  writer.u32(kFormatVersion);
  writer.u32(kByteOrderMarker);
  writer.u64(state.generation);
  writer.u64(state.epoch);
  writer.u64(state.incarnation);
  writer.u64(crafted_identity().high());
  writer.u64(crafted_identity().low());
  writer.u32(envelope.path_bytes.value_or(static_cast<std::uint32_t>(recorded_path.size())));
  writer.u32(envelope.state_bytes.value_or(static_cast<std::uint32_t>(state_bytes.size())));
  writer.u32(envelope.state_crc.value_or(detail::crc32c(state_bytes)));
  writer.u32(envelope.reserved.value_or(0));
  writer.raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(recorded_path.data()),
                                        recorded_path.size()));
  writer.raw(state_bytes);
  std::vector<std::byte> body = writer.take();
  const std::uint32_t trailer_crc =
      detail::crc32c(std::span<const std::byte>(body.data(), body.size()));
  detail::ByteWriter trailer;
  for (const char byte : kTrailerMagic) {
    trailer.u8(static_cast<std::uint8_t>(byte));
  }
  trailer.u32(trailer_crc);
  const std::vector<std::byte> trailer_bytes = trailer.take();
  body.insert(body.end(), trailer_bytes.begin(), trailer_bytes.end());

  detail::HeadRecord head;
  head.identity = crafted_identity();
  head.generation = StoreGeneration{envelope.head_generation.value_or(state.generation)};
  head.epoch = ControlEpoch{state.epoch};
  head.incarnation = Incarnation{state.incarnation};
  head.payload_bytes = envelope.head_payload_bytes.value_or(body.size());
  head.payload_crc32c = detail::crc32c(std::span<const std::byte>(
      body.data() + kPayloadHeaderBytes, body.size() - kPayloadHeaderBytes));
  head.path_crc32c = detail::crc32c(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(recorded_path.data()), recorded_path.size()));
  head.head_sequence = 1;
  head.committed_at = Tick{state.updated_at};
  head.created_at = Tick{state.created_at};

  write_all(path, detail::encode_head(head).value());
  write_all(payload_path_for(path, head.generation), body);
}

Result<std::shared_ptr<const UpsState>> read_crafted(const std::filesystem::path& path,
                                                     const CraftedState& state,
                                                     const CraftedEnvelope& envelope,
                                                     const StoreReadOptions& options) {
  write_crafted_store(path, state, envelope);
  return UpsStore::read_file(path, options);
}

Result<std::shared_ptr<const UpsState>> read_crafted(const std::filesystem::path& path,
                                                     const CraftedState& state) {
  return read_crafted(path, state, CraftedEnvelope{}, StoreReadOptions{});
}

/// Creates a real, valid, empty store and returns its path.
Result<std::filesystem::path> make_real_store(const std::filesystem::path& path) {
  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  const Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options);
  if (!store.ok()) {
    return store.status();
  }
  const Status closed = store.value()->close();
  if (!closed.ok()) {
    return closed;
  }
  return path;
}

}  // namespace

// ---------------------------------------------------------------------------
// Declared length boundaries, both sides
// ---------------------------------------------------------------------------

UC_TEST(adversarial, declared_path_length_at_the_limit_is_accepted) {
  uc_test::ScratchDirectory scratch("adversarial-path-limit-ok");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  // The bound has to admit the path being read, so it is derived from it; the
  // recorded path, which is what the payload declares, is then exactly at it.
  const std::uint32_t limit = static_cast<std::uint32_t>(path_to_utf8(path).size()) + 16;
  CraftedState state = crafted_state_with_one_unit();
  CraftedEnvelope envelope;
  envelope.recorded_path = std::string(limit, 'p');

  StoreReadOptions options;
  options.limits.max_path_bytes = limit;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, envelope, options);
  UC_REQUIRE_OK(read);
  UC_CHECK_EQ(read.value()->units.size(), std::size_t{1});
  UC_REQUIRE_STATUS(UpsStore::verify_file(path, options), StatusCode::Ok);
}

UC_TEST(adversarial, declared_path_length_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-path-limit-over");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  const std::uint32_t limit = static_cast<std::uint32_t>(path_to_utf8(path).size()) + 16;
  CraftedState state = crafted_state_with_one_unit();
  CraftedEnvelope envelope;
  envelope.recorded_path = std::string(static_cast<std::size_t>(limit) + 1, 'p');

  StoreReadOptions options;
  options.limits.max_path_bytes = limit;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, envelope, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find(std::to_string(limit)) != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
  UC_CHECK_MSG(read.status().message().find(std::to_string(static_cast<std::size_t>(limit) + 1)) !=
                   std::string::npos,
               "the refusal must name the declared length: " + read.status().message());
}

UC_TEST(adversarial, declared_payload_over_the_configured_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-state-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();

  // First prove the artifact is accepted at the default bound, so the refusal
  // below is about the configured limit and nothing else.
  const Result<std::shared_ptr<const UpsState>> accepted = read_crafted(path, state);
  UC_REQUIRE_OK(accepted);
  const std::uint64_t payload_size =
      std::filesystem::file_size(payload_path_for(path, StoreGeneration{state.generation}));

  StoreReadOptions options;
  options.limits.max_payload_bytes = payload_size - 1;
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("above the configured limit") != std::string::npos,
               "the refusal must name the configured bound: " + read.status().message());
}

UC_TEST(adversarial, declared_unit_count_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-unit-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  CraftedUnit second;
  second.id = "unit-b";
  state.units.push_back(second);
  const Result<std::shared_ptr<const UpsState>> accepted = read_crafted(path, state);
  UC_REQUIRE_OK(accepted);
  UC_CHECK_EQ(accepted.value()->units.size(), std::size_t{2});

  StoreReadOptions options;
  options.limits.max_ups_units = 1;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
}

UC_TEST(adversarial, declared_obligation_count_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-obligation-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_obligation_count = 2;  // no obligation bodies follow: only the count is read

  StoreReadOptions options;
  options.limits.max_obligations_per_ups = 1;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
}

UC_TEST(adversarial, declared_grant_count_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-grant-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_grant_count = 2;

  StoreReadOptions options;
  options.limits.max_grants_per_ups = 1;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
}

UC_TEST(adversarial, declared_attempt_count_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-attempt-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_attempt_count = 2;

  StoreReadOptions options;
  options.limits.max_attempt_journal = 1;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
}

UC_TEST(adversarial, declared_idempotency_count_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-idempotency-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_idempotency_count = 2;

  StoreReadOptions options;
  options.limits.max_idempotency_records = 1;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
}

UC_TEST(adversarial, declared_commit_log_count_over_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-commit-limit");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_commit_count = 2;

  StoreReadOptions options;
  options.limits.max_commit_log = 1;
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state, CraftedEnvelope{}, options);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + read.status().message());
}

UC_TEST(adversarial, declared_count_of_0xffffffff_is_refused_without_allocating) {
  uc_test::ScratchDirectory scratch("adversarial-huge-count");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_unit_count = 0xFFFFFFFFu;

  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state);
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("4294967295") != std::string::npos,
               "the refusal must name the declared count: " + read.status().message());

  // The same declaration inside a state section that is far too short to hold it
  // must be refused for the count, not decoded into a huge vector.
  CraftedEnvelope truncated;
  truncated.truncate_state_to = 84;
  const Result<std::shared_ptr<const UpsState>> short_read =
      read_crafted(path, state, truncated, StoreReadOptions{});
  UC_REQUIRE_STATUS(short_read, StatusCode::LimitExceeded);
}

UC_TEST(adversarial, forty_byte_state_section_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-short-state");
  const std::filesystem::path path = scratch.file("boundary.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  state.declare_unit_count = 0xFFFFFFFFu;
  CraftedEnvelope truncated;
  truncated.truncate_state_to = 40;  // shorter than the fixed state header

  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(path, state, truncated, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
}

// ---------------------------------------------------------------------------
// Absurd and oversized artifacts
// ---------------------------------------------------------------------------

UC_TEST(adversarial, head_declaring_an_absurd_payload_is_refused_before_reading_it) {
  uc_test::ScratchDirectory scratch("adversarial-absurd-payload");
  const std::filesystem::path path = scratch.file("absurd.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  CraftedEnvelope envelope;
  envelope.head_payload_bytes = (std::numeric_limits<std::uint64_t>::max)();

  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(path, state, envelope, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("head marker declares") != std::string::npos,
               "the refusal must come from the head declaration: " + read.status().message());

  CraftedEnvelope four_gib;
  four_gib.head_payload_bytes = 4ull * 1024ull * 1024ull * 1024ull;
  const Result<std::shared_ptr<const UpsState>> over =
      read_crafted(path, state, four_gib, StoreReadOptions{});
  UC_REQUIRE_STATUS(over, StatusCode::LimitExceeded);
}

UC_TEST(adversarial, four_gib_sparse_file_is_refused_by_metadata) {
  uc_test::ScratchDirectory scratch("adversarial-sparse");
  const std::filesystem::path sparse = scratch.file("sparse.bin");

  std::error_code error;
  write_text(sparse, "x");
  std::filesystem::resize_file(sparse, 4ull * 1024ull * 1024ull * 1024ull, error);
  const bool sparse_created = !error;
  if (!sparse_created) {
    std::cout << "           note: a 4 GiB sparse file could not be created ("
              << error.message() << "); the metadata declaration check is asserted instead\n";
  } else {
    UC_CHECK_EQ(std::filesystem::file_size(sparse), 4ull * 1024ull * 1024ull * 1024ull);
    // A 4 GiB file is not a 128-byte head marker, and the size is refused from
    // the metadata without reading the file.
    UC_REQUIRE_STATUS(UpsStore::verify_file(sparse, StoreReadOptions{}), StatusCode::Corruption);
  }

  // Whether or not the sparse file exists, a store whose head declares a 4 GiB
  // payload is refused against the default payload bound without a read.
  const std::filesystem::path path = scratch.file("declared.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  CraftedEnvelope envelope;
  envelope.head_payload_bytes = 4ull * 1024ull * 1024ull * 1024ull;
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(path, state, envelope, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::LimitExceeded);
  UC_CHECK_MSG(read.status().message().find("67108864") != std::string::npos,
               "the refusal must name the configured bound: " + read.status().message());
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

UC_TEST(adversarial, tiny_files_are_refused) {
  uc_test::ScratchDirectory scratch("adversarial-tiny");
  const std::filesystem::path path = scratch.file("tiny.upsstore");

  write_text(path, std::string());
  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::Corruption);

  write_text(path, std::string("x"));
  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::Corruption);

  write_text(path, std::string(127, 'x'));
  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::Corruption);

  write_text(path, std::string(129, 'x'));
  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::Corruption);

  write_text(path, std::string(128, '\0'));
  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::Corruption);
  UC_REQUIRE_STATUS(UpsStore::verify_file(path, StoreReadOptions{}), StatusCode::Corruption);
}

UC_TEST(adversarial, recorded_path_with_an_embedded_nul_is_refused_unconditionally) {
  uc_test::ScratchDirectory scratch("adversarial-recorded-nul");
  const std::filesystem::path path = scratch.file("embedded-craft.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  std::string recorded = path_to_utf8(path);
  recorded.insert(recorded.size() / 2, 1, '\0');
  CraftedEnvelope envelope;
  envelope.recorded_path = recorded;

  // The artifact is well formed by its own checksums. The recorded path is
  // untrusted input in its own right, so it is checked before it is used for
  // anything at all: an embedded NUL makes it incapable of naming a file, and the
  // refusal does not depend on whether the caller asked for path binding.
  const Result<std::shared_ptr<const UpsState>> unbound =
      read_crafted(path, state, envelope, StoreReadOptions{});
  UC_REQUIRE_STATUS(unbound, StatusCode::Corruption);

  StoreReadOptions bound;
  bound.enforce_path_binding = true;
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(path, state, envelope, bound);
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
}

UC_TEST(adversarial, head_that_is_a_directory_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-head-directory");
  const std::filesystem::path path = scratch.file("directory.upsstore");
  std::filesystem::create_directory(path);
  UC_REQUIRE(std::filesystem::is_directory(path));

  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::InvalidArgument);
  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::InvalidArgument);
}

UC_TEST(adversarial, payload_that_is_a_directory_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-payload-directory");
  const std::filesystem::path path = scratch.file("store.upsstore");
  const Result<std::filesystem::path> created = make_real_store(path);
  UC_REQUIRE_OK(created);
  const StoreGeneration generation =
      detail::decode_head(read_all(path)).value().generation;

  std::filesystem::remove(payload_path_for(path, generation));
  std::filesystem::create_directory(payload_path_for(path, generation));

  const Result<std::shared_ptr<const UpsState>> read = UpsStore::read_file(path, StoreReadOptions{});
  UC_CHECK_MSG(!read.ok(),
               "a payload that is a directory must never be adopted");
  if (!read.ok()) {
    std::cout << "           note: a directory payload is refused with "
              << to_string(read.status().code()) << "\n";
  }
}

UC_TEST(adversarial, a_payload_reached_through_a_link_is_refused_before_its_content) {
  uc_test::ScratchDirectory scratch("adversarial-payload-symlink");
  const std::filesystem::path path = scratch.file("store.upsstore");
  const Result<std::filesystem::path> created = make_real_store(path);
  UC_REQUIRE_OK(created);
  const Result<std::shared_ptr<const UpsState>> original = UpsStore::read_file(path, StoreReadOptions{});
  UC_REQUIRE_OK(original);
  const std::uint64_t digest = canonical_state_digest(*original.value());
  const StoreGeneration generation = detail::decode_head(read_all(path)).value().generation;
  const std::filesystem::path payload = payload_path_for(path, generation);
  const std::vector<std::byte> intact = read_all(payload);
  UC_REQUIRE(intact.size() > kTrailerBytes);

  const std::filesystem::path target = scratch.file("payload-copy.bin");
  std::error_code error;

  // A store is a pair of files in one directory, not a directory of whatever a
  // link happens to point at. The payload path is therefore held to the same rule
  // as the head path and refused before any content is examined, whichever target
  // the link names.
  const auto expect_refused_through_a_link = [&](const std::filesystem::path& link_target,
                                                 const char* what, StatusCode expected) {
    std::filesystem::remove(payload, error);
    error.clear();
    std::filesystem::create_symlink(link_target, payload, error);
    if (error) {
      UC_CHECK_MSG(false, std::string("the payload symlink for the ") + what +
                              " case could not be created, so it was not exercised: " +
                              error.message());
      return;
    }
    UC_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(payload)));
    const Result<std::shared_ptr<const UpsState>> linked =
        UpsStore::read_file(path, StoreReadOptions{});
    UC_CHECK_MSG(!linked.ok(), std::string("a payload reached through a link was adopted for the ") +
                                   what + " case");
    if (!linked.ok()) {
      UC_CHECK_MSG(linked.status().code() == expected,
                   std::string("the ") + what + " case was refused with " +
                       to_string(linked.status().code()) + " rather than " + to_string(expected));
    }
  };

  // A link to a payload whose bytes fail the integrity check.
  {
    std::vector<std::byte> altered = intact;
    const std::size_t index = altered.size() - kTrailerBytes - 1;
    altered[index] = static_cast<std::byte>(std::to_integer<unsigned char>(altered[index]) ^ 0x01u);
    write_all(target, altered);
    expect_refused_through_a_link(target, "altered target", StatusCode::InvalidArgument);
  }

  // A link that resolves nowhere cannot be distinguished from a missing file, and
  // both are refused as missing. What matters is that nothing is adopted through
  // it.
  expect_refused_through_a_link(scratch.file("missing.bin"), "dangling target",
                                StatusCode::NotFound);

  // A link to a byte-identical payload: still refused, because the refusal is
  // about the path rather than about the bytes it resolves to. The store keeps
  // verifying afterwards, so the refusal left nothing behind.
  {
    std::filesystem::remove(target, error);
    error.clear();
    write_all(target, intact);
    expect_refused_through_a_link(target, "byte-identical target", StatusCode::InvalidArgument);
    std::filesystem::remove(payload, error);
    error.clear();
    write_all(payload, intact);
    const Result<std::shared_ptr<const UpsState>> restored =
        UpsStore::read_file(path, StoreReadOptions{});
    UC_REQUIRE_OK(restored);
    UC_CHECK_EQ(canonical_state_digest(*restored.value()), digest);
  }
}

// ---------------------------------------------------------------------------
// Path attacks
// ---------------------------------------------------------------------------

UC_TEST(adversarial, reserved_device_paths_are_refused) {
  uc_test::ScratchDirectory scratch("adversarial-reserved-names");
  const std::array<const char*, 10> names = {"NUL",      "nul",     "CON",      "con.txt",
                                             "COM1",     "com1.dat", "LPT1",     "lpt1.log",
                                             "PRN",      "AUX"};
  for (const char* name : names) {
    StoreOpenOptions options;
    options.path = scratch.file(name);
    options.access = StoreAccess::ReadWrite;
    options.create_if_missing = true;
    options.created_at = Tick{1000};
    options.epoch = ControlEpoch{1};
    options.incarnation = Incarnation{1};
    const Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options);
    UC_REQUIRE_STATUS(store, StatusCode::InvalidArgument);
    UC_REQUIRE_STATUS(UpsStore::read_file(scratch.file(name), StoreReadOptions{}),
                      StatusCode::InvalidArgument);
  }
}

UC_TEST(adversarial, path_with_an_embedded_nul_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-path-nul");
  std::u8string name = u8"embedded";
  name.push_back(u8'\0');
  name += u8"nul.upsstore";
  const std::filesystem::path path = scratch.path() / std::filesystem::path(name);
  UC_CHECK_MSG(path_to_utf8(path).find('\0') != std::string::npos,
               "the crafted path must really carry a NUL byte");

  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsStore::read_file(path, StoreReadOptions{}), StatusCode::InvalidArgument);
}

UC_TEST(adversarial, path_longer_than_the_limit_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-path-length");
  StoreOpenOptions options;
  options.path = scratch.file("a-file-name-that-is-long-enough-to-exceed-the-configured-limit");
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  options.limits.max_path_bytes = 24;

  const Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options);
  UC_REQUIRE_STATUS(store, StatusCode::LimitExceeded);
  UC_CHECK_MSG(store.status().message().find("24") != std::string::npos,
               "the refusal must name the limit: " + store.status().message());
  StoreReadOptions read_options;
  read_options.limits.max_path_bytes = 24;
  UC_REQUIRE_STATUS(UpsStore::read_file(options.path, read_options), StatusCode::LimitExceeded);
}

UC_TEST(adversarial, path_whose_parent_is_a_file_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-parent-file");
  const std::filesystem::path parent = scratch.file("plain.txt");
  write_text(parent, "not a directory");
  UC_REQUIRE(std::filesystem::is_regular_file(parent));

  StoreOpenOptions options;
  options.path = parent / "store.upsstore";
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsStore::read_file(options.path, StoreReadOptions{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(adversarial, path_that_names_a_directory_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-path-directory");
  StoreOpenOptions options;
  options.path = scratch.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsStore::read_file(scratch.path(), StoreReadOptions{}),
                    StatusCode::InvalidArgument);
}

UC_TEST(adversarial, dangling_symlink_path_is_refused) {
  uc_test::ScratchDirectory scratch("adversarial-dangling-link");
  const std::filesystem::path link = scratch.file("link.upsstore");
  std::error_code error;
  std::filesystem::create_symlink(scratch.file("does-not-exist.upsstore"), link, error);
  if (error) {
    UC_CHECK_MSG(false, "the dangling store symlink could not be created, so this sub-case was not "
                        "exercised: " + error.message());
    return;
  }
  UC_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));

  StoreOpenOptions options;
  options.path = link;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsStore::read_file(link, StoreReadOptions{}), StatusCode::InvalidArgument);

  // A store path that is a symlink to a real store is refused too: a store is
  // never published or read through a link.
  const std::filesystem::path real = scratch.file("real.upsstore");
  const Result<std::filesystem::path> created = make_real_store(real);
  UC_REQUIRE_OK(created);
  const std::filesystem::path good_link = scratch.file("good-link.upsstore");
  std::filesystem::create_symlink(real, good_link, error);
  if (error) {
    UC_CHECK_MSG(false, "the store symlink could not be created: " + error.message());
    return;
  }
  options.path = good_link;
  options.create_if_missing = false;
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsStore::read_file(good_link, StoreReadOptions{}), StatusCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Unicode
// ---------------------------------------------------------------------------

std::string utf8_text(const char8_t* text) {
  return std::string(reinterpret_cast<const char*>(text));
}

UC_TEST(adversarial, unicode_store_path_round_trips) {
  uc_test::ScratchDirectory scratch("adversarial-unicode-path");
  const std::filesystem::path directory =
      scratch.path() / std::filesystem::path(std::u8string(u8"ünïcødé-日本語"));
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  UC_REQUIRE(!error);
  const std::filesystem::path path =
      directory / std::filesystem::path(std::u8string(u8"ups-ストア.upsstore"));

  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};

  const Result<std::shared_ptr<UpsStore>> created = UpsStore::open(options);
  UC_REQUIRE_OK(created);
  UpsState next = *created.value()->opened_state();
  next.generation = StoreGeneration{next.generation.value() + 1};
  next.updated_at = Tick{2000};
  CommitLogEntry entry;
  entry.generation = next.generation;
  entry.operation = OperationKind::Revalidated;
  entry.at = Tick{2000};
  next.commit_log.push_back(entry);
  const Result<CommitResult> committed = created.value()->commit(next);
  UC_REQUIRE_OK(committed);
  UC_REQUIRE_STATUS(created.value()->close(), StatusCode::Ok);

  const Result<std::shared_ptr<const UpsState>> reopened =
      UpsStore::read_file(path, StoreReadOptions{});
  UC_REQUIRE_OK(reopened);
  UC_CHECK_EQ(reopened.value()->generation, next.generation);
  UC_CHECK_EQ(*reopened.value(), next);
  UC_REQUIRE_STATUS(UpsStore::verify_file(path, StoreReadOptions{}), StatusCode::Ok);

  // path_to_utf8 must not throw for a path that is not in the active code page,
  // and must return the UTF-8 bytes unchanged.
  const std::string text = path_to_utf8(path);
  const std::string separator(1, std::filesystem::path::preferred_separator);
  UC_CHECK_EQ(text, path_to_utf8(scratch.path()) + separator + utf8_text(u8"ünïcødé-日本語") +
                        separator + utf8_text(u8"ups-ストア.upsstore"));
  UC_CHECK(text.find(utf8_text(u8"日本語")) != std::string::npos);
  UC_CHECK(text.find(utf8_text(u8"ストア")) != std::string::npos);
}

UC_TEST(adversarial, labels_must_be_valid_utf8) {
  uc_test::Fixture fixture("adversarial-labels");
  UC_REQUIRE(fixture.build());
  UC_REQUIRE_OK(fixture.current());

  // An overlong encoding, a lone continuation byte, a truncated sequence, and a
  // surrogate half are all refused rather than normalized.
  UC_REQUIRE_STATUS(validate_label(std::string("\xC0\xAF", 2), 256), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(validate_label(std::string("\x80", 1), 256), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(validate_label(std::string("\xE6\x97", 2), 256), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(validate_label(std::string("\xED\xA0\x80", 3), 256), StatusCode::InvalidArgument);

  // The same forms are refused end to end by the engine.
  const std::array<std::string, 2> invalid = {std::string("\xC0\xAF", 2), std::string("\x80", 1)};
  for (std::size_t index = 0; index < invalid.size(); ++index) {
    RegisterUpsRequest request;
    request.authority = fixture.context;
    request.now = Tick{1100};
    request.id = UpsId::parse(std::string("bad-label-") + std::to_string(index)).value();
    request.label = invalid[index];
    request.hardware = HardwareGeneration{2};
    request.lifecycle = LifecycleState::Commissioning;
    request.operating = OperatingState::Offline;
    UC_REQUIRE_STATUS(fixture.engine->register_ups(request), StatusCode::InvalidArgument);
  }

  // A valid multi-byte label is accepted and survives the store.
  const std::string label = utf8_text(u8"日本語 UPS ✓");
  {
    RegisterUpsRequest request;
    request.authority = fixture.context;
    request.now = Tick{1100};
    request.id = UpsId::parse("unicode-label").value();
    request.label = label;
    request.hardware = HardwareGeneration{2};
    request.lifecycle = LifecycleState::Commissioning;
    request.operating = OperatingState::Offline;
    const Result<UpsRecord> registered = fixture.engine->register_ups(request);
    UC_REQUIRE_OK(registered);
    UC_CHECK_EQ(registered.value().label, label);
  }
  const Result<std::vector<UpsRecord>> units = fixture.engine->units();
  UC_REQUIRE_OK(units);
  UC_CHECK_EQ(units.value().size(), std::size_t{2});
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  const Result<std::shared_ptr<const UpsState>> state =
      UpsStore::read_file(fixture.store_options.path, StoreReadOptions{});
  UC_REQUIRE_OK(state);
  const UpsId unicode_id = UpsId::parse("unicode-label").value();
  const UpsRecord* stored = find_ups(*state.value(), unicode_id);
  UC_REQUIRE(stored != nullptr);
  if (stored != nullptr) {
    UC_CHECK_EQ(stored->label, label);
  }
}

// ---------------------------------------------------------------------------
// Reordered and duplicate events
// ---------------------------------------------------------------------------

UC_TEST(adversarial, reordered_telemetry_is_refused) {
  uc_test::Fixture fixture("adversarial-telemetry-order");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));
  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE_OK(unit);
  const UpsRef ref{unit.value().id, unit.value().hardware, unit.value().revision};

  // An observation instant older than the recorded one is never merged.
  TelemetryReport older = uc_test::healthy_report(ref.ups, ref.hardware, Tick{900}, SourceRevision{5},
                                                  "ev-older");
  UC_REQUIRE_STATUS(fixture.observe(older, Tick{1001}), StatusCode::StaleSourceGeneration);

  // The same instant with a source revision that did not advance is refused too.
  TelemetryReport repeated = uc_test::healthy_report(ref.ups, ref.hardware, Tick{1000},
                                                     SourceRevision{1}, "ev-repeated");
  UC_REQUIRE_STATUS(fixture.observe(repeated, Tick{1001}), StatusCode::StaleSourceGeneration);

  // A newer source revision at the same instant is accepted, so the refusals
  // above are about ordering and not about the instant alone.
  TelemetryReport newer = uc_test::healthy_report(ref.ups, ref.hardware, Tick{1000},
                                                  SourceRevision{2}, "ev-newer");
  UC_REQUIRE_STATUS(fixture.observe(newer, Tick{1001}), StatusCode::Ok);
  const Result<std::vector<UpsRecord>> after = fixture.engine->units();
  UC_REQUIRE_OK(after);
  UC_REQUIRE(after.value().size() == 1);
  UC_CHECK_EQ(after.value().front().observation->revision, SourceRevision{2});
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
}

UC_TEST(adversarial, registering_the_same_unit_twice_is_refused) {
  uc_test::Fixture fixture("adversarial-duplicate-unit");
  UC_REQUIRE(fixture.build());
  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE_OK(unit);

  RegisterUpsRequest request;
  request.authority = fixture.context;
  request.now = Tick{1100};
  request.id = unit.value().id;
  request.label = "second registration";
  request.hardware = unit.value().hardware;
  request.lifecycle = LifecycleState::InService;
  request.operating = OperatingState::OnlineNormal;
  UC_REQUIRE_STATUS(fixture.engine->register_ups(request), StatusCode::AlreadyExists);

  const Result<std::vector<UpsRecord>> units = fixture.engine->units();
  UC_REQUIRE_OK(units);
  UC_CHECK_EQ(units.value().size(), std::size_t{1});
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
}

UC_TEST(adversarial, rebinding_the_same_obligation_reference_bumps_one_binding) {
  uc_test::Fixture fixture("adversarial-obligation-collapse");
  UC_REQUIRE(fixture.build());
  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE_OK(unit);

  const auto bind = [&fixture](const UpsRef& ref, Tick now, const std::string& load,
                               const std::string& note) {
    BindObligationRequest request;
    request.authority = fixture.context;
    request.ref = ref;
    request.now = now;
    request.obligation.ref = ObligationRef::parse("obl-same").value();
    request.obligation.load = LoadId::parse(load).value();
    request.obligation.tier = ObligationTier::Critical;
    request.obligation.protection = ProtectionRequirement::MustRemainProtected;
    request.obligation.asserted_by = AuthorityRef::parse("facility-ops").value();
    request.obligation.asserted_at = now;
    request.obligation.expires_at = Tick{900000};
    request.obligation.release_reason = note;
    return fixture.engine->bind_obligation(request);
  };

  const Result<ProtectedLoadObligation> first =
      bind(UpsRef{unit.value().id, unit.value().hardware, unit.value().revision}, Tick{1100},
           "load-1", "");
  UC_REQUIRE_OK(first);
  UC_CHECK_EQ(first.value().revision, ObligationRevision{1});

  const Result<UpsRecord> after_first = fixture.current();
  UC_REQUIRE_OK(after_first);
  const Result<ProtectedLoadObligation> second =
      bind(UpsRef{after_first.value().id, after_first.value().hardware, after_first.value().revision},
           Tick{1200}, "load-2", "");
  UC_REQUIRE_OK(second);
  UC_CHECK_MSG(second.value().revision == ObligationRevision{2},
               "rebinding the same reference must bump the binding revision, got " +
                   std::to_string(second.value().revision.value()));
  UC_CHECK_EQ(second.value().load.value(), std::string("load-2"));

  const Result<std::vector<UpsRecord>> units = fixture.engine->units();
  UC_REQUIRE_OK(units);
  UC_REQUIRE(units.value().size() == 1);
  UC_CHECK_MSG(units.value().front().obligations.size() == 1,
               "the two bindings must collapse into one, got " +
                   std::to_string(units.value().front().obligations.size()));
  if (units.value().front().obligations.size() == 1) {
    UC_CHECK_EQ(units.value().front().obligations.front().ref.value(), std::string("obl-same"));
    UC_CHECK_EQ(units.value().front().obligations.front().revision, ObligationRevision{2});
    UC_CHECK_EQ(units.value().front().obligations.front().load.value(), std::string("load-2"));
  }
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Integer boundaries
// ---------------------------------------------------------------------------

UC_TEST(adversarial, reserve_integer_boundaries_are_refused_or_exact) {
  const std::int64_t maximum = (std::numeric_limits<std::int64_t>::max)();

  // Above the configured bound: refused explicitly, never wrapped.
  const Result<ReserveQuantity> over =
      make_reserve_quantity(ReserveUnit::MilliwattHours, maximum);
  UC_REQUIRE_STATUS(over, StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(make_reserve_quantity(ReserveUnit::Seconds, maximum), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(make_reserve_quantity(ReserveUnit::MilliwattHours, -1),
                    StatusCode::InvalidArgument);

  // At the bound: accepted exactly, with no silent clamping.
  ResourceLimits wide = ResourceLimits::defaults();
  wide.max_reserve_milliwatt_hours = maximum;
  wide.max_reserve_seconds = maximum;
  const Result<ReserveQuantity> milliwatt_hours =
      make_reserve_quantity(ReserveUnit::MilliwattHours, maximum, wide);
  UC_REQUIRE_OK(milliwatt_hours);
  UC_CHECK_EQ(milliwatt_hours.value().value, maximum);
  const Result<ReserveQuantity> seconds = make_reserve_quantity(ReserveUnit::Seconds, maximum, wide);
  UC_REQUIRE_OK(seconds);
  UC_CHECK_EQ(seconds.value().value, maximum);
  UC_REQUIRE_STATUS(make_reserve_quantity(ReserveUnit::BasisPoints, maximum, wide),
                    StatusCode::InvalidArgument);
}

UC_TEST(adversarial, crafted_integer_boundaries_round_trip_without_wrapping) {
  uc_test::ScratchDirectory scratch("adversarial-integer-boundaries");
  const std::filesystem::path path = scratch.file("boundaries.upsstore");
  const std::int64_t tick_max = (std::numeric_limits<std::int64_t>::max)();
  const std::uint64_t ordinal_max = (std::numeric_limits<std::uint64_t>::max)();

  CraftedState state = crafted_state_with_one_unit();
  state.created_at = tick_max;
  state.updated_at = tick_max;
  state.revalidated_at = tick_max;
  state.operation_count = ordinal_max;
  state.last_attempt = ordinal_max;
  state.commit_log.front().at = tick_max;
  state.units.front().state_since = tick_max;
  state.units.front().registered_at = tick_max;
  state.units.front().last_verified_at = tick_max;
  state.units.front().revision = ordinal_max;
  CraftedAttempt attempt;
  attempt.id = ordinal_max;
  attempt.planned_revision = ordinal_max;
  state.attempts.push_back(attempt);

  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state);
  UC_REQUIRE_OK(read);
  UC_CHECK_EQ(read.value()->created_at, Tick{tick_max});
  UC_CHECK_EQ(read.value()->updated_at, Tick{tick_max});
  UC_CHECK_EQ(read.value()->revalidated_at, Tick{tick_max});
  UC_CHECK_EQ(read.value()->operation_count, ordinal_max);
  UC_CHECK_EQ(read.value()->last_attempt, AttemptId{ordinal_max});
  UC_REQUIRE(read.value()->units.size() == 1);
  if (!read.value()->units.empty()) {
    UC_CHECK_EQ(read.value()->units.front().revision, StateRevision{ordinal_max});
    UC_CHECK_EQ(read.value()->units.front().state_since, Tick{tick_max});
    UC_CHECK_EQ(read.value()->units.front().registered_at, Tick{tick_max});
    UC_CHECK_EQ(read.value()->units.front().last_verified_at, Tick{tick_max});
  }
  UC_REQUIRE(read.value()->attempts.size() == 1);
  if (!read.value()->attempts.empty()) {
    UC_CHECK_EQ(read.value()->attempts.front().id, AttemptId{ordinal_max});
    UC_CHECK_EQ(read.value()->attempts.front().planned_revision, StateRevision{ordinal_max});
  }
  UC_CHECK_EQ(read.value()->commit_log.front().at, Tick{tick_max});
}

UC_TEST(adversarial, negative_instants_are_refused_rather_than_wrapped) {
  uc_test::ScratchDirectory scratch("adversarial-negative-instant");
  const std::filesystem::path path = scratch.file("negative.upsstore");

  {
    CraftedState state = crafted_state_with_one_unit();
    state.created_at = -1;
    state.updated_at = -1;
    state.commit_log.front().at = -1;
    const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state);
    UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  }
  {
    CraftedState state = crafted_state_with_one_unit();
    state.updated_at = (std::numeric_limits<std::int64_t>::min)();
    state.commit_log.front().at = (std::numeric_limits<std::int64_t>::min)();
    const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state);
    UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  }
}

UC_TEST(adversarial, obligation_expiry_at_the_integer_boundary_survives_and_binds) {
  uc_test::Fixture fixture("adversarial-expiry-boundary");
  UC_REQUIRE(fixture.build());
  const Result<UpsRecord> unit = fixture.current();
  UC_REQUIRE_OK(unit);
  const std::int64_t tick_max = (std::numeric_limits<std::int64_t>::max)();

  BindObligationRequest request;
  request.authority = fixture.context;
  request.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
  request.now = Tick{1100};
  request.obligation.ref = ObligationRef::parse("obl-max").value();
  request.obligation.load = LoadId::parse("load-max").value();
  request.obligation.tier = ObligationTier::LifeSafety;
  request.obligation.protection = ProtectionRequirement::MustRemainProtected;
  request.obligation.asserted_by = AuthorityRef::parse("facility-ops").value();
  request.obligation.asserted_at = Tick{1100};
  request.obligation.expires_at = Tick{tick_max};
  const Result<ProtectedLoadObligation> bound = fixture.engine->bind_obligation(request);
  UC_REQUIRE_OK(bound);
  UC_CHECK_EQ(bound.value().expires_at.value(), Tick{tick_max});
  // The declared window is half open: it binds strictly before its expiry and has
  // lapsed at the expiry instant itself, with no wrap at the integer boundary.
  UC_CHECK(obligation_binds(bound.value(), Tick{tick_max - 1}));
  UC_CHECK(!obligation_lapsed(bound.value(), Tick{tick_max - 1}));
  UC_CHECK(!obligation_binds(bound.value(), Tick{tick_max}));
  UC_CHECK(obligation_lapsed(bound.value(), Tick{tick_max}));
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  const Result<std::shared_ptr<const UpsState>> state =
      UpsStore::read_file(fixture.store_options.path, StoreReadOptions{});
  UC_REQUIRE_OK(state);
  const UpsRecord* stored = find_ups(*state.value(), unit.value().id);
  UC_REQUIRE(stored != nullptr);
  if (stored != nullptr) {
    UC_REQUIRE(!stored->obligations.empty());
    if (!stored->obligations.empty()) {
      UC_CHECK_EQ(stored->obligations.front().expires_at.value(), Tick{tick_max});
      UC_CHECK_EQ(stored->obligations.front(), bound.value());
    }
  }
}

// ---------------------------------------------------------------------------
// Resource limits on an open
// ---------------------------------------------------------------------------

UC_TEST(adversarial, opening_with_tiny_limits_refuses_the_declaration_and_names_the_limit) {
  uc_test::ScratchDirectory scratch("adversarial-tiny-limits");
  const std::filesystem::path path = scratch.file("tiny.upsstore");
  CraftedState state = crafted_state_with_one_unit();
  CraftedUnit second;
  second.id = "unit-b";
  state.units.push_back(second);
  write_crafted_store(path, state, CraftedEnvelope{});

  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = false;
  options.limits.max_ups_units = 1;
  const Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options);
  UC_REQUIRE_STATUS(store, StatusCode::LimitExceeded);
  UC_CHECK_MSG(store.status().message().find("limit is 1") != std::string::npos,
               "the refusal must name the limit: " + store.status().message());

  // The same artifact opens under the default limits, so the refusal is about
  // the configured bound.
  StoreOpenOptions defaults = options;
  defaults.limits = ResourceLimits::defaults();
  const Result<std::shared_ptr<UpsStore>> accepted = UpsStore::open(defaults);
  UC_REQUIRE_OK(accepted);
  UC_CHECK_EQ(accepted.value()->opened_state()->units.size(), std::size_t{2});
  UC_REQUIRE_STATUS(accepted.value()->close(), StatusCode::Ok);
}
