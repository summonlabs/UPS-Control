#include "detail/codec.hpp"

#include <cstring>
#include <string>

namespace ups_control::detail {
namespace {

Status truncated(std::size_t needed, std::size_t remaining) {
  return Status::error(StatusCode::Corruption,
                       "truncated input: needed " + std::to_string(needed) + " bytes but only " +
                           std::to_string(remaining) + " remain");
}

}  // namespace

void ByteWriter::u8(std::uint8_t value) {
  buffer_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> static_cast<unsigned>(shift)) & 0xFFu));
  }
}

void ByteWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void ByteWriter::raw(std::span<const std::byte> data) {
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

Status ByteWriter::text(std::string_view value, std::uint32_t max_bytes) {
  if (value.size() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "text of " + std::to_string(value.size()) + " bytes exceeds the limit of " +
                             std::to_string(max_bytes));
  }
  u32(static_cast<std::uint32_t>(value.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  buffer_.insert(buffer_.end(), begin, begin + value.size());
  return Status::success();
}

Result<std::uint8_t> ByteReader::u8() {
  if (remaining() < 1) {
    return truncated(1, remaining());
  }
  const auto value = static_cast<std::uint8_t>(data_[offset_]);
  ++offset_;
  return value;
}

Result<std::uint32_t> ByteReader::u32() {
  if (remaining() < 4) {
    return truncated(4, remaining());
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(index)]) << (8 * index);
  }
  offset_ += 4;
  return value;
}

Result<std::uint64_t> ByteReader::u64() {
  if (remaining() < 8) {
    return truncated(8, remaining());
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(index)])
             << (8 * index);
  }
  offset_ += 8;
  return value;
}

Result<std::int64_t> ByteReader::i64() {
  const Result<std::uint64_t> value = u64();
  if (!value.ok()) {
    return value.status();
  }
  return static_cast<std::int64_t>(value.value());
}

Result<std::span<const std::byte>> ByteReader::raw(std::size_t count) {
  if (remaining() < count) {
    return truncated(count, remaining());
  }
  const std::span<const std::byte> slice = data_.subspan(offset_, count);
  offset_ += count;
  return slice;
}

Result<std::string> ByteReader::text(std::uint32_t max_bytes) {
  const Result<std::uint32_t> length = u32();
  if (!length.ok()) {
    return length.status();
  }
  if (length.value() > max_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "declared text length " + std::to_string(length.value()) +
                             " exceeds the limit of " + std::to_string(max_bytes));
  }
  const Result<std::span<const std::byte>> payload = raw(length.value());
  if (!payload.ok()) {
    return payload.status();
  }
  std::string text(payload.value().size(), '\0');
  if (!payload.value().empty()) {
    std::memcpy(text.data(), payload.value().data(), payload.value().size());
  }
  return text;
}

}  // namespace ups_control::detail
