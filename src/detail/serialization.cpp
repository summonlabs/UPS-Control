#include "detail/serialization.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>

#include "detail/codec.hpp"
#include "detail/crc32c.hpp"

namespace ups_control::detail {
namespace {

constexpr std::uint32_t kStateFormatVersion = 1;
constexpr std::uint32_t kByteOrderMarker = 0x01020304u;

constexpr std::array<char, 8> kHeadMagic = {'U', 'P', 'S', 'C', 'H', 'D', '0', '1'};

Status field_error(const char* field, const Status& status) {
  return Status::error(status.code(), std::string(field) + ": " + status.message());
}

Status range_error(const char* field, std::uint64_t value, std::uint64_t lowest,
                   std::uint64_t highest) {
  return Status::error(StatusCode::Corruption,
                       std::string("field '") + field + "' holds " + std::to_string(value) +
                           " which is outside the documented range " + std::to_string(lowest) +
                           ".." + std::to_string(highest));
}

template <typename Enum>
Status write_enum(ByteWriter& writer, Enum value, const char* field, int lowest, int highest) {
  const int numeric = static_cast<int>(value);
  if (numeric < lowest || numeric > highest) {
    return range_error(field, static_cast<std::uint64_t>(numeric),
                       static_cast<std::uint64_t>(lowest), static_cast<std::uint64_t>(highest));
  }
  writer.u8(static_cast<std::uint8_t>(numeric));
  return Status::success();
}

template <typename Enum>
Result<Enum> read_enum(ByteReader& reader, const char* field, int lowest, int highest) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return field_error(field, raw.status());
  }
  const int numeric = static_cast<int>(raw.value());
  if (numeric < lowest || numeric > highest) {
    return range_error(field, raw.value(), static_cast<std::uint64_t>(lowest),
                       static_cast<std::uint64_t>(highest));
  }
  return static_cast<Enum>(numeric);
}

Status write_instant(ByteWriter& writer, Tick value, const char* field) {
  if (value.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(field) + " must not be negative");
  }
  writer.i64(value.value());
  return Status::success();
}

Result<Tick> read_instant(ByteReader& reader, const char* field) {
  const Result<std::int64_t> raw = reader.i64();
  if (!raw.ok()) {
    return field_error(field, raw.status());
  }
  if (raw.value() < 0) {
    return Status::error(StatusCode::Corruption,
                         std::string(field) + " must not be negative");
  }
  return Tick{raw.value()};
}

Status write_text(ByteWriter& writer, const std::string& value, std::size_t max_bytes,
                  const char* field) {
  const Status status = writer.text(value, static_cast<std::uint32_t>(max_bytes));
  if (!status.ok()) {
    return field_error(field, status);
  }
  return Status::success();
}

Result<std::string> read_text(ByteReader& reader, std::size_t max_bytes, const char* field) {
  const Result<std::string> text = reader.text(static_cast<std::uint32_t>(max_bytes));
  if (!text.ok()) {
    return field_error(field, text.status());
  }
  return text.value();
}

Status write_reserve_observation(ByteWriter& writer, const ReserveObservation& observation,
                                 const ResourceLimits& limits, const char* field) {
  Status status = write_enum(writer, observation.state, field, 1, 2);
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, observation.quantity.unit, field, 1, 5);
  if (!status.ok()) {
    return status;
  }
  writer.i64(observation.quantity.value);
  status = write_enum(writer, observation.unknown_reason, field, 0, 7);
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, observation.quality, field, 0, 3);
  if (!status.ok()) {
    return status;
  }
  return validate_reserve_observation(observation, limits);
}

Result<ReserveObservation> read_reserve_observation(ByteReader& reader, const ResourceLimits& limits,
                                                    const char* field) {
  ReserveObservation observation;
  const Result<ReserveState> state = read_enum<ReserveState>(reader, field, 1, 2);
  if (!state.ok()) {
    return state.status();
  }
  observation.state = state.value();
  const Result<ReserveUnit> unit = read_enum<ReserveUnit>(reader, field, 1, 5);
  if (!unit.ok()) {
    return unit.status();
  }
  observation.quantity.unit = unit.value();
  const Result<std::int64_t> value = reader.i64();
  if (!value.ok()) {
    return field_error(field, value.status());
  }
  observation.quantity.value = value.value();
  const Result<UnknownReason> reason = read_enum<UnknownReason>(reader, field, 0, 7);
  if (!reason.ok()) {
    return reason.status();
  }
  observation.unknown_reason = reason.value();
  const Result<EvidenceQuality> quality = read_enum<EvidenceQuality>(reader, field, 0, 3);
  if (!quality.ok()) {
    return quality.status();
  }
  observation.quality = quality.value();
  const Status status = validate_reserve_observation(observation, limits);
  if (!status.ok()) {
    return status;
  }
  return observation;
}

Status write_bool3(ByteWriter& writer, Bool3 value, const char* field) {
  return write_enum(writer, value, field, 0, 2);
}

Result<Bool3> read_bool3(ByteReader& reader, const char* field) {
  return read_enum<Bool3>(reader, field, 0, 2);
}

Status write_transfer(ByteWriter& writer, const TransferStatus& transfer) {
  Status status = write_enum(writer, transfer.bypass_kind, "transfer.bypass_kind", 0, 3);
  if (!status.ok()) {
    return status;
  }
  const Bool3 values[5] = {transfer.bypass_available, transfer.bypass_qualified,
                           transfer.output_synchronized, transfer.transfer_ready,
                           transfer.battery_ready};
  for (const Bool3 value : values) {
    status = write_bool3(writer, value, "transfer.flag");
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

Result<TransferStatus> read_transfer(ByteReader& reader) {
  TransferStatus transfer;
  const Result<BypassKind> kind = read_enum<BypassKind>(reader, "transfer.bypass_kind", 0, 3);
  if (!kind.ok()) {
    return kind.status();
  }
  transfer.bypass_kind = kind.value();
  Bool3* targets[5] = {&transfer.bypass_available, &transfer.bypass_qualified,
                       &transfer.output_synchronized, &transfer.transfer_ready,
                       &transfer.battery_ready};
  for (Bool3* target : targets) {
    const Result<Bool3> value = read_bool3(reader, "transfer.flag");
    if (!value.ok()) {
      return value.status();
    }
    *target = value.value();
  }
  return transfer;
}

Status write_battery(ByteWriter& writer, const BatteryReading& battery, const ResourceLimits& limits) {
  Status status = write_reserve_observation(writer, battery.reserve, limits, "battery.reserve");
  if (!status.ok()) {
    return status;
  }
  status = write_reserve_observation(writer, battery.state_of_charge, limits, "battery.soc");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, battery.activity, "battery.activity", 0, 4);
  if (!status.ok()) {
    return status;
  }
  status = write_bool3(writer, battery.charge_inhibited, "battery.charge_inhibited");
  if (!status.ok()) {
    return status;
  }
  return write_bool3(writer, battery.discharge_inhibited, "battery.discharge_inhibited");
}

Result<BatteryReading> read_battery(ByteReader& reader, const ResourceLimits& limits) {
  BatteryReading battery;
  const Result<ReserveObservation> reserve =
      read_reserve_observation(reader, limits, "battery.reserve");
  if (!reserve.ok()) {
    return reserve.status();
  }
  battery.reserve = reserve.value();
  const Result<ReserveObservation> soc = read_reserve_observation(reader, limits, "battery.soc");
  if (!soc.ok()) {
    return soc.status();
  }
  battery.state_of_charge = soc.value();
  const Result<BatteryActivity> activity = read_enum<BatteryActivity>(reader, "battery.activity", 0, 4);
  if (!activity.ok()) {
    return activity.status();
  }
  battery.activity = activity.value();
  const Result<Bool3> charge = read_bool3(reader, "battery.charge_inhibited");
  if (!charge.ok()) {
    return charge.status();
  }
  battery.charge_inhibited = charge.value();
  const Result<Bool3> discharge = read_bool3(reader, "battery.discharge_inhibited");
  if (!discharge.ok()) {
    return discharge.status();
  }
  battery.discharge_inhibited = discharge.value();
  return battery;
}

Status write_observation(ByteWriter& writer, const ObservationRecord& observation,
                         const ResourceLimits& limits) {
  Status status = write_text(writer, observation.evidence.value(), limits.max_identifier_bytes,
                             "observation.evidence");
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, observation.source.value(), limits.max_identifier_bytes,
                      "observation.source");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, observation.provenance, "observation.provenance", 0, 4);
  if (!status.ok()) {
    return status;
  }
  writer.u64(observation.revision.value());
  status = write_instant(writer, observation.observed_at, "observation.observed_at");
  if (!status.ok()) {
    return status;
  }
  status = write_instant(writer, observation.received_at, "observation.received_at");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, observation.operating, "observation.operating", 0, 12);
  if (!status.ok()) {
    return status;
  }
  status = write_transfer(writer, observation.transfer);
  if (!status.ok()) {
    return status;
  }
  status = write_bool3(writer, observation.capability.recharge_enabled, "observation.recharge");
  if (!status.ok()) {
    return status;
  }
  status = write_bool3(writer, observation.capability.discharge_enabled, "observation.discharge");
  if (!status.ok()) {
    return status;
  }
  status = write_battery(writer, observation.battery, limits);
  if (!status.ok()) {
    return status;
  }
  status = write_bool3(writer, observation.fault_present, "observation.fault_present");
  if (!status.ok()) {
    return status;
  }
  writer.u8(observation.contradictory ? 1u : 0u);
  return write_text(writer, observation.contradiction_detail, limits.max_detail_bytes,
                    "observation.contradiction_detail");
}

