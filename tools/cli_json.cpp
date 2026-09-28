#include "cli_json.hpp"

#include <array>
#include <cstdio>

namespace cli {
namespace {

bool is_utf8_continuation(unsigned char byte) noexcept {
  return (byte & 0xC0u) == 0x80u;
}

void emit_ascii_escaped(std::string& out, const std::string& value) {
  static const char* digits = "0123456789abcdef";
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (byte) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte >= 0x20u && byte < 0x7Fu) {
          out += character;
        } else {
          out += "\\u00";
          out += digits[(byte >> 4) & 0xFu];
          out += digits[byte & 0xFu];
        }
        break;
    }
  }
}

}  // namespace

std::string json_quote(const std::string& value) {
  std::string out = "\"";
  emit_ascii_escaped(out, value);
  out += "\"";
  return out;
}

std::string json_quote_utf8(const std::string& value) {
  std::string out = "\"";
  std::size_t index = 0;
  while (index < value.size()) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte >= 0x20u && byte < 0x7Fu) {
      const char character = value[index];
      if (character == '"' || character == '\\') {
        out += '\\';
      }
      out += character;
      ++index;
      continue;
    }
    if (byte >= 0xC2u && byte <= 0xF4u) {
      std::size_t length = 1;
      if (byte >= 0xF0u) {
        length = 4;
      } else if (byte >= 0xE0u) {
        length = 3;
      } else {
        length = 2;
      }
      bool valid = index + length <= value.size();
      for (std::size_t offset = 1; valid && offset < length; ++offset) {
        valid = is_utf8_continuation(static_cast<unsigned char>(value[index + offset]));
      }
      if (valid) {
        out.append(value, index, length);
        index += length;
        continue;
      }
    }
    out += "\\u00";
    static const char* digits = "0123456789abcdef";
    out += digits[(byte >> 4) & 0xFu];
    out += digits[byte & 0xFu];
    ++index;
  }
  out += "\"";
  return out;
}

JsonObject& JsonObject::text(const std::string& key, const std::string& value) {
  fields_.push_back(json_quote(key) + ":" + json_quote_utf8(value));
  return *this;
}

JsonObject& JsonObject::number(const std::string& key, std::int64_t value) {
  fields_.push_back(json_quote(key) + ":" + std::to_string(value));
  return *this;
}

JsonObject& JsonObject::unsigned_number(const std::string& key, std::uint64_t value) {
  fields_.push_back(json_quote(key) + ":" + std::to_string(value));
  return *this;
}

JsonObject& JsonObject::boolean(const std::string& key, bool value) {
  fields_.push_back(json_quote(key) + ":" + std::string(value ? "true" : "false"));
  return *this;
}

JsonObject& JsonObject::raw(const std::string& key, const std::string& json) {
  fields_.push_back(json_quote(key) + ":" + json);
  return *this;
}

JsonObject& JsonObject::strings(const std::string& key, const std::vector<std::string>& values) {
  std::string array = "[";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      array += ",";
    }
    array += json_quote_utf8(values[index]);
  }
  array += "]";
  fields_.push_back(json_quote(key) + ":" + array);
  return *this;
}

std::string JsonObject::str() const {
  std::string out = "{";
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    if (index > 0) {
      out += ",";
    }
    out += fields_[index];
  }
  out += "}";
  return out;
}

}  // namespace cli
