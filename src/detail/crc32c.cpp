#include "detail/crc32c.hpp"

#include <array>

namespace ups_control::detail {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78u;  // reflected Castagnoli polynomial

std::array<std::uint32_t, 256> make_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? ((value >> 1) ^ kPolynomial) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

const std::array<std::uint32_t, 256>& table() noexcept {
  static const std::array<std::uint32_t, 256> instance = make_table();
  return instance;
}

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) noexcept {
  const std::array<std::uint32_t, 256>& lookup = table();
  std::uint32_t crc = ~seed;
  for (const std::byte byte : data) {
    const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint32_t>(byte));
    crc = lookup[index] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace ups_control::detail