Result<ObservationRecord> read_observation(ByteReader& reader, const ResourceLimits& limits) {
  ObservationRecord observation;
  const Result<std::string> evidence =
      read_text(reader, limits.max_identifier_bytes, "observation.evidence");
  if (!evidence.ok()) {
    return evidence.status();
  }
  const Result<EvidenceId> evidence_id = EvidenceId::parse(evidence.value(), limits.max_identifier_bytes);
  if (!evidence_id.ok()) {
    return field_error("observation.evidence", evidence_id.status());
  }
  observation.evidence = evidence_id.value();

  const Result<std::string> source =
      read_text(reader, limits.max_identifier_bytes, "observation.source");
  if (!source.ok()) {
    return source.status();
  }
  const Result<SourceId> source_id = SourceId::parse(source.value(), limits.max_identifier_bytes);
  if (!source_id.ok()) {
    return field_error("observation.source", source_id.status());
  }
  observation.source = source_id.value();

  const Result<Provenance> provenance = read_enum<Provenance>(reader, "observation.provenance", 0, 4);
  if (!provenance.ok()) {
    return provenance.status();
  }
  observation.provenance = provenance.value();

  const Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return field_error("observation.revision", revision.status());
  }
  observation.revision = SourceRevision{revision.value()};

  const Result<Tick> observed = read_instant(reader, "observation.observed_at");
  if (!observed.ok()) {
    return observed.status();
  }
  observation.observed_at = observed.value();
  const Result<Tick> received = read_instant(reader, "observation.received_at");
  if (!received.ok()) {
    return received.status();
  }
  observation.received_at = received.value();

  const Result<OperatingState> operating = read_enum<OperatingState>(reader, "observation.operating", 0, 12);
  if (!operating.ok()) {
    return operating.status();
  }
  observation.operating = operating.value();

  const Result<TransferStatus> transfer = read_transfer(reader);
  if (!transfer.ok()) {
    return transfer.status();
  }
  observation.transfer = transfer.value();

  const Result<Bool3> recharge = read_bool3(reader, "observation.recharge");
  if (!recharge.ok()) {
    return recharge.status();
  }
  observation.capability.recharge_enabled = recharge.value();
  const Result<Bool3> discharge = read_bool3(reader, "observation.discharge");
  if (!discharge.ok()) {
    return discharge.status();
  }
  observation.capability.discharge_enabled = discharge.value();

  const Result<BatteryReading> battery = read_battery(reader, limits);
  if (!battery.ok()) {
    return battery.status();
  }
  observation.battery = battery.value();

  const Result<Bool3> fault = read_bool3(reader, "observation.fault_present");
  if (!fault.ok()) {
    return fault.status();
  }
  observation.fault_present = fault.value();

  const Result<std::uint8_t> contradictory = reader.u8();
  if (!contradictory.ok()) {
    return field_error("observation.contradictory", contradictory.status());
  }
  if (contradictory.value() > 1u) {
    return range_error("observation.contradictory", contradictory.value(), 0, 1);
  }
  observation.contradictory = contradictory.value() == 1u;

  const Result<std::string> detail =
      read_text(reader, limits.max_detail_bytes, "observation.contradiction_detail");
  if (!detail.ok()) {
    return detail.status();
  }
  observation.contradiction_detail = detail.value();
  return observation;
}

Status write_obligation(ByteWriter& writer, const ProtectedLoadObligation& obligation,
                        const ResourceLimits& limits) {
  Status status = write_text(writer, obligation.ref.value(), limits.max_identifier_bytes, "obligation.ref");
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, obligation.load.value(), limits.max_identifier_bytes, "obligation.load");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, obligation.tier, "obligation.tier", 1, 4);
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, obligation.protection, "obligation.protection", 1, 3);
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, obligation.asserted_by.value(), limits.max_identifier_bytes,
                      "obligation.asserted_by");
  if (!status.ok()) {
    return status;
  }
  writer.u64(obligation.epoch.value());
  writer.u64(obligation.incarnation.value());
  status = write_instant(writer, obligation.asserted_at, "obligation.asserted_at");
  if (!status.ok()) {
    return status;
  }
  writer.u8(obligation.expires_at.has_value() ? 1u : 0u);
  writer.i64(obligation.expires_at.has_value() ? obligation.expires_at->value() : 0);
  status = write_enum(writer, obligation.state, "obligation.state", 1, 3);
  if (!status.ok()) {
    return status;
  }
  writer.u8(obligation.released_by.has_value() ? 1u : 0u);
  status = write_text(writer,
                      obligation.released_by.has_value() ? obligation.released_by->value() : std::string(),
                      limits.max_identifier_bytes, "obligation.released_by");
  if (!status.ok()) {
    return status;
  }
  writer.i64(obligation.released_at.has_value() ? obligation.released_at->value() : 0);
  status = write_text(writer, obligation.release_reason, limits.max_detail_bytes,
                      "obligation.release_reason");
  if (!status.ok()) {
    return status;
  }
  writer.u64(obligation.revision.value());
  return Status::success();
}

Result<ProtectedLoadObligation> read_obligation(ByteReader& reader, const ResourceLimits& limits) {
  ProtectedLoadObligation obligation;
  const Result<std::string> ref = read_text(reader, limits.max_identifier_bytes, "obligation.ref");
  if (!ref.ok()) {
    return ref.status();
  }
  const Result<ObligationRef> ref_id = ObligationRef::parse(ref.value(), limits.max_identifier_bytes);
  if (!ref_id.ok()) {
    return field_error("obligation.ref", ref_id.status());
  }
  obligation.ref = ref_id.value();

  const Result<std::string> load = read_text(reader, limits.max_identifier_bytes, "obligation.load");
  if (!load.ok()) {
    return load.status();
  }
  const Result<LoadId> load_id = LoadId::parse(load.value(), limits.max_identifier_bytes);
  if (!load_id.ok()) {
    return field_error("obligation.load", load_id.status());
  }
  obligation.load = load_id.value();

  const Result<ObligationTier> tier = read_enum<ObligationTier>(reader, "obligation.tier", 1, 4);
  if (!tier.ok()) {
    return tier.status();
  }
  obligation.tier = tier.value();
  const Result<ProtectionRequirement> protection =
      read_enum<ProtectionRequirement>(reader, "obligation.protection", 1, 3);
  if (!protection.ok()) {
    return protection.status();
  }
  obligation.protection = protection.value();

  const Result<std::string> asserted = read_text(reader, limits.max_identifier_bytes, "obligation.asserted_by");
  if (!asserted.ok()) {
    return asserted.status();
  }
  const Result<AuthorityRef> asserted_id = AuthorityRef::parse(asserted.value(), limits.max_identifier_bytes);
  if (!asserted_id.ok()) {
    return field_error("obligation.asserted_by", asserted_id.status());
  }
  obligation.asserted_by = asserted_id.value();

  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return field_error("obligation.epoch", epoch.status());
  }
  obligation.epoch = ControlEpoch{epoch.value()};
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return field_error("obligation.incarnation", incarnation.status());
  }
  obligation.incarnation = Incarnation{incarnation.value()};

  const Result<Tick> asserted_at = read_instant(reader, "obligation.asserted_at");
  if (!asserted_at.ok()) {
    return asserted_at.status();
  }
  obligation.asserted_at = asserted_at.value();

  const Result<std::uint8_t> has_expiry = reader.u8();
  if (!has_expiry.ok()) {
    return field_error("obligation.has_expiry", has_expiry.status());
  }
  if (has_expiry.value() > 1u) {
    return range_error("obligation.has_expiry", has_expiry.value(), 0, 1);
  }
  const Result<std::int64_t> expiry = reader.i64();
  if (!expiry.ok()) {
    return field_error("obligation.expires_at", expiry.status());
  }
  if (has_expiry.value() == 1u) {
    if (expiry.value() <= 0) {
      return Status::error(StatusCode::Corruption, "obligation.expires_at must be positive");
    }
    obligation.expires_at = Tick{expiry.value()};
  } else if (expiry.value() != 0) {
    return Status::error(StatusCode::Corruption,
                         "obligation.expires_at must be zero when no expiry is declared");
  }

  const Result<ObligationState> state = read_enum<ObligationState>(reader, "obligation.state", 1, 3);
  if (!state.ok()) {
    return state.status();
  }
  obligation.state = state.value();

  const Result<std::uint8_t> has_released_by = reader.u8();
  if (!has_released_by.ok()) {
    return field_error("obligation.has_released_by", has_released_by.status());
  }
  if (has_released_by.value() > 1u) {
    return range_error("obligation.has_released_by", has_released_by.value(), 0, 1);
  }
  const Result<std::string> released_by =
      read_text(reader, limits.max_identifier_bytes, "obligation.released_by");
  if (!released_by.ok()) {
    return released_by.status();
  }
  if (has_released_by.value() == 1u) {
    const Result<AuthorityRef> released_id =
        AuthorityRef::parse(released_by.value(), limits.max_identifier_bytes);
    if (!released_id.ok()) {
      return field_error("obligation.released_by", released_id.status());
    }
    obligation.released_by = released_id.value();
  } else if (!released_by.value().empty()) {
    return Status::error(StatusCode::Corruption,
                         "obligation.released_by must be empty when it is not present");
  }

  const Result<std::int64_t> released_at = reader.i64();
  if (!released_at.ok()) {
    return field_error("obligation.released_at", released_at.status());
  }
  if (released_at.value() < 0) {
    return Status::error(StatusCode::Corruption, "obligation.released_at must not be negative");
  }
  if (released_at.value() > 0) {
    obligation.released_at = Tick{released_at.value()};
  }

  const Result<std::string> reason =
      read_text(reader, limits.max_detail_bytes, "obligation.release_reason");
  if (!reason.ok()) {
    return reason.status();
  }
  obligation.release_reason = reason.value();

  const Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return field_error("obligation.revision", revision.status());
  }
  obligation.revision = ObligationRevision{revision.value()};

  const Status status = validate_obligation(obligation, limits);
  if (!status.ok()) {
    return status;
  }
  return obligation;
}

