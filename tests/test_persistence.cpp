// Persistence proof obligations: the artifact format and the publication
// protocol.
//
// Every test here is a proof obligation about the durable artifact: what the
// bytes are, what they mean, and what is refused. The adversarial artifacts are
// produced with the same codec and checksum the library itself uses, and every
// crafted artifact is first proven to be accepted in its valid form, so a
// refusal can never come from a mistake in the crafted encoder.

#include "test_harness.hpp"

#include "fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "detail/codec.hpp"
#include "detail/crc32c.hpp"
#include "detail/serialization.hpp"
#include "ups_control/engine.hpp"
#include "ups_control/state.hpp"
#include "ups_control/store.hpp"

using namespace ups_control;

namespace {

// ---------------------------------------------------------------------------
// Byte-level file helpers
// ---------------------------------------------------------------------------

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

std::string to_text(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte value : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
  }
  return text;
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

std::filesystem::path derived(const std::filesystem::path& target, const std::string& suffix) {
  std::filesystem::path result = target;
  std::string name = path_to_utf8(target.filename());
  name += suffix;
  result.replace_filename(std::filesystem::path(std::u8string(name.begin(), name.end())));
  return result;
}

std::filesystem::path payload_path_for(const std::filesystem::path& store, StoreGeneration generation) {
  return derived(store, ".g" + hex16(generation.value()));
}

std::uint32_t load_u32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
             << static_cast<unsigned>(8 * index);
  }
  return value;
}

std::uint64_t load_u64(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index]))
             << static_cast<unsigned>(8 * index);
  }
  return value;
}

void store_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    const unsigned shift = static_cast<unsigned>(8 * index);
    bytes[offset + index] =
        static_cast<std::byte>(static_cast<unsigned char>((value >> shift) & 0xFFu));
  }
}

void store_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    const unsigned shift = static_cast<unsigned>(8 * index);
    bytes[offset + index] =
        static_cast<std::byte>(static_cast<unsigned char>((value >> shift) & 0xFFu));
  }
}

/// Renders a byte range for a failure message.
std::string describe_bytes(std::span<const std::byte> bytes) {
  static const char* digits = "0123456789abcdef";
  std::string text;
  for (const std::byte value : bytes) {
    const unsigned char byte = std::to_integer<unsigned char>(value);
    text.push_back(digits[static_cast<std::size_t>(byte >> 4)]);
    text.push_back(digits[static_cast<std::size_t>(byte & 0x0Fu)]);
  }
  return text;
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
// Crafted canonical state: the same field order and the same primitives as the
// library's own encoder, but with the validation switched off so that a
// structurally invalid state can be framed in a valid envelope.
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
  std::uint8_t has_observation = 0;
  std::uint8_t has_floor = 0;
  std::uint8_t floor_unit = 0;
  std::int64_t floor_value = 0;
  std::int64_t max_evidence_age = 600;
  std::int64_t max_future_skew = 0;
  std::uint8_t minimum_quality = 2;  // Estimated
  std::optional<std::uint32_t> obligation_count;
  std::optional<std::uint32_t> grant_count;
  std::uint8_t has_in_flight = 0;
  std::uint64_t in_flight = 0;
  std::int64_t last_verified_at = 0;
};

struct CraftedAttempt {
  std::uint64_t id = 1;
  std::string key = "key-1";
  std::string ups = "unit-a";
  std::uint32_t hardware = 1;
  std::uint64_t epoch = 1;
  std::uint64_t incarnation = 1;
  std::uint64_t planned_revision = 1;
  std::uint8_t command = 1;  // EnterStaticBypass
  std::uint8_t target = 5;   // StaticBypass
  std::int64_t submitted_at = 1200;
  std::uint8_t phase = 1;  // Planned
  std::uint64_t plan_digest = 0x1122334455667788ull;
};

struct CraftedIdempotency {
  std::string key = "key-1";
  std::uint64_t attempt = 1;
  std::uint64_t digest = 0x1122334455667788ull;
  std::int64_t recorded_at = 1200;
};

struct CraftedCommit {
  std::uint64_t generation = 1;
  std::uint64_t attempt = 0;
  std::string key;
  std::uint8_t operation = 1;  // StoreCreated
  std::string ups;
  std::uint64_t revision = 0;
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
};

void write_crafted_unit(detail::ByteWriter& writer, const CraftedUnit& unit) {
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
  writer.u8(unit.has_observation);
  writer.u8(unit.has_floor);
  writer.u8(unit.floor_unit);
  writer.i64(unit.floor_value);
  writer.i64(unit.max_evidence_age);
  writer.i64(unit.max_future_skew);
  writer.u8(unit.minimum_quality);
  writer.u32(unit.obligation_count.value_or(0));
  writer.u32(unit.grant_count.value_or(0));
  writer.u8(unit.has_in_flight);
  writer.u64(unit.in_flight);
  writer.i64(unit.last_verified_at);
}

void write_crafted_attempt(detail::ByteWriter& writer, const CraftedAttempt& attempt) {
  writer.u64(attempt.id);
  (void)writer.text(attempt.key, 64);
  (void)writer.text(attempt.ups, 128);
  writer.u32(attempt.hardware);
  writer.u64(attempt.epoch);
  writer.u64(attempt.incarnation);
  writer.u64(attempt.planned_revision);
  writer.u8(attempt.command);
  // command parameters: asserted target, reserve requirement, verification dwell
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.i64(0);
  writer.i64(0);
  writer.u8(attempt.target);
  writer.i64(attempt.submitted_at);
  writer.u8(attempt.phase);
  writer.u64(attempt.plan_digest);
  // refusal
  writer.u8(0);
  writer.u8(0);
  (void)writer.text(std::string(), 512);
  // acknowledgement
  writer.u8(0);
  (void)writer.text(std::string(), 512);
  writer.i64(0);
  // observation
  writer.u8(0);
  writer.i64(0);
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  // verification
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
    write_crafted_unit(writer, unit);
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
    writer.u64(entry.digest);
    writer.i64(entry.recorded_at);
  }
  writer.u32(state.declare_commit_count.value_or(static_cast<std::uint32_t>(state.commit_log.size())));
  for (const CraftedCommit& entry : state.commit_log) {
    writer.u64(entry.generation);
    writer.u64(entry.attempt);
    (void)writer.text(entry.key, 64);
    writer.u8(entry.operation);
    (void)writer.text(entry.ups, 128);
    writer.u64(entry.revision);
    writer.i64(entry.at);
  }
  return writer.take();
}

/// A minimal but structurally valid state with one unit.
CraftedState valid_crafted_state(const std::string& unit_id = "unit-a") {
  CraftedState state;
  CraftedUnit unit;
  unit.id = unit_id;
  unit.label = "crafted unit";
  state.units.push_back(unit);
  CraftedCommit commit;
  commit.generation = 1;
  commit.operation = 1;  // StoreCreated
  commit.at = 1000;
  state.commit_log.push_back(commit);
  return state;
}

StoreIdentity crafted_identity() {
  return StoreIdentity::from_components(0x0102030405060708ull, 0x1112131415161718ull).value();
}

