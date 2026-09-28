#pragma once

// Internal CRC-32C (Castagnoli) implementation. Not installed.
//
// The store integrity check detects accidental corruption: bit rot, a truncated
// write, a partial flush, or a torn block. It is not a cryptographic
// authentication code, does not use a secret, and does not resist an adversary
// who can rewrite the artifact and recompute the check. The format's structural
// invariants and envelope checks are what reject a wrong-version,
// wrong-endian, oversized, or path-manipulated artifact.

#include <cstddef>
#include <cstdint>
#include <span>

namespace ups_control::detail {

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0) noexcept;

}  // namespace ups_control::detail