Status write_grant(ByteWriter& writer, const AuthorityGrant& grant, const ResourceLimits& limits) {
  Status status = write_text(writer, grant.ref.value(), limits.max_identifier_bytes, "grant.ref");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, grant.scope, "grant.scope", 1, 8);
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, grant.granted_by.value(), limits.max_identifier_bytes, "grant.granted_by");
  if (!status.ok()) {
    return status;
  }
  writer.u64(grant.epoch.value());
  writer.u64(grant.incarnation.value());
  status = write_instant(writer, grant.issued_at, "grant.issued_at");
  if (!status.ok()) {
    return status;
  }
  writer.u8(grant.expires_at.has_value() ? 1u : 0u);
  writer.i64(grant.expires_at.has_value() ? grant.expires_at->value() : 0);
  writer.u8(grant.allowed ? 1u : 0u);
  writer.u8(grant.reserve_floor.has_value() ? 1u : 0u);
  writer.u8(grant.reserve_floor.has_value() ? static_cast<std::uint8_t>(grant.reserve_floor->unit) : 0u);
  writer.i64(grant.reserve_floor.has_value() ? grant.reserve_floor->value : 0);
  writer.u8(grant.revoked_by.has_value() ? 1u : 0u);
  status = write_text(writer, grant.revoked_by.has_value() ? grant.revoked_by->value() : std::string(),
                      limits.max_identifier_bytes, "grant.revoked_by");
  if (!status.ok()) {
    return status;
  }
  writer.i64(grant.revoked_at.has_value() ? grant.revoked_at->value() : 0);
  status = write_text(writer, grant.note, limits.max_detail_bytes, "grant.note");
  if (!status.ok()) {
    return status;
  }
  writer.u64(grant.revision.value());
  return Status::success();
}

Result<AuthorityGrant> read_grant(ByteReader& reader, const ResourceLimits& limits) {
  AuthorityGrant grant;
  const Result<std::string> ref = read_text(reader, limits.max_identifier_bytes, "grant.ref");
  if (!ref.ok()) {
    return ref.status();
  }
  const Result<AuthorityRef> ref_id = AuthorityRef::parse(ref.value(), limits.max_identifier_bytes);
  if (!ref_id.ok()) {
    return field_error("grant.ref", ref_id.status());
  }
  grant.ref = ref_id.value();

  const Result<GrantScope> scope = read_enum<GrantScope>(reader, "grant.scope", 1, 8);
  if (!scope.ok()) {
    return scope.status();
  }
  grant.scope = scope.value();

  const Result<std::string> granted_by = read_text(reader, limits.max_identifier_bytes, "grant.granted_by");
  if (!granted_by.ok()) {
    return granted_by.status();
  }
  const Result<AuthorityRef> granted_by_id =
      AuthorityRef::parse(granted_by.value(), limits.max_identifier_bytes);
  if (!granted_by_id.ok()) {
    return field_error("grant.granted_by", granted_by_id.status());
  }
  grant.granted_by = granted_by_id.value();

  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return field_error("grant.epoch", epoch.status());
  }
  grant.epoch = ControlEpoch{epoch.value()};
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return field_error("grant.incarnation", incarnation.status());
  }
  grant.incarnation = Incarnation{incarnation.value()};

  const Result<Tick> issued = read_instant(reader, "grant.issued_at");
  if (!issued.ok()) {
    return issued.status();
  }
  grant.issued_at = issued.value();

  const Result<std::uint8_t> has_expiry = reader.u8();
  if (!has_expiry.ok()) {
    return field_error("grant.has_expiry", has_expiry.status());
  }
  if (has_expiry.value() > 1u) {
    return range_error("grant.has_expiry", has_expiry.value(), 0, 1);
  }
  const Result<std::int64_t> expiry = reader.i64();
  if (!expiry.ok()) {
    return field_error("grant.expires_at", expiry.status());
  }
  if (has_expiry.value() == 1u) {
    if (expiry.value() <= 0) {
      return Status::error(StatusCode::Corruption, "grant.expires_at must be positive");
    }
    grant.expires_at = Tick{expiry.value()};
  } else if (expiry.value() != 0) {
    return Status::error(StatusCode::Corruption,
                         "grant.expires_at must be zero when no expiry is declared");
  }

  const Result<std::uint8_t> allowed = reader.u8();
  if (!allowed.ok()) {
    return field_error("grant.allowed", allowed.status());
  }
  if (allowed.value() > 1u) {
    return range_error("grant.allowed", allowed.value(), 0, 1);
  }
  grant.allowed = allowed.value() == 1u;

  const Result<std::uint8_t> has_floor = reader.u8();
  if (!has_floor.ok()) {
    return field_error("grant.has_reserve_floor", has_floor.status());
  }
  if (has_floor.value() > 1u) {
    return range_error("grant.has_reserve_floor", has_floor.value(), 0, 1);
  }
  const Result<std::uint8_t> floor_unit = reader.u8();
  if (!floor_unit.ok()) {
    return field_error("grant.reserve_floor.unit", floor_unit.status());
  }
  const Result<std::int64_t> floor_value = reader.i64();
  if (!floor_value.ok()) {
    return field_error("grant.reserve_floor.value", floor_value.status());
  }
  if (has_floor.value() == 1u) {
    if (floor_unit.value() < 1u || floor_unit.value() > 5u) {
      return range_error("grant.reserve_floor.unit", floor_unit.value(), 1, 5);
    }
    const Result<ReserveQuantity> quantity =
        make_reserve_quantity(static_cast<ReserveUnit>(floor_unit.value()), floor_value.value(), limits);
    if (!quantity.ok()) {
      return quantity.status();
    }
    grant.reserve_floor = quantity.value();
  } else if (floor_unit.value() != 0u || floor_value.value() != 0) {
    return Status::error(StatusCode::Corruption,
                         "grant reserve floor must be absent when it is not declared");
  }

  const Result<std::uint8_t> has_revoked_by = reader.u8();
  if (!has_revoked_by.ok()) {
    return field_error("grant.has_revoked_by", has_revoked_by.status());
  }
  if (has_revoked_by.value() > 1u) {
    return range_error("grant.has_revoked_by", has_revoked_by.value(), 0, 1);
  }
  const Result<std::string> revoked_by = read_text(reader, limits.max_identifier_bytes, "grant.revoked_by");
  if (!revoked_by.ok()) {
    return revoked_by.status();
  }
  if (has_revoked_by.value() == 1u) {
    const Result<AuthorityRef> revoked_id =
        AuthorityRef::parse(revoked_by.value(), limits.max_identifier_bytes);
    if (!revoked_id.ok()) {
      return field_error("grant.revoked_by", revoked_id.status());
    }
    grant.revoked_by = revoked_id.value();
  } else if (!revoked_by.value().empty()) {
    return Status::error(StatusCode::Corruption, "grant.revoked_by must be empty when it is not present");
  }

  const Result<std::int64_t> revoked_at = reader.i64();
  if (!revoked_at.ok()) {
    return field_error("grant.revoked_at", revoked_at.status());
  }
  if (revoked_at.value() < 0) {
    return Status::error(StatusCode::Corruption, "grant.revoked_at must not be negative");
  }
  if (revoked_at.value() > 0) {
    grant.revoked_at = Tick{revoked_at.value()};
  }

  const Result<std::string> note = read_text(reader, limits.max_detail_bytes, "grant.note");
  if (!note.ok()) {
    return note.status();
  }
  grant.note = note.value();

  const Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return field_error("grant.revision", revision.status());
  }
  grant.revision = GrantRevision{revision.value()};

  const Status status = validate_grant(grant, limits);
  if (!status.ok()) {
    return status;
  }
  return grant;
}