std::vector<std::byte> build_crafted_payload(const std::string& recorded_path,
                                             const std::vector<std::byte>& state_bytes,
                                             std::uint64_t generation, std::uint64_t epoch,
                                             std::uint64_t incarnation,
                                             std::uint32_t& payload_crc_out) {
  detail::ByteWriter writer;
  for (const char byte : kPayloadMagic) {
    writer.u8(static_cast<std::uint8_t>(byte));
  }
  writer.u32(kFormatVersion);
  writer.u32(kByteOrderMarker);
  writer.u64(generation);
  writer.u64(epoch);
  writer.u64(incarnation);
  writer.u64(crafted_identity().high());
  writer.u64(crafted_identity().low());
  writer.u32(static_cast<std::uint32_t>(recorded_path.size()));
  writer.u32(static_cast<std::uint32_t>(state_bytes.size()));
  writer.u32(detail::crc32c(state_bytes));
  writer.u32(0);
  writer.raw(std::span<const std::byte>(reinterpret_cast<const std::byte*>(recorded_path.data()),
                                        recorded_path.size()));
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

/// Writes a complete crafted store: the head marker at the path and the payload
/// it names.
void write_crafted_store(const std::filesystem::path& path, const CraftedState& state) {
  const std::string recorded_path = path_to_utf8(path);
  const std::vector<std::byte> state_bytes = encode_crafted_state(state);
  std::uint32_t payload_crc = 0;
  const std::vector<std::byte> payload = build_crafted_payload(
      recorded_path, state_bytes, state.generation, state.epoch, state.incarnation, payload_crc);

  detail::HeadRecord head;
  head.identity = crafted_identity();
  head.generation = StoreGeneration{state.generation};
  head.epoch = ControlEpoch{state.epoch};
  head.incarnation = Incarnation{state.incarnation};
  head.payload_bytes = payload.size();
  head.payload_crc32c = payload_crc;
  head.path_crc32c = detail::crc32c(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(recorded_path.data()), recorded_path.size()));
  head.head_sequence = 1;
  head.committed_at = Tick{state.updated_at};
  head.created_at = Tick{state.created_at};

  write_all(path, detail::encode_head(head).value());
  write_all(payload_path_for(path, head.generation), payload);
}

Result<std::shared_ptr<const UpsState>> read_crafted(const std::filesystem::path& path,
                                                     const CraftedState& state,
                                                     const StoreReadOptions& options) {
  write_crafted_store(path, state);
  return UpsStore::read_file(path, options);
}

Result<std::shared_ptr<const UpsState>> read_crafted(const std::filesystem::path& path,
                                                     const CraftedState& state) {
  return read_crafted(path, state, StoreReadOptions{});
}

// ---------------------------------------------------------------------------
// Real-store mutation helpers
// ---------------------------------------------------------------------------

detail::HeadRecord head_of(const std::filesystem::path& path) {
  return detail::decode_head(read_all(path)).value();
}

std::vector<std::byte> payload_of(const std::filesystem::path& path) {
  return read_all(payload_path_for(path, head_of(path).generation));
}

/// Rewrites the committed payload. When refresh_head is set the head is rebound
/// to the new payload, so the envelope stays valid.
void rewrite_payload(const std::filesystem::path& path, const std::vector<std::byte>& payload,
                     bool refresh_head) {
  detail::HeadRecord head = head_of(path);
  write_all(payload_path_for(path, head.generation), payload);
  if (!refresh_head) {
    return;
  }
  head.payload_bytes = payload.size();
  head.payload_crc32c = detail::crc32c(std::span<const std::byte>(
      payload.data() + kPayloadHeaderBytes, payload.size() - kPayloadHeaderBytes));
  write_all(path, detail::encode_head(head).value());
}

/// Recomputes the payload trailer checksum so that the trailer check passes. The
/// documented trailer covers everything before the trailer magic, so the last
/// twelve bytes are excluded.
void refresh_trailer(std::vector<std::byte>& payload) {
  const std::uint32_t crc = detail::crc32c(
      std::span<const std::byte>(payload.data(), payload.size() - kTrailerBytes));
  store_u32(payload, payload.size() - 4, crc);
}

// ---------------------------------------------------------------------------
// State comparison
// ---------------------------------------------------------------------------

void expect_observation_identical(const ObservationRecord& expected, const ObservationRecord& actual,
                                  const std::string& where) {
  UC_CHECK_MSG(expected.evidence == actual.evidence, where + ": observation evidence");
  UC_CHECK_MSG(expected.source == actual.source, where + ": observation source");
  UC_CHECK_MSG(expected.provenance == actual.provenance, where + ": observation provenance");
  UC_CHECK_MSG(expected.revision == actual.revision, where + ": observation source revision");
  UC_CHECK_MSG(expected.observed_at == actual.observed_at, where + ": observation instant");
  UC_CHECK_MSG(expected.received_at == actual.received_at, where + ": observation receipt instant");
  UC_CHECK_MSG(expected.operating == actual.operating, where + ": observation operating state");
  UC_CHECK_MSG(expected.transfer == actual.transfer, where + ": observation transfer status");
  UC_CHECK_MSG(expected.capability == actual.capability, where + ": observation capability");
  UC_CHECK_MSG(expected.battery == actual.battery, where + ": observation battery reading");
  UC_CHECK_MSG(expected.fault_present == actual.fault_present, where + ": observation fault flag");
  UC_CHECK_MSG(expected.contradictory == actual.contradictory, where + ": observation contradiction");
  UC_CHECK_MSG(expected.contradiction_detail == actual.contradiction_detail,
               where + ": observation contradiction detail");
}

void expect_unit_identical(const UpsRecord& expected, const UpsRecord& actual, const std::string& where) {
  UC_CHECK_MSG(expected.id == actual.id, where + ": unit id");
  UC_CHECK_MSG(expected.label == actual.label, where + ": unit label");
  UC_CHECK_MSG(expected.hardware == actual.hardware, where + ": unit hardware generation");
  UC_CHECK_MSG(expected.lifecycle == actual.lifecycle, where + ": unit lifecycle");
  UC_CHECK_MSG(expected.operating == actual.operating, where + ": unit operating state");
  UC_CHECK_MSG(expected.basis == actual.basis, where + ": unit state basis");
  UC_CHECK_MSG(expected.revision == actual.revision, where + ": unit state revision");
  UC_CHECK_MSG(expected.state_since == actual.state_since, where + ": unit state instant");
  UC_CHECK_MSG(expected.registered_at == actual.registered_at, where + ": unit registration instant");
  UC_CHECK_MSG(expected.registration_note == actual.registration_note, where + ": unit note");
  UC_CHECK_MSG(expected.reserve_policy == actual.reserve_policy, where + ": unit reserve policy");
  UC_CHECK_MSG(expected.in_flight == actual.in_flight, where + ": unit in-flight gate");
  UC_CHECK_MSG(expected.last_verified_at == actual.last_verified_at, where + ": unit last verified");
  UC_CHECK_MSG(expected.obligations.size() == actual.obligations.size(), where + ": obligation count");
  for (std::size_t index = 0; index < expected.obligations.size() && index < actual.obligations.size();
       ++index) {
    UC_CHECK_MSG(expected.obligations[index] == actual.obligations[index],
                 where + ": obligation " + std::to_string(index));
  }
  UC_CHECK_MSG(expected.grants.size() == actual.grants.size(), where + ": grant count");
  for (std::size_t index = 0; index < expected.grants.size() && index < actual.grants.size(); ++index) {
    UC_CHECK_MSG(expected.grants[index] == actual.grants[index],
                 where + ": grant " + std::to_string(index));
  }
  UC_CHECK_MSG(expected.observation.has_value() == actual.observation.has_value(),
               where + ": observation presence");
  if (expected.observation.has_value() && actual.observation.has_value()) {
    expect_observation_identical(expected.observation.value(), actual.observation.value(), where);
  }
}

