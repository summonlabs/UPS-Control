#pragma once

// Minimal deterministic JSON rendering for the command line tool. Not installed.
//
// Only the shapes this tool emits are supported: an object whose fields are
// written in the order they are added. There is no parser here, and the tool
// never accepts JSON as input.

#include <cstdint>
#include <string>
#include <vector>

namespace cli {

/// Quotes and escapes a string as a JSON string literal. Every byte outside the
/// printable ASCII range is emitted as a \u escape over its UTF-8 bytes, so the
/// output is pure ASCII and byte-for-byte deterministic.
std::string json_quote(const std::string& value);

/// Quotes and escapes a string, passing valid multi-byte UTF-8 through unchanged.
std::string json_quote_utf8(const std::string& value);

class JsonObject {
 public:
  JsonObject& text(const std::string& key, const std::string& value);
  JsonObject& number(const std::string& key, std::int64_t value);
  JsonObject& unsigned_number(const std::string& key, std::uint64_t value);
  JsonObject& boolean(const std::string& key, bool value);
  /// Inserts an already-rendered JSON value.
  JsonObject& raw(const std::string& key, const std::string& json);
  JsonObject& strings(const std::string& key, const std::vector<std::string>& values);

  bool empty() const noexcept { return fields_.empty(); }
  std::string str() const;

 private:
  std::vector<std::string> fields_;
};

}  // namespace cli