Status write_command_parameters(ByteWriter& writer, const CommandParameters& parameters,
                                const ResourceLimits& limits) {
  writer.u8(parameters.asserted_target.has_value() ? 1u : 0u);
  writer.u8(parameters.asserted_target.has_value()
                ? static_cast<std::uint8_t>(parameters.asserted_target.value())
                : 0u);
  writer.u8(parameters.reserve_requirement.has_value() ? 1u : 0u);
  writer.u8(parameters.reserve_requirement.has_value()
                ? static_cast<std::uint8_t>(parameters.reserve_requirement->minimum.unit)
                : 0u);
  writer.i64(parameters.reserve_requirement.has_value() ? parameters.reserve_requirement->minimum.value
                                                        : 0);
  writer.i64(parameters.verification_dwell.value());
  return validate_command_parameters(parameters, limits);
}

Result<CommandParameters> read_command_parameters(ByteReader& reader, const ResourceLimits& limits) {
  CommandParameters parameters;
  const Result<std::uint8_t> has_target = reader.u8();
  if (!has_target.ok()) {
    return field_error("attempt.parameters.has_asserted_target", has_target.status());
  }
  if (has_target.value() > 1u) {
    return range_error("attempt.parameters.has_asserted_target", has_target.value(), 0, 1);
  }
  const Result<std::uint8_t> target = reader.u8();
  if (!target.ok()) {
    return field_error("attempt.parameters.asserted_target", target.status());
  }
  if (has_target.value() == 1u) {
    if (target.value() < 1u || target.value() > 12u) {
      return range_error("attempt.parameters.asserted_target", target.value(), 1, 12);
    }
    parameters.asserted_target = static_cast<OperatingState>(target.value());
  } else if (target.value() != 0u) {
    return Status::error(StatusCode::Corruption,
                         "attempt.parameters.asserted_target must be absent when it is not declared");
  }

  const Result<std::uint8_t> has_requirement = reader.u8();
  if (!has_requirement.ok()) {
    return field_error("attempt.parameters.has_reserve_requirement", has_requirement.status());
  }
  if (has_requirement.value() > 1u) {
    return range_error("attempt.parameters.has_reserve_requirement", has_requirement.value(), 0, 1);
  }
  const Result<std::uint8_t> unit = reader.u8();
  if (!unit.ok()) {
    return field_error("attempt.parameters.reserve_unit", unit.status());
  }
  const Result<std::int64_t> value = reader.i64();
  if (!value.ok()) {
    return field_error("attempt.parameters.reserve_value", value.status());
  }
  if (has_requirement.value() == 1u) {
    if (unit.value() < 1u || unit.value() > 5u) {
      return range_error("attempt.parameters.reserve_unit", unit.value(), 1, 5);
    }
    const Result<ReserveQuantity> quantity =
        make_reserve_quantity(static_cast<ReserveUnit>(unit.value()), value.value(), limits);
    if (!quantity.ok()) {
      return quantity.status();
    }
    ReserveRequirement requirement;
    requirement.minimum = quantity.value();
    parameters.reserve_requirement = requirement;
  } else if (unit.value() != 0u || value.value() != 0) {
    return Status::error(StatusCode::Corruption,
                         "attempt reserve requirement must be absent when it is not declared");
  }

  const Result<std::int64_t> dwell = reader.i64();
  if (!dwell.ok()) {
    return field_error("attempt.parameters.verification_dwell", dwell.status());
  }
  parameters.verification_dwell = TickSpan{dwell.value()};
  const Status status = validate_command_parameters(parameters, limits);
  if (!status.ok()) {
    return status;
  }
  return parameters;
}

Status write_attempt(ByteWriter& writer, const AttemptRecord& attempt, const ResourceLimits& limits) {
  writer.u64(attempt.id.value());
  Status status = write_text(writer, attempt.key.value(), limits.max_idempotency_key_bytes, "attempt.key");
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, attempt.ups.value(), limits.max_identifier_bytes, "attempt.ups");
  if (!status.ok()) {
    return status;
  }
  writer.u32(attempt.hardware.value());
  writer.u64(attempt.epoch.value());
  writer.u64(attempt.incarnation.value());
  writer.u64(attempt.planned_revision.value());
  status = write_enum(writer, attempt.command, "attempt.command", 1, 14);
  if (!status.ok()) {
    return status;
  }
  status = write_command_parameters(writer, attempt.parameters, limits);
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, attempt.target, "attempt.target", 0, 12);
  if (!status.ok()) {
    return status;
  }
  status = write_instant(writer, attempt.submitted_at, "attempt.submitted_at");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, attempt.phase, "attempt.phase", 1, 8);
  if (!status.ok()) {
    return status;
  }
  writer.u64(attempt.plan_digest);

  writer.u8(attempt.refusal.has_value() ? 1u : 0u);
  status = write_enum(writer, attempt.refusal.has_value() ? attempt.refusal->code : RefusalCode::None,
                      "attempt.refusal.code", 0, 36);
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, attempt.refusal.has_value() ? attempt.refusal->detail : std::string(),
                      limits.max_detail_bytes, "attempt.refusal.detail");
  if (!status.ok()) {
    return status;
  }

  status = write_enum(writer, attempt.ack, "attempt.ack", 0, 5);
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, attempt.ack_detail, limits.max_detail_bytes, "attempt.ack_detail");
  if (!status.ok()) {
    return status;
  }
  writer.i64(attempt.acknowledged_at.value());
  status = write_enum(writer, attempt.observed, "attempt.observed", 0, 4);
  if (!status.ok()) {
    return status;
  }
  writer.i64(attempt.observed_at.value());
  status = write_enum(writer, attempt.observed_state, "attempt.observed_state", 0, 12);
  if (!status.ok()) {
    return status;
  }
  status = write_bool3(writer, attempt.observed_capability.recharge_enabled, "attempt.observed_recharge");
  if (!status.ok()) {
    return status;
  }
  status = write_bool3(writer, attempt.observed_capability.discharge_enabled, "attempt.observed_discharge");
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, attempt.verification, "attempt.verification", 0, 4);
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, attempt.verification_detail, limits.max_detail_bytes,
                      "attempt.verification_detail");
  if (!status.ok()) {
    return status;
  }
  writer.i64(attempt.verified_at.value());
  writer.u8(attempt.verification_evidence.has_value() ? 1u : 0u);
  status = write_text(writer,
                      attempt.verification_evidence.has_value() ? attempt.verification_evidence->value()
                                                                : std::string(),
                      limits.max_identifier_bytes, "attempt.verification_evidence");
  if (!status.ok()) {
    return status;
  }
  writer.u32(attempt.verification_rounds);
  status = write_enum(writer, attempt.evidence, "attempt.evidence", 0, 2);
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, attempt.adapter.value(), limits.max_identifier_bytes, "attempt.adapter");
  if (!status.ok()) {
    return status;
  }
  return write_text(writer, attempt.terminal_detail, limits.max_detail_bytes, "attempt.terminal_detail");
}