void expect_state_identical(const UpsState& expected, const UpsState& actual, const std::string& where) {
  UC_CHECK_MSG(expected.generation == actual.generation, where + ": store generation");
  UC_CHECK_MSG(expected.epoch == actual.epoch, where + ": control epoch");
  UC_CHECK_MSG(expected.incarnation == actual.incarnation, where + ": controller incarnation");
  UC_CHECK_MSG(expected.created_at == actual.created_at, where + ": creation instant");
  UC_CHECK_MSG(expected.updated_at == actual.updated_at, where + ": update instant");
  UC_CHECK_MSG(expected.revalidated_at == actual.revalidated_at, where + ": revalidation instant");
  UC_CHECK_MSG(expected.last_attempt == actual.last_attempt, where + ": last attempt ordinal");
  UC_CHECK_MSG(expected.operation_count == actual.operation_count, where + ": operation count");
  UC_CHECK_MSG(expected.units.size() == actual.units.size(), where + ": unit count");
  for (std::size_t index = 0; index < expected.units.size() && index < actual.units.size(); ++index) {
    expect_unit_identical(expected.units[index], actual.units[index],
                          where + ": unit " + std::to_string(index));
  }
  UC_CHECK_MSG(expected.attempts.size() == actual.attempts.size(), where + ": attempt count");
  for (std::size_t index = 0; index < expected.attempts.size() && index < actual.attempts.size();
       ++index) {
    UC_CHECK_MSG(expected.attempts[index] == actual.attempts[index],
                 where + ": attempt " + std::to_string(index));
  }
  UC_CHECK_MSG(expected.idempotency.size() == actual.idempotency.size(), where + ": idempotency count");
  for (std::size_t index = 0;
       index < expected.idempotency.size() && index < actual.idempotency.size(); ++index) {
    UC_CHECK_MSG(expected.idempotency[index] == actual.idempotency[index],
                 where + ": idempotency binding " + std::to_string(index));
  }
  UC_CHECK_MSG(expected.commit_log.size() == actual.commit_log.size(), where + ": commit log count");
  for (std::size_t index = 0; index < expected.commit_log.size() && index < actual.commit_log.size();
       ++index) {
    UC_CHECK_MSG(expected.commit_log[index] == actual.commit_log[index],
                 where + ": commit log entry " + std::to_string(index));
  }
  UC_CHECK_MSG(expected == actual, where + ": whole state");
}

