#include "ups_control/ids.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <random>
#include <string>

namespace ups_control {
namespace {

bool is_identifier_character(unsigned char character) noexcept {
  if (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) {
    return true;
  }
  if (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) {
    return true;
  }
  if (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9')) {
    return true;
  }
  switch (character) {
    case static_cast<unsigned char>('.'):
    case static_cast<unsigned char>('_'):
    case static_cast<unsigned char>('-'):
    case static_cast<unsigned char>(':'):
    case static_cast<unsigned char>('@'):
      return true;
    default:
      return false;
  }
}

bool is_continuation_byte(unsigned char character) noexcept {
  return (character & 0xC0u) == 0x80u;
}

/// Strict UTF-8 validation: rejects overlong encodings, surrogate halves, code
/// points above U+10FFFF, and truncated sequences. A label that is not valid
/// UTF-8 is refused rather than normalized, so normalization can never erase the
/// evidence that the input was malformed.
bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80u) {
      ++index;
      continue;
    } else if ((lead & 0xE0u) == 0xC0u) {
      extra = 1;
      code_point = lead & 0x1Fu;
      if (code_point < 0x02u) {
        return false;  // overlong
      }
    } else if ((lead & 0xF0u) == 0xE0u) {
      extra = 2;
      code_point = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
      extra = 3;
      code_point = lead & 0x07u;
      if (code_point > 0x04u) {
        return false;  // above U+10FFFF
      }
    } else {
      return false;
    }
    if (index + extra >= text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if (!is_continuation_byte(continuation)) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3Fu);
    }
    if (extra == 2 && code_point < 0x0800u) {
      return false;  // overlong
    }
    if (extra == 3 && code_point < 0x10000u) {
      return false;  // overlong
    }
    // The maximum is re-checked after the continuations are folded in, not only
    // against the lead byte. A 0xF4 lead carries the value 0x04 and passes a
    // lead-byte-only test, but a second byte above 0x8F lifts the result to
    // U+110000..U+13FFFF, which RFC 3629 does not permit.
    if (code_point > 0x10FFFFu) {
      return false;  // above U+10FFFF
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
      return false;  // surrogate half
    }
    index += extra + 1;
  }
  return true;
}

}  // namespace

Status validate_identifier(std::string_view text, std::size_t max_bytes) {
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument, "identifier must not be empty");
  }
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "identifier of " + std::to_string(text.size()) +
                             " bytes exceeds the limit of " + std::to_string(max_bytes) + " bytes");
  }
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto value = static_cast<unsigned char>(text[index]);
    if (!is_identifier_character(value)) {
      return Status::error(StatusCode::InvalidArgument,
                           "identifier contains a character outside A-Z a-z 0-9 . _ - : @ at byte " +
                               std::to_string(index));
    }
  }
  return Status::success();
}

Status validate_label(std::string_view text, std::size_t max_bytes) {
  if (text.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "label of " + std::to_string(text.size()) +
                             " bytes exceeds the limit of " + std::to_string(max_bytes) + " bytes");
  }
  if (!is_valid_utf8(text)) {
    return Status::error(StatusCode::InvalidArgument, "label is not valid UTF-8");
  }
  for (const char character : text) {
    const auto value = static_cast<unsigned char>(character);
    if (value == 0x00u) {
      return Status::error(StatusCode::InvalidArgument, "label must not contain a NUL byte");
    }
    if (value < 0x20u || value == 0x7Fu) {
      return Status::error(StatusCode::InvalidArgument,
                           "label must not contain a C0 control or DEL character");
    }
  }
  return Status::success();
}

template <FingerprintFamily Family>
BasicFingerprint<Family> BasicFingerprint<Family>::generate() {
  std::random_device device;
  std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^
                         static_cast<std::uint64_t>(device()));
  std::uniform_int_distribution<std::uint64_t> distribution;
  BasicFingerprint result;
  for (int attempt = 0; attempt < 8; ++attempt) {
    result.high_ = distribution(engine);
    result.low_ = distribution(engine);
    if (!result.is_zero()) {
      return result;
    }
  }
  // A deterministic non-zero fallback keeps the contract "never zero" even if the
  // entropy source is degenerate.
  result.high_ = 0x9E3779B97F4A7C15ull;
  result.low_ = 0xBF58476D1CE4E5B9ull;
  return result;
}

template <FingerprintFamily Family>
Result<BasicFingerprint<Family>> BasicFingerprint<Family>::parse(std::string_view hex) {
  if (hex.size() != 32) {
    return Status::error(StatusCode::InvalidArgument,
                         "fingerprint must be exactly 32 hexadecimal characters, got " +
                             std::to_string(hex.size()));
  }
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (std::size_t index = 0; index < 32; ++index) {
    const char character = hex[index];
    std::uint64_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uint64_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uint64_t>(character - 'a') + 10u;
    } else {
      return Status::error(StatusCode::InvalidArgument,
                           "fingerprint must be lowercase hexadecimal; found a character outside "
                           "0-9 a-f at index " + std::to_string(index));
    }
    if (index < 16) {
      high = (high << 4) | digit;
    } else {
      low = (low << 4) | digit;
    }
  }
  return from_components(high, low);
}

template <FingerprintFamily Family>
Result<BasicFingerprint<Family>> BasicFingerprint<Family>::from_components(std::uint64_t high,
                                                                           std::uint64_t low) {
  if (high == 0 && low == 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "the all-zero fingerprint means unset and is never a valid identity");
  }
  BasicFingerprint result;
  result.high_ = high;
  result.low_ = low;
  return result;
}

template <FingerprintFamily Family>
std::string BasicFingerprint<Family>::to_hex() const {
  static const char* digits = "0123456789abcdef";
  std::string text(32, '0');
  for (int index = 0; index < 16; ++index) {
    const unsigned shift = static_cast<unsigned>(60 - 4 * index);
    text[static_cast<std::size_t>(index)] = digits[(high_ >> shift) & 0xFull];
    text[static_cast<std::size_t>(16 + index)] = digits[(low_ >> shift) & 0xFull];
  }
  return text;
}

template class BasicFingerprint<FingerprintFamily::StoreIdentity>;
template class BasicFingerprint<FingerprintFamily::SessionId>;

}  // namespace ups_control