Result<AttemptRecord> read_attempt(ByteReader& reader, const ResourceLimits& limits) {
  AttemptRecord attempt;
  const Result<std::uint64_t> id = reader.u64();
  if (!id.ok()) {
    return field_error("attempt.id", id.status());
  }
  attempt.id = AttemptId{id.value()};

  const Result<std::string> key = read_text(reader, limits.max_idempotency_key_bytes, "attempt.key");
  if (!key.ok()) {
    return key.status();
  }
  const Result<IdempotencyKey> key_id = IdempotencyKey::parse(key.value(), limits.max_idempotency_key_bytes);
  if (!key_id.ok()) {
    return field_error("attempt.key", key_id.status());
  }
  attempt.key = key_id.value();

  const Result<std::string> ups = read_text(reader, limits.max_identifier_bytes, "attempt.ups");
  if (!ups.ok()) {
    return ups.status();
  }
  const Result<UpsId> ups_id = UpsId::parse(ups.value(), limits.max_identifier_bytes);
  if (!ups_id.ok()) {
    return field_error("attempt.ups", ups_id.status());
  }
  attempt.ups = ups_id.value();

  const Result<std::uint32_t> hardware = reader.u32();
  if (!hardware.ok()) {
    return field_error("attempt.hardware", hardware.status());
  }
  attempt.hardware = HardwareGeneration{hardware.value()};
  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return field_error("attempt.epoch", epoch.status());
  }
  attempt.epoch = ControlEpoch{epoch.value()};
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return field_error("attempt.incarnation", incarnation.status());
  }
  attempt.incarnation = Incarnation{incarnation.value()};
  const Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return field_error("attempt.planned_revision", revision.status());
  }
  attempt.planned_revision = StateRevision{revision.value()};

  const Result<CommandKind> command = read_enum<CommandKind>(reader, "attempt.command", 1, 14);
  if (!command.ok()) {
    return command.status();
  }
  attempt.command = command.value();

  const Result<CommandParameters> parameters = read_command_parameters(reader, limits);
  if (!parameters.ok()) {
    return parameters.status();
  }
  attempt.parameters = parameters.value();

  const Result<OperatingState> target = read_enum<OperatingState>(reader, "attempt.target", 0, 12);
  if (!target.ok()) {
    return target.status();
  }
  attempt.target = target.value();

  const Result<Tick> submitted = read_instant(reader, "attempt.submitted_at");
  if (!submitted.ok()) {
    return submitted.status();
  }
  attempt.submitted_at = submitted.value();

  const Result<AttemptPhase> phase = read_enum<AttemptPhase>(reader, "attempt.phase", 1, 8);
  if (!phase.ok()) {
    return phase.status();
  }
  attempt.phase = phase.value();

  const Result<std::uint64_t> digest = reader.u64();
  if (!digest.ok()) {
    return field_error("attempt.plan_digest", digest.status());
  }
  attempt.plan_digest = digest.value();

  const Result<std::uint8_t> has_refusal = reader.u8();
  if (!has_refusal.ok()) {
    return field_error("attempt.has_refusal", has_refusal.status());
  }
  if (has_refusal.value() > 1u) {
    return range_error("attempt.has_refusal", has_refusal.value(), 0, 1);
  }
  const Result<RefusalCode> refusal = read_enum<RefusalCode>(reader, "attempt.refusal.code", 0, 36);
  if (!refusal.ok()) {
    return refusal.status();
  }
  const Result<std::string> refusal_detail =
      read_text(reader, limits.max_detail_bytes, "attempt.refusal.detail");
  if (!refusal_detail.ok()) {
    return refusal_detail.status();
  }
  if (has_refusal.value() == 1u) {
    RefusalRecord record;
    record.code = refusal.value();
    record.detail = refusal_detail.value();
    attempt.refusal = record;
  } else if (refusal.value() != RefusalCode::None || !refusal_detail.value().empty()) {
    return Status::error(StatusCode::Corruption,
                         "attempt refusal must be absent when it is not recorded");
  }

  const Result<AckOutcome> ack = read_enum<AckOutcome>(reader, "attempt.ack", 0, 5);
  if (!ack.ok()) {
    return ack.status();
  }
  attempt.ack = ack.value();
  const Result<std::string> ack_detail = read_text(reader, limits.max_detail_bytes, "attempt.ack_detail");
  if (!ack_detail.ok()) {
    return ack_detail.status();
  }
  attempt.ack_detail = ack_detail.value();

  const Result<std::int64_t> acknowledged_at = reader.i64();
  if (!acknowledged_at.ok()) {
    return field_error("attempt.acknowledged_at", acknowledged_at.status());
  }
  if (acknowledged_at.value() < 0) {
    return Status::error(StatusCode::Corruption, "attempt.acknowledged_at must not be negative");
  }
  attempt.acknowledged_at = Tick{acknowledged_at.value()};

  const Result<ObservedOutcome> observed = read_enum<ObservedOutcome>(reader, "attempt.observed", 0, 4);
  if (!observed.ok()) {
    return observed.status();
  }
  attempt.observed = observed.value();
  const Result<std::int64_t> observed_at = reader.i64();
  if (!observed_at.ok()) {
    return field_error("attempt.observed_at", observed_at.status());
  }
  if (observed_at.value() < 0) {
    return Status::error(StatusCode::Corruption, "attempt.observed_at must not be negative");
  }
  attempt.observed_at = Tick{observed_at.value()};
  const Result<OperatingState> observed_state =
      read_enum<OperatingState>(reader, "attempt.observed_state", 0, 12);
  if (!observed_state.ok()) {
    return observed_state.status();
  }
  attempt.observed_state = observed_state.value();
  const Result<Bool3> observed_recharge = read_bool3(reader, "attempt.observed_recharge");
  if (!observed_recharge.ok()) {
    return observed_recharge.status();
  }
  attempt.observed_capability.recharge_enabled = observed_recharge.value();
  const Result<Bool3> observed_discharge = read_bool3(reader, "attempt.observed_discharge");
  if (!observed_discharge.ok()) {
    return observed_discharge.status();
  }
  attempt.observed_capability.discharge_enabled = observed_discharge.value();

  const Result<VerificationVerdict> verification =
      read_enum<VerificationVerdict>(reader, "attempt.verification", 0, 4);
  if (!verification.ok()) {
    return verification.status();
  }
  attempt.verification = verification.value();
  const Result<std::string> verification_detail =
      read_text(reader, limits.max_detail_bytes, "attempt.verification_detail");
  if (!verification_detail.ok()) {
    return verification_detail.status();
  }
  attempt.verification_detail = verification_detail.value();

  const Result<std::int64_t> verified_at = reader.i64();
  if (!verified_at.ok()) {
    return field_error("attempt.verified_at", verified_at.status());
  }
  if (verified_at.value() < 0) {
    return Status::error(StatusCode::Corruption, "attempt.verified_at must not be negative");
  }
  attempt.verified_at = Tick{verified_at.value()};

  const Result<std::uint8_t> has_evidence = reader.u8();
  if (!has_evidence.ok()) {
    return field_error("attempt.has_verification_evidence", has_evidence.status());
  }
  if (has_evidence.value() > 1u) {
    return range_error("attempt.has_verification_evidence", has_evidence.value(), 0, 1);
  }
  const Result<std::string> evidence =
      read_text(reader, limits.max_identifier_bytes, "attempt.verification_evidence");
  if (!evidence.ok()) {
    return evidence.status();
  }
  if (has_evidence.value() == 1u) {
    const Result<EvidenceId> evidence_id =
        EvidenceId::parse(evidence.value(), limits.max_identifier_bytes);
    if (!evidence_id.ok()) {
      return field_error("attempt.verification_evidence", evidence_id.status());
    }
    attempt.verification_evidence = evidence_id.value();
  } else if (!evidence.value().empty()) {
    return Status::error(StatusCode::Corruption,
                         "attempt verification evidence must be empty when it is not recorded");
  }

  const Result<std::uint32_t> rounds = reader.u32();
  if (!rounds.ok()) {
    return field_error("attempt.verification_rounds", rounds.status());
  }
  attempt.verification_rounds = rounds.value();

  const Result<EvidenceClass> evidence_class = read_enum<EvidenceClass>(reader, "attempt.evidence", 0, 2);
  if (!evidence_class.ok()) {
    return evidence_class.status();
  }
  attempt.evidence = evidence_class.value();

  const Result<std::string> adapter = read_text(reader, limits.max_identifier_bytes, "attempt.adapter");
  if (!adapter.ok()) {
    return adapter.status();
  }
  if (!adapter.value().empty()) {
    const Result<AdapterId> adapter_id = AdapterId::parse(adapter.value(), limits.max_identifier_bytes);
    if (!adapter_id.ok()) {
      return field_error("attempt.adapter", adapter_id.status());
    }
    attempt.adapter = adapter_id.value();
  }
  const Result<std::string> terminal = read_text(reader, limits.max_detail_bytes, "attempt.terminal_detail");
  if (!terminal.ok()) {
    return terminal.status();
  }
  attempt.terminal_detail = terminal.value();
  return attempt;
}