/// A minimal valid state built directly as a value, for canonical determinism.
UpsState canonical_state_fixture(std::uint64_t generation) {
  UpsState state;
  state.generation = StoreGeneration{generation};
  state.epoch = ControlEpoch{1};
  state.incarnation = Incarnation{1};
  state.created_at = Tick{1000};
  state.updated_at = Tick{1000};
  UpsRecord unit;
  unit.id = UpsId::parse("unit-a").value();
  unit.label = "unit a";
  unit.hardware = HardwareGeneration{1};
  unit.lifecycle = LifecycleState::InService;
  unit.operating = OperatingState::OnlineNormal;
  unit.basis = StateBasis::Observed;
  unit.revision = StateRevision{1};
  unit.state_since = Tick{1000};
  unit.registered_at = Tick{1000};
  unit.reserve_policy.max_evidence_age = TickSpan{600};
  state.units.push_back(unit);
  CommitLogEntry entry;
  entry.generation = state.generation;
  entry.operation = OperationKind::StoreCreated;
  entry.at = Tick{1000};
  state.commit_log.push_back(entry);
  return state;
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

UC_TEST(persistence, round_trip_reopen_preserves_every_field) {
  uc_test::Fixture fixture("persistence-round-trip");
  fixture.script.effects_log = fixture.scratch->file("effects.log");
  UC_REQUIRE(fixture.build(GrantScope::BypassTransfer, ReserveUnit::Seconds, 600));
  const std::filesystem::path store_path = fixture.store_options.path;

  const Result<UpsRecord> initial = fixture.current();
  UC_REQUIRE_OK(initial);
  const UpsRef initial_ref{initial.value().id, initial.value().hardware, initial.value().revision};

  // --- a protected-load obligation, and a second one that is released ---
  {
    BindObligationRequest request;
    request.authority = fixture.context;
    request.ref = initial_ref;
    request.now = Tick{1100};
    request.obligation.ref = ObligationRef::parse("obl-keep").value();
    request.obligation.load = LoadId::parse("load-1").value();
    request.obligation.tier = ObligationTier::LifeSafety;
    request.obligation.protection = ProtectionRequirement::MustRemainProtected;
    request.obligation.asserted_by = AuthorityRef::parse("facility-ops").value();
    request.obligation.asserted_at = Tick{1100};
    request.obligation.expires_at = Tick{900000};
    UC_REQUIRE_STATUS(fixture.engine->bind_obligation(request), StatusCode::Ok);
  }
  {
    const Result<UpsRecord> unit = fixture.current();
    UC_REQUIRE_OK(unit);
    BindObligationRequest request;
    request.authority = fixture.context;
    request.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
    request.now = Tick{1200};
    request.obligation.ref = ObligationRef::parse("obl-release").value();
    request.obligation.load = LoadId::parse("load-2").value();
    request.obligation.tier = ObligationTier::Essential;
    request.obligation.protection = ProtectionRequirement::MayBeInterruptedWithAuthority;
    request.obligation.asserted_by = AuthorityRef::parse("facility-ops").value();
    request.obligation.asserted_at = Tick{1200};
    UC_REQUIRE_STATUS(fixture.engine->bind_obligation(request), StatusCode::Ok);
  }
  {
    const Result<UpsRecord> unit = fixture.current();
    UC_REQUIRE_OK(unit);
    ReleaseObligationRequest request;
    request.authority = fixture.context;
    request.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
    request.now = Tick{1300};
    request.obligation = ObligationRef::parse("obl-release").value();
    request.obligation_revision = ObligationRevision{1};
    request.release_authority = AuthorityRef::parse("facility-ops").value();
    request.reason = "planned service window";
    request.outcome = ObligationState::Released;
    UC_REQUIRE_STATUS(fixture.engine->release_obligation(request), StatusCode::Ok);
  }

  // --- a second grant ---
  {
    const Result<UpsRecord> unit = fixture.current();
    UC_REQUIRE_OK(unit);
    IssueGrantRequest request;
    request.authority = fixture.context;
    request.ref = UpsRef{unit.value().id, unit.value().hardware, unit.value().revision};
    request.now = Tick{1400};
    request.grant.ref = AuthorityRef::parse("grant-test").value();
    request.grant.scope = GrantScope::TestExecution;
    request.grant.granted_by = AuthorityRef::parse("facility-ops").value();
    request.grant.issued_at = Tick{1400};
    request.grant.expires_at = Tick{900000};
    request.grant.note = "self test window";
    UC_REQUIRE_STATUS(fixture.engine->issue_grant(request), StatusCode::Ok);
  }

  // --- a control attempt that is acknowledged, observed and verified ---
  AttemptId verified_attempt;
  {
    const Result<AttemptRecord> submitted =
        fixture.submit(CommandKind::EnterStaticBypass, "bypass-key", Tick{1500}, "test-authority");
    UC_REQUIRE_OK(submitted);
    verified_attempt = submitted.value().id;
    UC_CHECK_EQ(submitted.value().phase, AttemptPhase::Acknowledged);
  }
  {
    const Result<UpsRecord> unit = fixture.current();
    UC_REQUIRE_OK(unit);
    TelemetryReport report = uc_test::healthy_report(unit.value().id, unit.value().hardware, Tick{1600},
                                                     SourceRevision{3}, "ev-3");
    report.operating = OperatingState::StaticBypass;
    UC_REQUIRE_STATUS(fixture.observe(report, Tick{1600}), StatusCode::Ok);
  }
  {
    const Result<AttemptRecord> verified = fixture.verify(verified_attempt, Tick{1700});
    UC_REQUIRE_OK(verified);
    UC_CHECK_EQ(verified.value().phase, AttemptPhase::Verified);
    UC_CHECK_EQ(verified.value().verification, VerificationVerdict::Verified);
  }

  // --- a second attempt that is abandoned, leaving a terminal failure ---
  {
    const Result<AttemptRecord> submitted =
        fixture.submit(CommandKind::LeaveStaticBypass, "leave-key", Tick{1800}, "test-authority");
    UC_REQUIRE_OK(submitted);
    UC_CHECK_EQ(submitted.value().phase, AttemptPhase::Acknowledged);
    const Result<AttemptRecord> abandoned =
        fixture.abandon(submitted.value().id, Tick{1900}, "cancelled");
    UC_REQUIRE_OK(abandoned);
    UC_CHECK_EQ(abandoned.value().phase, AttemptPhase::Failed);
  }

  // --- a forward authority move, so the epoch and incarnation are non-trivial ---
  {
    AdoptAuthorityRequest request;
    request.authority = fixture.context;
    request.now = Tick{2000};
    request.epoch = ControlEpoch{2};
    request.incarnation = Incarnation{3};
    UC_REQUIRE_STATUS(fixture.engine->adopt_authority(request), StatusCode::Ok);
  }

  const Result<std::shared_ptr<const UpsState>> committed =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_OK(committed);
  const UpsState before = *committed.value();
  UC_CHECK_MSG(before.units.size() == 1, "one unit must be registered");
  UC_CHECK_MSG(before.units.front().obligations.size() == 2, "two obligation bindings must survive");
  UC_CHECK_MSG(before.units.front().grants.size() == 2, "two grants must survive");
  UC_CHECK_MSG(before.attempts.size() == 2, "two attempts must have been journalled");
  UC_CHECK_MSG(before.idempotency.size() == 2, "two idempotency bindings must have been retained");
  UC_CHECK_MSG(before.commit_log.size() >= 10, "the commit log must describe every mutation");
  UC_CHECK_EQ(before.epoch, ControlEpoch{2});
  UC_CHECK_EQ(before.incarnation, Incarnation{3});

  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  fixture.engine.reset();

  {
    const Result<std::shared_ptr<const UpsState>> reopened =
        UpsStore::read_file(store_path, StoreReadOptions{});
    UC_REQUIRE_OK(reopened);
    expect_state_identical(before, *reopened.value(), "reopened store");
    UC_CHECK_EQ(canonical_state_digest(before), canonical_state_digest(*reopened.value()));
  }

  // The engine's own recovery path must expose exactly the same state.
  {
    EngineOpenOptions options;
    options.store.path = store_path;
    options.store.access = StoreAccess::ReadWrite;
    options.store.create_if_missing = false;
    options.adapter = fixture.adapter;
    const Result<std::shared_ptr<UpsControlEngine>> engine = UpsControlEngine::open(options);
    UC_REQUIRE_OK(engine);
    const Result<std::vector<UpsRecord>> units = engine.value()->units();
    UC_REQUIRE_OK(units);
    UC_CHECK_MSG(units.value().size() == 1, "the engine must recover exactly one unit");
    if (!units.value().empty()) {
      // The durable record keeps the basis that was committed, but the engine may
      // not report a pre-restart basis as current: every unit it recovers is
      // reported as Recovered until this session establishes it again.
      UC_CHECK_EQ(units.value().front().basis, StateBasis::Recovered);
      UpsRecord expected = before.units.front();
      expected.basis = StateBasis::Recovered;
      expect_unit_identical(expected, units.value().front(), "engine recovery");
    }
    const EngineInfo info = engine.value()->info();
    UC_CHECK_EQ(info.generation, before.generation);
    UC_CHECK_EQ(info.epoch, before.epoch);
    UC_CHECK_EQ(info.incarnation, before.incarnation);
    UC_CHECK_EQ(info.attempt_count, before.attempts.size());
    UC_REQUIRE_STATUS(engine.value()->close(), StatusCode::Ok);
  }
}

// ---------------------------------------------------------------------------
// Canonical determinism
// ---------------------------------------------------------------------------

UC_TEST(persistence, canonical_bytes_exclude_store_identity_and_recorded_path) {
  uc_test::ScratchDirectory scratch("persistence-canonical-envelope");
  StoreOpenOptions options;
  options.path = scratch.file("first.upsstore");
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};
  const Result<std::shared_ptr<UpsStore>> first = UpsStore::open(options);
  UC_REQUIRE_OK(first);

  options.path = scratch.file("second.upsstore");
  const Result<std::shared_ptr<UpsStore>> second = UpsStore::open(options);
  UC_REQUIRE_OK(second);

  // The two stores are genuinely different artifacts: different identities and
  // different recorded paths.
  UC_CHECK_NE(first.value()->info().store_identity, second.value()->info().store_identity);
  UC_CHECK_NE(first.value()->info().recorded_path, second.value()->info().recorded_path);
  UC_CHECK_NE(first.value()->info().path, second.value()->info().path);

  const UpsState& left = *first.value()->opened_state();
  const UpsState& right = *second.value()->opened_state();
  const Result<std::vector<std::byte>> left_bytes =
      encode_canonical_state(left, ResourceLimits::defaults());
  const Result<std::vector<std::byte>> right_bytes =
      encode_canonical_state(right, ResourceLimits::defaults());
  UC_REQUIRE_OK(left_bytes);
  UC_REQUIRE_OK(right_bytes);
  UC_CHECK_MSG(left_bytes.value() == right_bytes.value(),
               "the canonical encoding must not depend on the store identity or the recorded path: "
               "left=" + describe_bytes(left_bytes.value()) +
                   " right=" + describe_bytes(right_bytes.value()));
  UC_CHECK_EQ(canonical_state_digest(left), canonical_state_digest(right));

  // The recorded path really does differ and really is inside the payload, so the
  // independence above is a property of the encoding and not of two identical
  // files.
  const std::vector<std::byte> payload = payload_of(options.path);
  const std::uint32_t path_bytes = load_u32(payload, 56);
  const std::string recorded =
      to_text(std::span<const std::byte>(payload.data() + kPayloadHeaderBytes, path_bytes));
  UC_CHECK_EQ(recorded, second.value()->info().recorded_path);

  UC_REQUIRE_STATUS(first.value()->close(), StatusCode::Ok);
  UC_REQUIRE_STATUS(second.value()->close(), StatusCode::Ok);
}

UC_TEST(persistence, canonical_bytes_ignore_unit_insertion_order) {
  ResourceLimits limits = ResourceLimits::defaults();
  UpsState forward;
  forward.generation = StoreGeneration{7};
  forward.epoch = ControlEpoch{1};
  forward.incarnation = Incarnation{1};
  forward.created_at = Tick{1000};
  forward.updated_at = Tick{1500};
  forward.revalidated_at = Tick{1500};
  forward.operation_count = 3;

  UpsState reverse = forward;

  const std::array<const char*, 3> identifiers = {"unit-a", "unit-b", "unit-c"};
  for (const char* identifier : identifiers) {
    UpsRecord unit;
    unit.id = UpsId::parse(identifier).value();
    unit.label = std::string("label of ") + identifier;
    unit.hardware = HardwareGeneration{1};
    unit.lifecycle = LifecycleState::InService;
    unit.operating = OperatingState::OnlineNormal;
    unit.basis = StateBasis::Observed;
    unit.revision = StateRevision{4};
    unit.state_since = Tick{1200};
    unit.registered_at = Tick{1000};
    unit.reserve_policy.max_evidence_age = TickSpan{600};
    UC_REQUIRE_STATUS(insert_sorted_by_id(forward.units, unit, limits), StatusCode::Ok);
  }
  for (std::size_t index = identifiers.size(); index > 0; --index) {
    UpsRecord unit;
    unit.id = UpsId::parse(identifiers[index - 1]).value();
    unit.label = std::string("label of ") + identifiers[index - 1];
    unit.hardware = HardwareGeneration{1};
    unit.lifecycle = LifecycleState::InService;
    unit.operating = OperatingState::OnlineNormal;
    unit.basis = StateBasis::Observed;
    unit.revision = StateRevision{4};
    unit.state_since = Tick{1200};
    unit.registered_at = Tick{1000};
    unit.reserve_policy.max_evidence_age = TickSpan{600};
    UC_REQUIRE_STATUS(insert_sorted_by_id(reverse.units, unit, limits), StatusCode::Ok);
  }

  CommitLogEntry entry;
  entry.generation = StoreGeneration{7};
  entry.operation = OperationKind::UpsRegistered;
  entry.at = Tick{1500};
  forward.commit_log.push_back(entry);
  reverse.commit_log.push_back(entry);

  UC_CHECK_EQ(forward.units.size(), std::size_t{3});

  const Result<std::vector<std::byte>> forward_bytes = encode_canonical_state(forward, limits);
  const Result<std::vector<std::byte>> reverse_bytes = encode_canonical_state(reverse, limits);
  UC_REQUIRE_OK(forward_bytes);
  UC_REQUIRE_OK(reverse_bytes);
  UC_CHECK_MSG(forward_bytes.value() == reverse_bytes.value(),
               "the canonical encoding must not depend on the insertion order of equal units");
  UC_CHECK_EQ(canonical_state_digest(forward), canonical_state_digest(reverse));
}

