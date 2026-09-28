#pragma once

// Internal deterministic change-detection digest. Not installed.

#include <cstdint>
#include <span>
#include <string_view>

namespace ups_control::detail {

/// FNV-1a 64-bit digest.
///
/// Used to answer "did this answer change" for revalidation. It detects change;
/// it is not a cryptographic commitment and does not resist deliberate
/// collision construction.
class Digest64 {
 public:
  void update(std::span<const std::byte> data) noexcept;
  void update_u64(std::uint64_t value) noexcept;
  void update_i64(std::int64_t value) noexcept;
  void update_text(std::string_view value) noexcept;
  void update_bool(bool value) noexcept;

  std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_ = 14695981039346656037ULL;
};

}  // namespace ups_control::detail