Status write_unit(ByteWriter& writer, const UpsRecord& unit, const ResourceLimits& limits) {
  Status status = write_text(writer, unit.id.value(), limits.max_identifier_bytes, "unit.id");
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, unit.label, limits.max_label_bytes, "unit.label");
  if (!status.ok()) {
    return status;
  }
  writer.u32(unit.hardware.value());
  status = write_enum(writer, unit.lifecycle, "unit.lifecycle", 1, 8);
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, unit.operating, "unit.operating", 0, 12);
  if (!status.ok()) {
    return status;
  }
  status = write_enum(writer, unit.basis, "unit.basis", 1, 4);
  if (!status.ok()) {
    return status;
  }
  writer.u64(unit.revision.value());
  status = write_instant(writer, unit.state_since, "unit.state_since");
  if (!status.ok()) {
    return status;
  }
  status = write_instant(writer, unit.registered_at, "unit.registered_at");
  if (!status.ok()) {
    return status;
  }
  status = write_text(writer, unit.registration_note, limits.max_detail_bytes, "unit.note");
  if (!status.ok()) {
    return status;
  }

  writer.u8(unit.observation.has_value() ? 1u : 0u);
  if (unit.observation.has_value()) {
    status = write_observation(writer, unit.observation.value(), limits);
    if (!status.ok()) {
      return status;
    }
  }

  writer.u8(unit.reserve_policy.discharge_floor.has_value() ? 1u : 0u);
  writer.u8(unit.reserve_policy.discharge_floor.has_value()
                ? static_cast<std::uint8_t>(unit.reserve_policy.discharge_floor->unit)
                : 0u);
  writer.i64(unit.reserve_policy.discharge_floor.has_value()
                 ? unit.reserve_policy.discharge_floor->value
                 : 0);
  writer.i64(unit.reserve_policy.max_evidence_age.value());
  writer.i64(unit.reserve_policy.max_future_skew.value());
  status = write_enum(writer, unit.reserve_policy.minimum_quality, "unit.policy.minimum_quality", 0, 3);
  if (!status.ok()) {
    return status;
  }

  writer.u32(static_cast<std::uint32_t>(unit.obligations.size()));
  for (const ProtectedLoadObligation& obligation : unit.obligations) {
    status = write_obligation(writer, obligation, limits);
    if (!status.ok()) {
      return status;
    }
  }
  writer.u32(static_cast<std::uint32_t>(unit.grants.size()));
  for (const AuthorityGrant& grant : unit.grants) {
    status = write_grant(writer, grant, limits);
    if (!status.ok()) {
      return status;
    }
  }
  writer.u8(unit.in_flight.has_value() ? 1u : 0u);
  writer.u64(unit.in_flight.has_value() ? unit.in_flight->value() : 0u);
  writer.i64(unit.last_verified_at.value());
  return Status::success();
}

Result<UpsRecord> read_unit(ByteReader& reader, const ResourceLimits& limits) {
  UpsRecord unit;
  const Result<std::string> id = read_text(reader, limits.max_identifier_bytes, "unit.id");
  if (!id.ok()) {
    return id.status();
  }
  const Result<UpsId> ups_id = UpsId::parse(id.value(), limits.max_identifier_bytes);
  if (!ups_id.ok()) {
    return field_error("unit.id", ups_id.status());
  }
  unit.id = ups_id.value();

  const Result<std::string> label = read_text(reader, limits.max_label_bytes, "unit.label");
  if (!label.ok()) {
    return label.status();
  }
  unit.label = label.value();

  const Result<std::uint32_t> hardware = reader.u32();
  if (!hardware.ok()) {
    return field_error("unit.hardware", hardware.status());
  }
  unit.hardware = HardwareGeneration{hardware.value()};

  const Result<LifecycleState> lifecycle = read_enum<LifecycleState>(reader, "unit.lifecycle", 1, 8);
  if (!lifecycle.ok()) {
    return lifecycle.status();
  }
  unit.lifecycle = lifecycle.value();
  const Result<OperatingState> operating = read_enum<OperatingState>(reader, "unit.operating", 0, 12);
  if (!operating.ok()) {
    return operating.status();
  }
  unit.operating = operating.value();
  const Result<StateBasis> basis = read_enum<StateBasis>(reader, "unit.basis", 1, 4);
  if (!basis.ok()) {
    return basis.status();
  }
  unit.basis = basis.value();

  const Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return field_error("unit.revision", revision.status());
  }
  unit.revision = StateRevision{revision.value()};

  const Result<Tick> state_since = read_instant(reader, "unit.state_since");
  if (!state_since.ok()) {
    return state_since.status();
  }
  unit.state_since = state_since.value();
  const Result<Tick> registered = read_instant(reader, "unit.registered_at");
  if (!registered.ok()) {
    return registered.status();
  }
  unit.registered_at = registered.value();

  const Result<std::string> note = read_text(reader, limits.max_detail_bytes, "unit.note");
  if (!note.ok()) {
    return note.status();
  }
  unit.registration_note = note.value();

  const Result<std::uint8_t> has_observation = reader.u8();
  if (!has_observation.ok()) {
    return field_error("unit.has_observation", has_observation.status());
  }
  if (has_observation.value() > 1u) {
    return range_error("unit.has_observation", has_observation.value(), 0, 1);
  }
  if (has_observation.value() == 1u) {
    const Result<ObservationRecord> observation = read_observation(reader, limits);
    if (!observation.ok()) {
      return observation.status();
    }
    unit.observation = observation.value();
  }

  const Result<std::uint8_t> has_floor = reader.u8();
  if (!has_floor.ok()) {
    return field_error("unit.policy.has_floor", has_floor.status());
  }
  if (has_floor.value() > 1u) {
    return range_error("unit.policy.has_floor", has_floor.value(), 0, 1);
  }
  const Result<std::uint8_t> floor_unit = reader.u8();
  if (!floor_unit.ok()) {
    return field_error("unit.policy.floor_unit", floor_unit.status());
  }
  const Result<std::int64_t> floor_value = reader.i64();
  if (!floor_value.ok()) {
    return field_error("unit.policy.floor_value", floor_value.status());
  }
  if (has_floor.value() == 1u) {
    if (floor_unit.value() < 1u || floor_unit.value() > 5u) {
      return range_error("unit.policy.floor_unit", floor_unit.value(), 1, 5);
    }
    const Result<ReserveQuantity> quantity =
        make_reserve_quantity(static_cast<ReserveUnit>(floor_unit.value()), floor_value.value(), limits);
    if (!quantity.ok()) {
      return quantity.status();
    }
    unit.reserve_policy.discharge_floor = quantity.value();
  } else if (floor_unit.value() != 0u || floor_value.value() != 0) {
    return Status::error(StatusCode::Corruption,
                         "unit reserve floor must be absent when it is not declared");
  }
  const Result<std::int64_t> max_age = reader.i64();
  if (!max_age.ok()) {
    return field_error("unit.policy.max_evidence_age", max_age.status());
  }
  unit.reserve_policy.max_evidence_age = TickSpan{max_age.value()};
  const Result<std::int64_t> max_skew = reader.i64();
  if (!max_skew.ok()) {
    return field_error("unit.policy.max_future_skew", max_skew.status());
  }
  unit.reserve_policy.max_future_skew = TickSpan{max_skew.value()};
  const Result<EvidenceQuality> minimum_quality =
      read_enum<EvidenceQuality>(reader, "unit.policy.minimum_quality", 0, 3);
  if (!minimum_quality.ok()) {
    return minimum_quality.status();
  }
  unit.reserve_policy.minimum_quality = minimum_quality.value();

  const Result<std::uint32_t> obligation_count = reader.u32();
  if (!obligation_count.ok()) {
    return field_error("unit.obligation_count", obligation_count.status());
  }
  if (obligation_count.value() > limits.max_obligations_per_ups) {
    return Status::error(StatusCode::LimitExceeded,
                         "unit declares " + std::to_string(obligation_count.value()) +
                             " obligations, the limit is " +
                             std::to_string(limits.max_obligations_per_ups));
  }
  unit.obligations.reserve(obligation_count.value());
  for (std::uint32_t index = 0; index < obligation_count.value(); ++index) {
    const Result<ProtectedLoadObligation> obligation = read_obligation(reader, limits);
    if (!obligation.ok()) {
      return obligation.status();
    }
    unit.obligations.push_back(obligation.value());
  }

  const Result<std::uint32_t> grant_count = reader.u32();
  if (!grant_count.ok()) {
    return field_error("unit.grant_count", grant_count.status());
  }
  if (grant_count.value() > limits.max_grants_per_ups) {
    return Status::error(StatusCode::LimitExceeded,
                         "unit declares " + std::to_string(grant_count.value()) +
                             " grants, the limit is " + std::to_string(limits.max_grants_per_ups));
  }
  unit.grants.reserve(grant_count.value());
  for (std::uint32_t index = 0; index < grant_count.value(); ++index) {
    const Result<AuthorityGrant> grant = read_grant(reader, limits);
    if (!grant.ok()) {
      return grant.status();
    }
    unit.grants.push_back(grant.value());
  }

  const Result<std::uint8_t> has_in_flight = reader.u8();
  if (!has_in_flight.ok()) {
    return field_error("unit.has_in_flight", has_in_flight.status());
  }
  if (has_in_flight.value() > 1u) {
    return range_error("unit.has_in_flight", has_in_flight.value(), 0, 1);
  }
  const Result<std::uint64_t> in_flight = reader.u64();
  if (!in_flight.ok()) {
    return field_error("unit.in_flight", in_flight.status());
  }
  if (has_in_flight.value() == 1u) {
    if (in_flight.value() == 0) {
      return Status::error(StatusCode::Corruption, "unit.in_flight must name a real attempt");
    }
    unit.in_flight = AttemptId{in_flight.value()};
  } else if (in_flight.value() != 0) {
    return Status::error(StatusCode::Corruption,
                         "unit.in_flight must be zero when no attempt is in flight");
  }

  const Result<std::int64_t> last_verified = reader.i64();
  if (!last_verified.ok()) {
    return field_error("unit.last_verified_at", last_verified.status());
  }
  if (last_verified.value() < 0) {
    return Status::error(StatusCode::Corruption, "unit.last_verified_at must not be negative");
  }
  unit.last_verified_at = Tick{last_verified.value()};

  const Status status = validate_ups_record(unit, limits);
  if (!status.ok()) {
    return status;
  }
  return unit;
}

}  // namespace