UC_TEST(persistence, canonical_digest_is_stable_across_repeats_and_copies) {
  const UpsState state = canonical_state_fixture(5);
  const Result<std::vector<std::byte>> first =
      encode_canonical_state(state, ResourceLimits::defaults());
  const Result<std::vector<std::byte>> second =
      encode_canonical_state(state, ResourceLimits::defaults());
  UC_REQUIRE_OK(first);
  UC_REQUIRE_OK(second);
  UC_CHECK_EQ(first.value(), second.value());

  const std::uint64_t digest = canonical_state_digest(state);
  UC_CHECK_EQ(digest, canonical_state_digest(state));
  UC_CHECK_NE(digest, std::uint64_t{0});

  const UpsState copy = state;
  UC_CHECK_EQ(canonical_state_digest(copy), digest);
  const Result<std::vector<std::byte>> copy_bytes =
      encode_canonical_state(copy, ResourceLimits::defaults());
  UC_REQUIRE_OK(copy_bytes);
  UC_CHECK_EQ(copy_bytes.value(), first.value());

  // A round trip through the codec must not move the digest either.
  const Result<UpsState> decoded = detail::decode_state(first.value(), ResourceLimits::defaults());
  UC_REQUIRE_OK(decoded);
  UC_CHECK_EQ(canonical_state_digest(decoded.value()), digest);
  UC_CHECK_EQ(decoded.value(), state);
}

UC_TEST(persistence, canonical_encoding_covers_generation_and_logical_instants) {
  // The documented state section carries the generation and the logical
  // instants, so two states that differ only in those fields must encode to
  // different bytes. The envelope facts (store identity, recorded path) are the
  // ones the canonical encoding excludes.
  const UpsState base = canonical_state_fixture(5);
  const Result<std::vector<std::byte>> base_bytes =
      encode_canonical_state(base, ResourceLimits::defaults());
  UC_REQUIRE_OK(base_bytes);

  UpsState other_generation = base;
  other_generation.generation = StoreGeneration{6};
  other_generation.commit_log.front().generation = StoreGeneration{6};
  const Result<std::vector<std::byte>> generation_bytes =
      encode_canonical_state(other_generation, ResourceLimits::defaults());
  UC_REQUIRE_OK(generation_bytes);
  UC_CHECK_MSG(base_bytes.value() != generation_bytes.value(),
               "the committed generation is a logical field of the canonical state");
  UC_CHECK_NE(canonical_state_digest(base), canonical_state_digest(other_generation));

  UpsState other_instant = base;
  other_instant.created_at = Tick{1001};
  other_instant.updated_at = Tick{1001};
  other_instant.commit_log.front().at = Tick{1001};
  const Result<std::vector<std::byte>> instant_bytes =
      encode_canonical_state(other_instant, ResourceLimits::defaults());
  UC_REQUIRE_OK(instant_bytes);
  UC_CHECK_MSG(base_bytes.value() != instant_bytes.value(),
               "the logical creation instant is a field of the canonical state");
}

// ---------------------------------------------------------------------------
// Documented sizes
// ---------------------------------------------------------------------------

UC_TEST(persistence, head_marker_is_exactly_128_bytes) {
  uc_test::Fixture fixture("persistence-head-size");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  const std::uintmax_t size = std::filesystem::file_size(store_path);
  UC_CHECK_EQ(size, static_cast<std::uintmax_t>(detail::kHeadBytes));
  UC_CHECK_EQ(detail::kHeadBytes, std::size_t{128});
  UC_CHECK_EQ(read_all(store_path).size(), detail::kHeadBytes);
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
}

UC_TEST(persistence, payload_is_exactly_84_plus_path_plus_state_bytes) {
  uc_test::Fixture fixture("persistence-payload-size");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;

  const detail::HeadRecord head = head_of(store_path);
  const std::vector<std::byte> payload = payload_of(store_path);
  const std::uint32_t path_bytes = load_u32(payload, 56);
  const std::uint32_t state_bytes = load_u32(payload, 60);
  const std::uint32_t state_crc = load_u32(payload, 64);

  UC_CHECK_EQ(payload.size(), static_cast<std::size_t>(head.payload_bytes));
  UC_CHECK_EQ(payload.size(), std::size_t{84} + static_cast<std::size_t>(path_bytes) +
                                  static_cast<std::size_t>(state_bytes));
  UC_CHECK_EQ(payload.size(), kPayloadHeaderBytes + static_cast<std::size_t>(path_bytes) +
                                  static_cast<std::size_t>(state_bytes) + kTrailerBytes);
  UC_CHECK_EQ(std::filesystem::file_size(payload_path_for(store_path, head.generation)),
              static_cast<std::uintmax_t>(payload.size()));

  // The declared sections are exactly where the format says they are, and the
  // state section is the canonical encoding of the committed state.
  const std::span<const std::byte> state_span(payload.data() + kPayloadHeaderBytes + path_bytes,
                                              state_bytes);
  UC_CHECK_EQ(state_crc, detail::crc32c(state_span));

  const Result<std::shared_ptr<const UpsState>> state =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_OK(state);
  const Result<std::vector<std::byte>> canonical =
      encode_canonical_state(*state.value(), ResourceLimits::defaults());
  UC_REQUIRE_OK(canonical);
  UC_CHECK_MSG(std::vector<std::byte>(state_span.begin(), state_span.end()) == canonical.value(),
               "the state section must be exactly encode_canonical_state output");
  UC_CHECK_EQ(payload.size(),
              std::size_t{84} + path_to_utf8(store_path).size() + canonical.value().size());

  const std::string recorded =
      to_text(std::span<const std::byte>(payload.data() + kPayloadHeaderBytes, path_bytes));
  UC_CHECK_EQ(recorded, path_to_utf8(std::filesystem::weakly_canonical(store_path)));
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
}

