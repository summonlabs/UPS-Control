#include "detail/digest.hpp"

namespace ups_control::detail {
namespace {

constexpr std::uint64_t kPrime = 1099511628211ULL;

void absorb(std::uint64_t& state, std::uint8_t byte) noexcept {
  state ^= static_cast<std::uint64_t>(byte);
  state *= kPrime;
}

}  // namespace

void Digest64::update(std::span<const std::byte> data) noexcept {
  for (const std::byte byte : data) {
    absorb(state_, static_cast<std::uint8_t>(byte));
  }
}

void Digest64::update_u64(std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    absorb(state_, static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFu));
  }
}

void Digest64::update_i64(std::int64_t value) noexcept {
  update_u64(static_cast<std::uint64_t>(value));
}

void Digest64::update_text(std::string_view value) noexcept {
  for (const char character : value) {
    absorb(state_, static_cast<std::uint8_t>(character));
  }
  absorb(state_, 0xFFu);
}

void Digest64::update_bool(bool value) noexcept {
  absorb(state_, value ? 1u : 0u);
}

}  // namespace ups_control::detail
