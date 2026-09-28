#pragma once

// Internal canonical state codec and store envelope codec. Not installed.
//
// The logical state is encoded in a fixed field order with explicit
// little-endian integers and length-prefixed text. Nothing volatile enters the
// encoding: no wall clock, no process identifier, no memory address, no
// iteration order that depends on a container's implementation, and no store
// identity or path. Two logically equal states therefore encode to identical
// bytes.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/state.hpp"
#include "ups_control/status.hpp"

namespace ups_control::detail {

inline constexpr std::size_t kHeadBytes = 128;

/// The fixed-size authoritative head marker.
struct HeadRecord {
  StoreIdentity identity;
  StoreGeneration generation;
  ControlEpoch epoch;
  Incarnation incarnation;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc32c = 0;
  std::uint32_t path_crc32c = 0;
  std::uint64_t head_sequence = 0;
  Tick committed_at;
  Tick created_at;
};

Result<std::vector<std::byte>> encode_head(const HeadRecord& head);
Result<HeadRecord> decode_head(std::span<const std::byte> bytes);

/// Encodes only the logical state, with no envelope.
Result<std::vector<std::byte>> encode_state(const UpsState& state, const ResourceLimits& limits);

/// Decodes a logical state. Every declared length is checked against the limits
/// before allocation, every enumeration value is range-checked, and the decoded
/// state is structurally validated before it is returned.
Result<UpsState> decode_state(std::span<const std::byte> bytes, const ResourceLimits& limits);

}  // namespace ups_control::detail