Result<std::vector<std::byte>> encode_head(const HeadRecord& head) {
  if (head.identity.is_zero()) {
    return Status::error(StatusCode::InvalidArgument, "a head marker must carry a store identity");
  }
  ByteWriter writer;
  for (const char byte : kHeadMagic) {
    writer.u8(static_cast<std::uint8_t>(byte));
  }
  writer.u32(kStateFormatVersion);
  writer.u32(kByteOrderMarker);
  writer.u64(head.identity.high());
  writer.u64(head.identity.low());
  writer.u64(head.generation.value());
  writer.u64(head.epoch.value());
  writer.u64(head.incarnation.value());
  writer.u64(head.payload_bytes);
  writer.u32(head.payload_crc32c);
  writer.u32(head.path_crc32c);
  writer.u64(head.head_sequence);
  writer.i64(head.committed_at.value());
  writer.i64(head.created_at.value());
  for (int index = 0; index < 24; ++index) {
    writer.u8(0);
  }
  writer.u32(kStateFormatVersion);
  std::vector<std::byte> bytes = writer.take();
  if (bytes.size() != kHeadBytes - 4) {
    return Status::error(StatusCode::InvariantViolation,
                         "internal error: the head marker body is " + std::to_string(bytes.size()) +
                             " bytes, expected " + std::to_string(kHeadBytes - 4));
  }
  const std::uint32_t checksum = crc32c(bytes);
  ByteWriter tail;
  tail.u32(checksum);
  const std::vector<std::byte> tail_bytes = tail.take();
  bytes.insert(bytes.end(), tail_bytes.begin(), tail_bytes.end());
  return bytes;
}

Result<HeadRecord> decode_head(std::span<const std::byte> bytes) {
  if (bytes.size() != kHeadBytes) {
    return Status::error(StatusCode::Corruption,
                         "the head marker must be exactly " + std::to_string(kHeadBytes) +
                             " bytes, got " + std::to_string(bytes.size()));
  }
  const std::uint32_t expected_crc = crc32c(bytes.first(kHeadBytes - 4));
  ByteReader tail(bytes.subspan(kHeadBytes - 4, 4));
  const Result<std::uint32_t> stored_crc = tail.u32();
  if (!stored_crc.ok()) {
    return stored_crc.status();
  }
  if (stored_crc.value() != expected_crc) {
    return Status::error(StatusCode::Corruption,
                         "the head marker fails its integrity check: stored " +
                             std::to_string(stored_crc.value()) + ", computed " +
                             std::to_string(expected_crc));
  }

  ByteReader reader(bytes.first(kHeadBytes - 4));
  for (const char expected : kHeadMagic) {
    const Result<std::uint8_t> byte = reader.u8();
    if (!byte.ok()) {
      return byte.status();
    }
    if (byte.value() != static_cast<std::uint8_t>(expected)) {
      return Status::error(StatusCode::Corruption,
                           "the head marker magic does not match; this is not a UPS Control store "
                           "head marker");
    }
  }
  const Result<std::uint32_t> version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != kStateFormatVersion) {
    return Status::error(StatusCode::IncompatibleVersion,
                         "the head marker declares format version " + std::to_string(version.value()) +
                             ", this build understands version " +
                             std::to_string(kStateFormatVersion));
  }
  const Result<std::uint32_t> marker = reader.u32();
  if (!marker.ok()) {
    return marker.status();
  }
  if (marker.value() != kByteOrderMarker) {
    return Status::error(StatusCode::EndianMismatch,
                         "the head marker was written in the opposite byte order");
  }

  HeadRecord head;
  const Result<std::uint64_t> high = reader.u64();
  if (!high.ok()) {
    return high.status();
  }
  const Result<std::uint64_t> low = reader.u64();
  if (!low.ok()) {
    return low.status();
  }
  const Result<StoreIdentity> identity = StoreIdentity::from_components(high.value(), low.value());
  if (!identity.ok()) {
    return identity.status();
  }
  head.identity = identity.value();

  const Result<std::uint64_t> generation = reader.u64();
  if (!generation.ok()) {
    return generation.status();
  }
  head.generation = StoreGeneration{generation.value()};
  if (head.generation.is_zero()) {
    return Status::error(StatusCode::Corruption, "the head marker names generation zero");
  }
  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.status();
  }
  head.epoch = ControlEpoch{epoch.value()};
  if (head.epoch.is_zero()) {
    return Status::error(StatusCode::Corruption, "the head marker names control epoch zero");
  }
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  head.incarnation = Incarnation{incarnation.value()};
  if (head.incarnation.is_zero()) {
    return Status::error(StatusCode::Corruption, "the head marker names incarnation zero");
  }

  const Result<std::uint64_t> payload_bytes = reader.u64();
  if (!payload_bytes.ok()) {
    return payload_bytes.status();
  }
  head.payload_bytes = payload_bytes.value();
  const Result<std::uint32_t> payload_crc = reader.u32();
  if (!payload_crc.ok()) {
    return payload_crc.status();
  }
  head.payload_crc32c = payload_crc.value();
  const Result<std::uint32_t> path_crc = reader.u32();
  if (!path_crc.ok()) {
    return path_crc.status();
  }
  head.path_crc32c = path_crc.value();
  const Result<std::uint64_t> sequence = reader.u64();
  if (!sequence.ok()) {
    return sequence.status();
  }
  head.head_sequence = sequence.value();
  const Result<std::int64_t> committed_at = reader.i64();
  if (!committed_at.ok()) {
    return committed_at.status();
  }
  head.committed_at = Tick{committed_at.value()};
  const Result<std::int64_t> created_at = reader.i64();
  if (!created_at.ok()) {
    return created_at.status();
  }
  head.created_at = Tick{created_at.value()};
  for (int index = 0; index < 24; ++index) {
    const Result<std::uint8_t> reserved = reader.u8();
    if (!reserved.ok()) {
      return reserved.status();
    }
    if (reserved.value() != 0u) {
      return Status::error(StatusCode::Corruption,
                           "a reserved byte of the head marker is not zero");
    }
  }
  const Result<std::uint32_t> repeated_version = reader.u32();
  if (!repeated_version.ok()) {
    return repeated_version.status();
  }
  if (repeated_version.value() != kStateFormatVersion) {
    return Status::error(StatusCode::Corruption,
                         "the repeated format version at the tail of the head marker does not "
                         "match the version at its head");
  }
  if (!reader.at_end()) {
    return Status::error(StatusCode::Corruption,
                         "the head marker carries trailing bytes inside its fixed frame");
  }
  return head;
}

