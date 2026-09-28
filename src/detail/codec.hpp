#pragma once

// Internal little-endian byte codec. Not installed.
//
// Every multi-byte integer is written and read explicitly in little-endian byte
// order, so the artifact format does not depend on the host byte order and a
// byte-swapped artifact is detected rather than misinterpreted. Every read is
// bounded by the remaining input, and every declared length is checked against a
// caller-supplied maximum before any allocation happens.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ups_control/status.hpp"

namespace ups_control::detail {

class ByteWriter {
 public:
  void u8(std::uint8_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void raw(std::span<const std::byte> data);
  /// Writes a `u32` byte length followed by the bytes. The length is checked
  /// against `max_bytes` before anything is written.
  Status text(std::string_view value, std::uint32_t max_bytes);

  const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  std::vector<std::byte> take() noexcept { return std::move(buffer_); }
  std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::byte> buffer_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  Result<std::uint8_t> u8();
  Result<std::uint32_t> u32();
  Result<std::uint64_t> u64();
  Result<std::int64_t> i64();
  Result<std::span<const std::byte>> raw(std::size_t count);
  /// Reads a `u32` byte length and then that many bytes, refusing a length above
  /// `max_bytes` or beyond the end of the input.
  Result<std::string> text(std::uint32_t max_bytes);

  bool at_end() const noexcept { return offset_ == data_.size(); }
  std::size_t remaining() const noexcept { return data_.size() - offset_; }
  std::size_t offset() const noexcept { return offset_; }

 private:
  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
};

}  // namespace ups_control::detail