UC_TEST(persistence, refused_reads_leave_the_artifact_untouched) {
  uc_test::Fixture fixture("persistence-refused-read");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  const detail::HeadRecord head = head_of(store_path);
  const std::filesystem::path directory = store_path.parent_path();
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  // Break the checksum bound into the head marker, leaving everything else valid.
  std::vector<std::byte> payload = payload_of(store_path);
  const std::size_t index = payload.size() - kTrailerBytes - 1;
  payload[index] = static_cast<std::byte>(std::to_integer<unsigned char>(payload[index]) ^ 0x01u);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, false);

  const std::vector<std::string> names_before = directory_names(directory);
  const std::vector<std::byte> head_before = read_all(store_path);
  const std::vector<std::byte> payload_before = read_all(payload_path_for(store_path, head.generation));

  for (int attempt = 0; attempt < 3; ++attempt) {
    UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
    UC_REQUIRE_STATUS(UpsStore::verify_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
    const Result<std::shared_ptr<const UpsState>> state =
        UpsStore::read_file(store_path, StoreReadOptions{});
    UC_CHECK(!state.ok());
  }

  // A refused open must not adopt the artifact either, and must leave it whole.
  StoreOpenOptions options;
  options.path = store_path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = false;
  UC_REQUIRE_STATUS(UpsStore::open(options), StatusCode::Corruption);

  UC_CHECK_EQ(directory_names(directory), names_before);
  UC_CHECK_EQ(read_all(store_path), head_before);
  UC_CHECK_EQ(read_all(payload_path_for(store_path, head.generation)), payload_before);
}

// ---------------------------------------------------------------------------
// Envelope rejection
// ---------------------------------------------------------------------------

UC_TEST(persistence, rejects_wrong_head_magic) {
  uc_test::Fixture fixture("persistence-head-magic");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> head = read_all(store_path);
  UC_REQUIRE(head.size() == detail::kHeadBytes);
  head[0] = static_cast<std::byte>('X');
  store_u32(head, 124, detail::crc32c(std::span<const std::byte>(head.data(), head.size() - 4)));
  write_all(store_path, head);

  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
  UC_REQUIRE_STATUS(UpsStore::verify_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
}

UC_TEST(persistence, rejects_wrong_payload_magic) {
  uc_test::Fixture fixture("persistence-payload-magic");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  UC_REQUIRE(payload.size() > kPayloadHeaderBytes + kTrailerBytes);
  payload[0] = static_cast<std::byte>('X');
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);

  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
}

UC_TEST(persistence, rejects_wrong_head_format_version) {
  uc_test::Fixture fixture("persistence-head-version");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> head = read_all(store_path);
  store_u32(head, 8, 2);
  store_u32(head, 120, 2);
  store_u32(head, 124, detail::crc32c(std::span<const std::byte>(head.data(), head.size() - 4)));
  write_all(store_path, head);

  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}),
                    StatusCode::IncompatibleVersion);
}

UC_TEST(persistence, rejects_wrong_byte_order_marker_in_head) {
  uc_test::Fixture fixture("persistence-head-endian");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> head = read_all(store_path);
  store_u32(head, 12, 0x04030201u);
  store_u32(head, 124, detail::crc32c(std::span<const std::byte>(head.data(), head.size() - 4)));
  write_all(store_path, head);

  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::EndianMismatch);
}

UC_TEST(persistence, rejects_head_that_is_not_128_bytes) {
  uc_test::Fixture fixture("persistence-head-truncated");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);
  const std::vector<std::byte> head = read_all(store_path);

  write_all(store_path, std::span<const std::byte>(head.data(), 127));
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
  write_all(store_path, std::span<const std::byte>(head.data(), 1));
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
  write_all(store_path, std::span<const std::byte>());
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
  // A head that is one byte too long is refused for the same reason.
  std::vector<std::byte> oversized = head;
  oversized.push_back(std::byte{0});
  write_all(store_path, oversized);
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
}

UC_TEST(persistence, rejects_head_with_broken_checksum) {
  uc_test::Fixture fixture("persistence-head-crc");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> head = read_all(store_path);
  head[40] = static_cast<std::byte>(std::to_integer<unsigned char>(head[40]) ^ 0x01u);
  write_all(store_path, head);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("integrity check") != std::string::npos,
               "the refusal must name the failed integrity check: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_length_disagreement_with_head) {
  uc_test::Fixture fixture("persistence-payload-length");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  payload.pop_back();
  rewrite_payload(store_path, payload, false);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("partial generation") != std::string::npos ||
                   read.status().message().find("declares") != std::string::npos,
               "the refusal must explain the length disagreement: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_checksum_disagreement_with_head) {
  uc_test::Fixture fixture("persistence-payload-crc");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  // A well formed trailer over altered content: only the checksum bound into the
  // head marker can notice this.
  std::vector<std::byte> payload = payload_of(store_path);
  const std::size_t index = payload.size() - kTrailerBytes - 1;
  payload[index] =
      static_cast<std::byte>(std::to_integer<unsigned char>(payload[index]) ^ 0x01u);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, false);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("head marker") != std::string::npos,
               "the refusal must name the checksum bound into the head marker: " +
                   read.status().message());
}

UC_TEST(persistence, rejects_payload_sections_that_do_not_sum_to_the_file_size) {
  uc_test::Fixture fixture("persistence-payload-sections");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  const std::uint32_t state_bytes = load_u32(payload, 60);
  store_u32(payload, 60, state_bytes + 1);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("declared sections total") != std::string::npos,
               "the refusal must name the section sum: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_trailing_bytes) {
  uc_test::Fixture fixture("persistence-payload-trailing");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  for (int index = 0; index < 4; ++index) {
    payload.push_back(static_cast<std::byte>(0xEE));
  }
  // The head is rebound so that the size check passes and the trailing bytes are
  // the only remaining reason to refuse.
  rewrite_payload(store_path, payload, true);

  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::Corruption);
}

UC_TEST(persistence, rejects_payload_epoch_disagreement_with_head) {
  uc_test::Fixture fixture("persistence-payload-epoch");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  store_u64(payload, 24, load_u64(payload, 24) + 7);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("epoch") != std::string::npos,
               "the refusal must name the epoch: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_incarnation_disagreement_with_head) {
  uc_test::Fixture fixture("persistence-payload-incarnation");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  store_u64(payload, 32, load_u64(payload, 32) + 1);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("incarnation") != std::string::npos,
               "the refusal must name the incarnation: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_identity_disagreement_with_head) {
  uc_test::Fixture fixture("persistence-payload-identity");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  store_u64(payload, 40, load_u64(payload, 40) ^ 0x00000000000000FFull);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("identity") != std::string::npos,
               "the refusal must name the store identity: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_generation_disagreement_with_head) {
  uc_test::Fixture fixture("persistence-payload-generation");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  const detail::HeadRecord head = head_of(store_path);
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  // Rename the payload to the name of a generation the head does not name: the
  // reader must derive the file from the head and never search for the newest
  // one.
  const std::filesystem::path payload_path = payload_path_for(store_path, head.generation);
  const std::filesystem::path renamed =
      payload_path_for(store_path, StoreGeneration{head.generation.value() + 5});
  std::filesystem::rename(payload_path, renamed);
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, StoreReadOptions{}), StatusCode::NotFound);
  std::filesystem::rename(renamed, payload_path);

  // A payload whose own generation field disagrees with the head is corruption.
  std::vector<std::byte> payload = payload_of(store_path);
  store_u64(payload, 16, load_u64(payload, 16) + 1);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);
  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("generation") != std::string::npos,
               "the refusal must name the generation: " + read.status().message());
}

UC_TEST(persistence, rejects_payload_with_non_zero_reserved_field) {
  uc_test::Fixture fixture("persistence-payload-reserved");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  std::vector<std::byte> payload = payload_of(store_path);
  store_u32(payload, 68, 1);
  refresh_trailer(payload);
  rewrite_payload(store_path, payload, true);

  const Result<std::shared_ptr<const UpsState>> read =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("reserved") != std::string::npos,
               "the refusal must name the reserved field: " + read.status().message());
}

// ---------------------------------------------------------------------------
// Structural rejection after a successful checksum
// ---------------------------------------------------------------------------