Result<std::vector<std::byte>> encode_state(const UpsState& state, const ResourceLimits& limits) {
  const Status valid = validate_state(state, limits);
  if (!valid.ok()) {
    return valid;
  }
  ByteWriter writer;
  writer.u32(kStateFormatVersion);
  writer.u32(kByteOrderMarker);
  writer.u64(state.generation.value());
  writer.u64(state.epoch.value());
  writer.u64(state.incarnation.value());
  writer.i64(state.created_at.value());
  writer.i64(state.updated_at.value());
  writer.i64(state.revalidated_at.value());
  writer.u64(state.last_attempt.value());
  writer.u64(state.operation_count);

  writer.u32(static_cast<std::uint32_t>(state.units.size()));
  for (const UpsRecord& unit : state.units) {
    const Status status = write_unit(writer, unit, limits);
    if (!status.ok()) {
      return status;
    }
  }
  writer.u32(static_cast<std::uint32_t>(state.attempts.size()));
  for (const AttemptRecord& attempt : state.attempts) {
    const Status status = write_attempt(writer, attempt, limits);
    if (!status.ok()) {
      return status;
    }
  }
  writer.u32(static_cast<std::uint32_t>(state.idempotency.size()));
  for (const IdempotencyEntry& entry : state.idempotency) {
    const Status status = writer.text(entry.key.value(), static_cast<std::uint32_t>(limits.max_idempotency_key_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u64(entry.attempt.value());
    writer.u64(entry.plan_digest);
    writer.i64(entry.recorded_at.value());
  }
  writer.u32(static_cast<std::uint32_t>(state.commit_log.size()));
  for (const CommitLogEntry& entry : state.commit_log) {
    writer.u64(entry.generation.value());
    writer.u64(entry.attempt.value());
    const Status status = writer.text(entry.key.value(), static_cast<std::uint32_t>(limits.max_idempotency_key_bytes));
    if (!status.ok()) {
      return status;
    }
    const Status kind = write_enum(writer, entry.operation, "commit_log.operation", 0, 17);
    if (!kind.ok()) {
      return kind;
    }
    const Status ups = writer.text(entry.ups.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!ups.ok()) {
      return ups;
    }
    writer.u64(entry.revision.value());
    writer.i64(entry.at.value());
  }
  return writer.take();
}

Result<UpsState> decode_state(std::span<const std::byte> bytes, const ResourceLimits& limits) {
  ByteReader reader(bytes);
  const Result<std::uint32_t> version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != kStateFormatVersion) {
    return Status::error(StatusCode::IncompatibleVersion,
                         "the payload declares format version " + std::to_string(version.value()) +
                             ", this build understands version " +
                             std::to_string(kStateFormatVersion));
  }
  const Result<std::uint32_t> marker = reader.u32();
  if (!marker.ok()) {
    return marker.status();
  }
  if (marker.value() != kByteOrderMarker) {
    return Status::error(StatusCode::EndianMismatch,
                         "the payload was written in the opposite byte order");
  }

  UpsState state;
  const Result<std::uint64_t> generation = reader.u64();
  if (!generation.ok()) {
    return generation.status();
  }
  state.generation = StoreGeneration{generation.value()};
  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.status();
  }
  state.epoch = ControlEpoch{epoch.value()};
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  state.incarnation = Incarnation{incarnation.value()};

  const Result<std::int64_t> created_at = reader.i64();
  if (!created_at.ok()) {
    return created_at.status();
  }
  state.created_at = Tick{created_at.value()};
  const Result<std::int64_t> updated_at = reader.i64();
  if (!updated_at.ok()) {
    return updated_at.status();
  }
  state.updated_at = Tick{updated_at.value()};
  const Result<std::int64_t> revalidated_at = reader.i64();
  if (!revalidated_at.ok()) {
    return revalidated_at.status();
  }
  state.revalidated_at = Tick{revalidated_at.value()};
  if (state.created_at.value() < 0 || state.updated_at.value() < 0 || state.revalidated_at.value() < 0) {
    return Status::error(StatusCode::Corruption, "state instants must not be negative");
  }

  const Result<std::uint64_t> last_attempt = reader.u64();
  if (!last_attempt.ok()) {
    return last_attempt.status();
  }
  state.last_attempt = AttemptId{last_attempt.value()};
  const Result<std::uint64_t> operations = reader.u64();
  if (!operations.ok()) {
    return operations.status();
  }
  state.operation_count = operations.value();

  const Result<std::uint32_t> unit_count = reader.u32();
  if (!unit_count.ok()) {
    return unit_count.status();
  }
  if (unit_count.value() > limits.max_ups_units) {
    return Status::error(StatusCode::LimitExceeded,
                         "the payload declares " + std::to_string(unit_count.value()) +
                             " units, the limit is " + std::to_string(limits.max_ups_units));
  }
  state.units.reserve(unit_count.value());
  for (std::uint32_t index = 0; index < unit_count.value(); ++index) {
    const Result<UpsRecord> unit = read_unit(reader, limits);
    if (!unit.ok()) {
      return unit.status();
    }
    state.units.push_back(unit.value());
  }

  const Result<std::uint32_t> attempt_count = reader.u32();
  if (!attempt_count.ok()) {
    return attempt_count.status();
  }
  if (attempt_count.value() > limits.max_attempt_journal) {
    return Status::error(StatusCode::LimitExceeded,
                         "the payload declares " + std::to_string(attempt_count.value()) +
                             " attempts, the limit is " +
                             std::to_string(limits.max_attempt_journal));
  }
  state.attempts.reserve(attempt_count.value());
  for (std::uint32_t index = 0; index < attempt_count.value(); ++index) {
    const Result<AttemptRecord> attempt = read_attempt(reader, limits);
    if (!attempt.ok()) {
      return attempt.status();
    }
    state.attempts.push_back(attempt.value());
  }

  const Result<std::uint32_t> idempotency_count = reader.u32();
  if (!idempotency_count.ok()) {
    return idempotency_count.status();
  }
  if (idempotency_count.value() > limits.max_idempotency_records) {
    return Status::error(StatusCode::LimitExceeded,
                         "the payload declares " + std::to_string(idempotency_count.value()) +
                             " idempotency bindings, the limit is " +
                             std::to_string(limits.max_idempotency_records));
  }
  state.idempotency.reserve(idempotency_count.value());
  for (std::uint32_t index = 0; index < idempotency_count.value(); ++index) {
    IdempotencyEntry entry;
    const Result<std::string> key = reader.text(static_cast<std::uint32_t>(limits.max_idempotency_key_bytes));
    if (!key.ok()) {
      return key.status();
    }
    const Result<IdempotencyKey> key_id =
        IdempotencyKey::parse(key.value(), limits.max_idempotency_key_bytes);
    if (!key_id.ok()) {
      return key_id.status();
    }
    entry.key = key_id.value();
    const Result<std::uint64_t> attempt = reader.u64();
    if (!attempt.ok()) {
      return attempt.status();
    }
    entry.attempt = AttemptId{attempt.value()};
    const Result<std::uint64_t> digest = reader.u64();
    if (!digest.ok()) {
      return digest.status();
    }
    entry.plan_digest = digest.value();
    const Result<std::int64_t> recorded_at = reader.i64();
    if (!recorded_at.ok()) {
      return recorded_at.status();
    }
    entry.recorded_at = Tick{recorded_at.value()};
    state.idempotency.push_back(entry);
  }

  const Result<std::uint32_t> commit_count = reader.u32();
  if (!commit_count.ok()) {
    return commit_count.status();
  }
  if (commit_count.value() > limits.max_commit_log) {
    return Status::error(StatusCode::LimitExceeded,
                         "the payload declares " + std::to_string(commit_count.value()) +
                             " commit log entries, the limit is " +
                             std::to_string(limits.max_commit_log));
  }
  state.commit_log.reserve(commit_count.value());
  for (std::uint32_t index = 0; index < commit_count.value(); ++index) {
    CommitLogEntry entry;
    const Result<std::uint64_t> generation_value = reader.u64();
    if (!generation_value.ok()) {
      return generation_value.status();
    }
    entry.generation = StoreGeneration{generation_value.value()};
    const Result<std::uint64_t> attempt = reader.u64();
    if (!attempt.ok()) {
      return attempt.status();
    }
    entry.attempt = AttemptId{attempt.value()};
    const Result<std::string> key = reader.text(static_cast<std::uint32_t>(limits.max_idempotency_key_bytes));
    if (!key.ok()) {
      return key.status();
    }
    if (!key.value().empty()) {
      const Result<IdempotencyKey> key_id =
          IdempotencyKey::parse(key.value(), limits.max_idempotency_key_bytes);
      if (!key_id.ok()) {
        return key_id.status();
      }
      entry.key = key_id.value();
    }
    const Result<OperationKind> operation = read_enum<OperationKind>(reader, "commit_log.operation", 0, 17);
    if (!operation.ok()) {
      return operation.status();
    }
    entry.operation = operation.value();
    const Result<std::string> ups = reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!ups.ok()) {
      return ups.status();
    }
    if (!ups.value().empty()) {
      const Result<UpsId> ups_id = UpsId::parse(ups.value(), limits.max_identifier_bytes);
      if (!ups_id.ok()) {
        return ups_id.status();
      }
      entry.ups = ups_id.value();
    }
    const Result<std::uint64_t> revision = reader.u64();
    if (!revision.ok()) {
      return revision.status();
    }
    entry.revision = StateRevision{revision.value()};
    const Result<std::int64_t> at = reader.i64();
    if (!at.ok()) {
      return at.status();
    }
    entry.at = Tick{at.value()};
    state.commit_log.push_back(entry);
  }

  if (!reader.at_end()) {
    return Status::error(StatusCode::Corruption,
                         "the payload carries " + std::to_string(reader.remaining()) +
                             " trailing bytes; a state section must be consumed exactly");
  }
  const Status valid = validate_state(state, limits);
  if (!valid.ok()) {
    return valid;
  }
  return state;
}

}  // namespace ups_control::detail