UC_TEST(persistence, crafted_payload_baseline_is_accepted_and_decoded) {
  uc_test::ScratchDirectory scratch("persistence-crafted-baseline");
  const CraftedState state = valid_crafted_state();
  const std::filesystem::path path = scratch.file("base.upsstore");
  const Result<std::shared_ptr<const UpsState>> read = read_crafted(path, state);
  UC_REQUIRE_OK(read);
  UC_CHECK_EQ(read.value()->generation, StoreGeneration{state.generation});
  UC_CHECK_EQ(read.value()->epoch, ControlEpoch{state.epoch});
  UC_CHECK_EQ(read.value()->incarnation, Incarnation{state.incarnation});
  UC_CHECK_EQ(read.value()->created_at, Tick{state.created_at});
  UC_CHECK_EQ(read.value()->units.size(), std::size_t{1});
  if (!read.value()->units.empty()) {
    UC_CHECK_EQ(read.value()->units.front().id.value(), std::string("unit-a"));
    UC_CHECK_EQ(read.value()->units.front().lifecycle, LifecycleState::InService);
  }
  UC_CHECK_EQ(read.value()->commit_log.size(), std::size_t{1});
  UC_REQUIRE_STATUS(UpsStore::verify_file(path, StoreReadOptions{}), StatusCode::Ok);
}

UC_TEST(persistence, rejects_state_with_out_of_range_enum_byte) {
  uc_test::ScratchDirectory scratch("persistence-structural-enum");
  CraftedState state = valid_crafted_state();
  const Result<std::shared_ptr<const UpsState>> accepted =
      read_crafted(scratch.file("ok.upsstore"), state);
  UC_REQUIRE_OK(accepted);

  state.units.front().lifecycle = 200;
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(scratch.file("bad.upsstore"), state);
  UC_REQUIRE_STATUS(read, StatusCode::Corruption);
  UC_CHECK_MSG(read.status().message().find("unit.lifecycle") != std::string::npos,
               "the refusal must name the field: " + read.status().message());
}

UC_TEST(persistence, rejects_duplicate_unit_identifier_after_checksum) {
  uc_test::ScratchDirectory scratch("persistence-structural-duplicate");
  CraftedState state = valid_crafted_state();
  CraftedUnit second;
  second.id = "unit-b";
  second.label = "unit b";
  state.units.push_back(second);
  const Result<std::shared_ptr<const UpsState>> accepted =
      read_crafted(scratch.file("ok.upsstore"), state);
  UC_REQUIRE_OK(accepted);

  state.units[1].id = state.units[0].id;
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(scratch.file("bad.upsstore"), state);
  UC_REQUIRE_STATUS(read, StatusCode::DuplicateIdentity);
}

UC_TEST(persistence, rejects_attempt_naming_an_unknown_unit) {
  uc_test::ScratchDirectory scratch("persistence-structural-attempt-unit");
  CraftedState state = valid_crafted_state();
  state.attempts.push_back(CraftedAttempt{});
  state.last_attempt = 1;
  const Result<std::shared_ptr<const UpsState>> accepted =
      read_crafted(scratch.file("ok.upsstore"), state);
  UC_REQUIRE_OK(accepted);
  UC_CHECK_EQ(accepted.value()->attempts.size(), std::size_t{1});

  state.attempts.front().ups = "unit-z";
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(scratch.file("bad.upsstore"), state);
  UC_REQUIRE_STATUS(read, StatusCode::InvariantViolation);
  UC_CHECK_MSG(read.status().message().find("does not exist") != std::string::npos,
               "the refusal must name the missing unit: " + read.status().message());
}

UC_TEST(persistence, rejects_in_flight_gate_without_an_unresolved_attempt) {
  uc_test::ScratchDirectory scratch("persistence-structural-gate");
  CraftedState state = valid_crafted_state();
  state.attempts.push_back(CraftedAttempt{});
  state.last_attempt = 1;
  state.units.front().has_in_flight = 1;
  state.units.front().in_flight = 1;
  const Result<std::shared_ptr<const UpsState>> accepted =
      read_crafted(scratch.file("ok.upsstore"), state);
  UC_REQUIRE_OK(accepted);
  UC_CHECK_EQ(accepted.value()->units.front().in_flight, std::optional<AttemptId>(AttemptId{1}));

  // The attempt becomes terminal while the unit still holds the gate for it.
  state.attempts.front().phase = 7;  // Failed
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(scratch.file("bad.upsstore"), state);
  UC_REQUIRE_STATUS(read, StatusCode::InvariantViolation);
  UC_CHECK_MSG(read.status().message().find("in-flight gate") != std::string::npos,
               "the refusal must name the in-flight gate: " + read.status().message());
}

UC_TEST(persistence, rejects_commit_log_out_of_order) {
  uc_test::ScratchDirectory scratch("persistence-structural-commit-order");
  CraftedState state = valid_crafted_state();
  CraftedCommit second;
  second.generation = 2;
  second.operation = 2;  // UpsRegistered
  second.at = 1100;
  state.commit_log.push_back(second);
  state.generation = 2;
  state.updated_at = 1100;
  const Result<std::shared_ptr<const UpsState>> accepted =
      read_crafted(scratch.file("ok.upsstore"), state);
  UC_REQUIRE_OK(accepted);
  UC_CHECK_EQ(accepted.value()->commit_log.size(), std::size_t{2});

  std::swap(state.commit_log[0].generation, state.commit_log[1].generation);
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(scratch.file("bad.upsstore"), state);
  UC_REQUIRE_STATUS(read, StatusCode::InvariantViolation);
  UC_CHECK_MSG(read.status().message().find("commit log") != std::string::npos,
               "the refusal must name the commit log: " + read.status().message());
}

UC_TEST(persistence, rejects_idempotency_binding_naming_a_missing_attempt) {
  uc_test::ScratchDirectory scratch("persistence-structural-idempotency");
  CraftedState state = valid_crafted_state();
  state.attempts.push_back(CraftedAttempt{});
  state.last_attempt = 1;
  CraftedIdempotency entry;
  entry.key = "key-1";
  entry.attempt = 1;
  state.idempotency.push_back(entry);
  const Result<std::shared_ptr<const UpsState>> accepted =
      read_crafted(scratch.file("ok.upsstore"), state);
  UC_REQUIRE_OK(accepted);
  UC_CHECK_EQ(accepted.value()->idempotency.size(), std::size_t{1});

  state.idempotency.front().attempt = 9;
  const Result<std::shared_ptr<const UpsState>> read =
      read_crafted(scratch.file("bad.upsstore"), state);
  UC_REQUIRE_STATUS(read, StatusCode::InvariantViolation);
  UC_CHECK_MSG(read.status().message().find("idempotency") != std::string::npos,
               "the refusal must name the binding: " + read.status().message());
}

// ---------------------------------------------------------------------------
// Read preconditions
// ---------------------------------------------------------------------------

UC_TEST(persistence, read_honours_the_generation_floor) {
  uc_test::Fixture fixture("persistence-generation-floor");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  const StoreGeneration committed = head_of(store_path).generation;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  StoreReadOptions options;
  options.minimum_generation = committed;
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, options), StatusCode::Ok);

  options.minimum_generation = StoreGeneration{committed.value() + 1};
  const Result<std::shared_ptr<const UpsState>> read = UpsStore::read_file(store_path, options);
  UC_REQUIRE_STATUS(read, StatusCode::StaleGeneration);
  UC_CHECK_MSG(read.status().message().find("floor") != std::string::npos,
               "the refusal must explain the floor: " + read.status().message());
  UC_REQUIRE_STATUS(UpsStore::verify_file(store_path, options), StatusCode::StaleGeneration);
}

UC_TEST(persistence, read_honours_the_expected_store_identity) {
  uc_test::Fixture fixture("persistence-identity-floor");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  const StoreIdentity identity = head_of(store_path).identity;
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  StoreReadOptions options;
  options.expected_store_identity = identity;
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, options), StatusCode::Ok);

  const StoreIdentity other = StoreIdentity::generate();
  UC_CHECK_NE(other, identity);
  options.expected_store_identity = other;
  const Result<std::shared_ptr<const UpsState>> read = UpsStore::read_file(store_path, options);
  UC_REQUIRE_STATUS(read, StatusCode::StaleAuthority);
  UC_CHECK_MSG(read.status().message().find("identity") != std::string::npos,
               "the refusal must name the identity: " + read.status().message());
}

UC_TEST(persistence, path_binding_refuses_a_store_read_at_another_path) {
  uc_test::Fixture fixture("persistence-path-binding");
  UC_REQUIRE(fixture.build());
  const std::filesystem::path store_path = fixture.store_options.path;
  const detail::HeadRecord head = head_of(store_path);
  UC_REQUIRE_STATUS(fixture.engine->close(), StatusCode::Ok);

  const std::filesystem::path copy = fixture.scratch->file("copy.upsstore");
  write_all(copy, read_all(store_path));
  write_all(payload_path_for(copy, head.generation), payload_of(store_path));

  StoreReadOptions bound;
  bound.enforce_path_binding = true;
  UC_REQUIRE_STATUS(UpsStore::read_file(store_path, bound), StatusCode::Ok);
  const Result<std::shared_ptr<const UpsState>> at_copy = UpsStore::read_file(copy, bound);
  UC_REQUIRE_STATUS(at_copy, StatusCode::Conflict);
  UC_CHECK_MSG(at_copy.status().message().find("path binding") != std::string::npos,
               "the refusal must name the path binding: " + at_copy.status().message());

  // Without the binding requirement the copy is readable: the recorded path is
  // advisory unless the caller asks for it to be enforced.
  StoreReadOptions unbound;
  UC_REQUIRE_STATUS(UpsStore::read_file(copy, unbound), StatusCode::Ok);
  UC_REQUIRE_STATUS(UpsStore::verify_file(copy, unbound), StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Writer exclusion
// ---------------------------------------------------------------------------

UC_TEST(persistence, writer_exclusion_read_only_observer_and_reopen) {
  uc_test::ScratchDirectory scratch("persistence-writer-exclusion");
  const std::filesystem::path store_path = scratch.file("exclusive.upsstore");

  StoreOpenOptions options;
  options.path = store_path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};

  const Result<std::shared_ptr<UpsStore>> writer = UpsStore::open(options);
  UC_REQUIRE_OK(writer);
  UC_CHECK(writer.value()->writable());

  // A second read-write open must be refused, and the first handle must keep
  // working.
  const Result<std::shared_ptr<UpsStore>> second = UpsStore::open(options);
  UC_REQUIRE_STATUS(second, StatusCode::LockConflict);
  UC_CHECK(writer.value()->is_open());

  const StoreGeneration generation = writer.value()->info().generation;

  // A read-only open takes no lock and sees one whole verified generation.
  StoreOpenOptions read_only = options;
  read_only.access = StoreAccess::ReadOnly;
  read_only.create_if_missing = false;
  const Result<std::shared_ptr<UpsStore>> observer = UpsStore::open(read_only);
  UC_REQUIRE_OK(observer);
  UC_CHECK(observer.value()->opened_state() != nullptr);
  UC_CHECK_EQ(observer.value()->info().generation, generation);
  UC_CHECK_NE(observer.value()->opened_state()->generation, StoreGeneration{0});
  UC_REQUIRE_STATUS(observer.value()->commit(*observer.value()->opened_state()), StatusCode::ReadOnly);
  UC_REQUIRE_STATUS(observer.value()->close(), StatusCode::Ok);

  // The writer still commits after the refused open and the refused read-only
  // commit.
  UpsState next = *writer.value()->opened_state();
  next.generation = StoreGeneration{next.generation.value() + 1};
  next.updated_at = Tick{2000};
  CommitLogEntry entry;
  entry.generation = next.generation;
  entry.operation = OperationKind::Revalidated;
  entry.at = Tick{2000};
  next.commit_log.push_back(entry);
  const Result<CommitResult> committed = writer.value()->commit(next);
  UC_REQUIRE_OK(committed);
  UC_CHECK_EQ(committed.value().generation, next.generation);
  UC_REQUIRE_STATUS(writer.value()->close(), StatusCode::Ok);

  // After the writer released the lock a new read-write open succeeds.
  const Result<std::shared_ptr<UpsStore>> reopened = UpsStore::open(options);
  UC_REQUIRE_OK(reopened);
  UC_CHECK_EQ(reopened.value()->info().generation, next.generation);
  UC_REQUIRE_STATUS(reopened.value()->close(), StatusCode::Ok);
}

// ---------------------------------------------------------------------------
// Repeated open and close
// ---------------------------------------------------------------------------

UC_TEST(persistence, fifty_open_close_cycles_leave_no_residue) {
  uc_test::ScratchDirectory scratch("persistence-open-close");
  const std::filesystem::path store_path = scratch.file("cycle.upsstore");

  StoreOpenOptions options;
  options.path = store_path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick{1000};
  options.epoch = ControlEpoch{1};
  options.incarnation = Incarnation{1};

  const Result<std::shared_ptr<UpsStore>> created = UpsStore::open(options);
  UC_REQUIRE_OK(created);
  const StoreGeneration generation = created.value()->info().generation;
  const std::uint64_t digest = canonical_state_digest(*created.value()->opened_state());
  UC_REQUIRE_STATUS(created.value()->close(), StatusCode::Ok);

  options.create_if_missing = false;
  for (int cycle = 0; cycle < 50; ++cycle) {
    const Result<std::shared_ptr<UpsStore>> store = UpsStore::open(options);
    UC_REQUIRE_OK(store);
    UC_CHECK_EQ(store.value()->info().generation, generation);
    UC_CHECK_EQ(canonical_state_digest(*store.value()->opened_state()), digest);
    UC_REQUIRE_STATUS(store.value()->close(), StatusCode::Ok);
  }

  const std::vector<std::string> names = directory_names(scratch.path());
  const std::string base = path_to_utf8(store_path.filename());
  UC_CHECK_EQ(names.size(), std::size_t{3});
  UC_CHECK_MSG(std::find(names.begin(), names.end(), base) != names.end(),
               "the store head must still be there");
  UC_CHECK_MSG(std::find(names.begin(), names.end(), base + ".lock") != names.end(),
               "the writer lock file must still be there");
  const std::string payload_name = base + ".g" + hex16(generation.value());
  UC_CHECK_MSG(std::find(names.begin(), names.end(), payload_name) != names.end(),
               "exactly one payload file must remain");
  for (const std::string& name : names) {
    UC_CHECK_MSG(name.find(".staging-") == std::string::npos,
                 "no staging residue may remain: " + name);
    UC_CHECK_MSG(name.find(".head-tmp") == std::string::npos,
                 "no staged head may remain: " + name);
    if (name != base && name != base + ".lock") {
      UC_CHECK_MSG(name == payload_name, "unexpected file left behind: " + name);
    }
  }

  UC_REQUIRE_STATUS(UpsStore::verify_file(store_path, StoreReadOptions{}), StatusCode::Ok);
  const Result<std::shared_ptr<const UpsState>> state =
      UpsStore::read_file(store_path, StoreReadOptions{});
  UC_REQUIRE_OK(state);
  UC_CHECK_EQ(canonical_state_digest(*state.value()), digest);
}
