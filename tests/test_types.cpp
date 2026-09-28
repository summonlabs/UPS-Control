// Unit and identifier proofs for the UPS Control library.
//
// Every check below states a contract taken from the public headers, which are
// the source of truth: include/ups_control/ids.hpp, units.hpp, arith.hpp,
// limits.hpp and status.hpp. Reference vocabularies are written out a second
// time here and compared with the library's own answer, so a table that drifts
// fails instead of silently passing.

#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ups_control/arith.hpp"
#include "ups_control/ids.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/status.hpp"
#include "ups_control/units.hpp"

using namespace ups_control;

namespace {

constexpr std::int64_t kInt64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kInt64Min = (std::numeric_limits<std::int64_t>::min)();

/// One byte as a one-character string, built without narrowing conversions.
std::string byte_string(unsigned int value) {
  const unsigned char byte = static_cast<unsigned char>(value & 0xFFu);
  std::string text;
  text.push_back(static_cast<char>(byte));
  return text;
}

/// A byte sequence, built without narrowing conversions.
std::string raw_bytes(std::initializer_list<unsigned int> values) {
  std::string text;
  for (const unsigned int value : values) {
    text.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFFu)));
  }
  return text;
}

std::string repeated(char character, std::size_t count) { return std::string(count, character); }

/// The documented identifier alphabet: A-Z a-z 0-9 . _ - : @ and nothing else.
bool reference_identifier_byte(unsigned char byte) {
  if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
    return true;
  }
  if (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z')) {
    return true;
  }
  if (byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) {
    return true;
  }
  switch (byte) {
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

/// Detection helpers for the type-separation proofs. A requires-expression in a
/// non-template context is evaluated eagerly rather than as a substitution, so
/// the classic void_t detection idiom is used instead: it is well formed on
/// every compiler and answers the same question.
template <typename Left, typename Right, typename = void>
struct is_equality_comparable : std::false_type {};
template <typename Left, typename Right>
struct is_equality_comparable<
    Left, Right,
    std::void_t<decltype(std::declval<const Left&>() == std::declval<const Right&>())>>
    : std::true_type {};

template <typename Left, typename Right, typename = void>
struct is_less_comparable : std::false_type {};
template <typename Left, typename Right>
struct is_less_comparable<
    Left, Right,
    std::void_t<decltype(std::declval<const Left&>() < std::declval<const Right&>())>>
    : std::true_type {};

/// True when is_valid_instant accepts the type: only Tick does.
template <typename T>
struct accepts_instant : std::is_invocable<decltype(&is_valid_instant), T> {};

struct IdentifierCase {
  std::string text;
  std::size_t max_bytes;
  StatusCode expected;
};

void check_identifier_case(const IdentifierCase& item) {
  const Status status = validate_identifier(item.text, item.max_bytes);
  UC_CHECK_MSG(status.code() == item.expected,
               "validate_identifier(" + std::to_string(item.text.size()) + " bytes, max " +
                   std::to_string(item.max_bytes) + ") returned " + to_string(status.code()) +
                   " (" + status.message() + "), expected " + to_string(item.expected));
  // The same verdict must be reached through the typed parser entry point.
  const Status through_id = uc_test::status_of(UpsId::parse(item.text, item.max_bytes));
  UC_CHECK_MSG(through_id.code() == item.expected,
               "UpsId::parse disagreed with validate_identifier: " +
                   std::string(to_string(through_id.code())));
}

}  // namespace

UC_TEST(types, identifier_accepts_exactly_the_documented_alphabet) {
  for (unsigned int value = 0; value < 256u; ++value) {
    const std::string text = byte_string(value);
    const bool accepted = validate_identifier(text, 8).ok();
    const bool expected = reference_identifier_byte(static_cast<unsigned char>(value));
    UC_CHECK_MSG(accepted == expected,
                 "byte " + std::to_string(value) + ": accepted=" + (accepted ? "true" : "false") +
                     " but the documented alphabet says " + (expected ? "true" : "false"));
  }
  // The accepted characters are usable together, in any position.
  UC_CHECK(validate_identifier("AZaz09._-:@", 64).ok());
  UC_CHECK(validate_identifier("UPS-01.site_a:@b", 64).ok());
}

UC_TEST(types, identifier_rejects_empty_nul_whitespace_controls_separators_and_non_ascii) {
  const std::vector<IdentifierCase> cases = {
      {"", 128, StatusCode::InvalidArgument},
      {std::string("\0", 1), 128, StatusCode::InvalidArgument},
      {std::string("a\0b", 3), 128, StatusCode::InvalidArgument},
      {"a b", 128, StatusCode::InvalidArgument},
      {" leading", 128, StatusCode::InvalidArgument},
      {"trailing ", 128, StatusCode::InvalidArgument},
      {"tab\there", 128, StatusCode::InvalidArgument},
      {"line\nbreak", 128, StatusCode::InvalidArgument},
      {"carriage\rreturn", 128, StatusCode::InvalidArgument},
      {"vertical\x0Btab", 128, StatusCode::InvalidArgument},
      {"form\x0C" "feed", 128, StatusCode::InvalidArgument},
      {"del\x7F", 128, StatusCode::InvalidArgument},
      {"path/segment", 128, StatusCode::InvalidArgument},
      {"path\\segment", 128, StatusCode::InvalidArgument},
      {"..\\..", 128, StatusCode::InvalidArgument},
      {"c:/temp", 128, StatusCode::InvalidArgument},
      {"caf\xC3\xA9", 128, StatusCode::InvalidArgument},
      {raw_bytes({0x80}), 128, StatusCode::InvalidArgument},
      {raw_bytes({0xFF}), 128, StatusCode::InvalidArgument},
      {"bang!", 128, StatusCode::InvalidArgument},
      {"quote\"", 128, StatusCode::InvalidArgument},
      {"hash#", 128, StatusCode::InvalidArgument},
      {"dollar$", 128, StatusCode::InvalidArgument},
      {"percent%", 128, StatusCode::InvalidArgument},
      {"amp&", 128, StatusCode::InvalidArgument},
      {"apostrophe'", 128, StatusCode::InvalidArgument},
      {"paren(", 128, StatusCode::InvalidArgument},
      {"paren)", 128, StatusCode::InvalidArgument},
      {"star*", 128, StatusCode::InvalidArgument},
      {"plus+", 128, StatusCode::InvalidArgument},
      {"comma,", 128, StatusCode::InvalidArgument},
      {"semi;", 128, StatusCode::InvalidArgument},
      {"less<", 128, StatusCode::InvalidArgument},
      {"equals=", 128, StatusCode::InvalidArgument},
      {"greater>", 128, StatusCode::InvalidArgument},
      {"question?", 128, StatusCode::InvalidArgument},
      {"bracket[", 128, StatusCode::InvalidArgument},
      {"bracket]", 128, StatusCode::InvalidArgument},
      {"caret^", 128, StatusCode::InvalidArgument},
      {raw_bytes({0x60}), 128, StatusCode::InvalidArgument},
      {"brace{", 128, StatusCode::InvalidArgument},
      {"pipe|", 128, StatusCode::InvalidArgument},
      {"brace}", 128, StatusCode::InvalidArgument},
      {"tilde~", 128, StatusCode::InvalidArgument},
  };
  for (const IdentifierCase& item : cases) {
    check_identifier_case(item);
  }
}

UC_TEST(types, identifier_length_bound_is_inclusive_and_checked_before_the_alphabet) {
  check_identifier_case({"abcd", 4, StatusCode::Ok});
  check_identifier_case({"abcde", 4, StatusCode::LimitExceeded});
  check_identifier_case({repeated('a', 128), 128, StatusCode::Ok});
  check_identifier_case({repeated('a', 129), 128, StatusCode::LimitExceeded});
  check_identifier_case({repeated('.', 1000), 1000, StatusCode::Ok});
  check_identifier_case({"", 0, StatusCode::InvalidArgument});
  check_identifier_case({"a", 0, StatusCode::LimitExceeded});
  // An over-long identifier is refused as over-long even when it also contains
  // an illegal character: the bound is applied before allocation, never after.
  check_identifier_case({"aaaa/", 4, StatusCode::LimitExceeded});
}

UC_TEST(types, label_accepts_valid_multibyte_utf8) {
  UC_CHECK(validate_label("", 128).ok());
  UC_CHECK(validate_label("UPS 1", 128).ok());
  UC_CHECK(validate_label("UPS \xE2\x80\x94 r\xC3\xA9sum\xC3\xA9", 128).ok());
  UC_CHECK(validate_label(raw_bytes({0xC2, 0x80}), 8).ok());             // U+0080
  UC_CHECK(validate_label(raw_bytes({0xC3, 0xA9}), 8).ok());             // U+00E9
  UC_CHECK(validate_label(raw_bytes({0xDF, 0xBF}), 8).ok());             // U+07FF
  UC_CHECK(validate_label(raw_bytes({0xE0, 0xA0, 0x80}), 8).ok());       // U+0800
  UC_CHECK(validate_label(raw_bytes({0xE2, 0x82, 0xAC}), 8).ok());       // U+20AC
  UC_CHECK(validate_label(raw_bytes({0xEF, 0xBF, 0xBF}), 8).ok());       // U+FFFF
  UC_CHECK(validate_label(raw_bytes({0xF0, 0x90, 0x80, 0x80}), 8).ok()); // U+10000
  UC_CHECK(validate_label(raw_bytes({0xF0, 0x9F, 0x94, 0x8B}), 8).ok()); // U+1F50B
  // U+10FFFF is the highest encodable code point and stays valid: the maximum
  // check must reject exactly one code point above it and nothing below it.
  UC_CHECK(validate_label(raw_bytes({0xF4, 0x8F, 0xBF, 0xBF}), 8).ok());
  const std::string max_code_point = raw_bytes({0xF4, 0x8F, 0xBF, 0xBF});
  UC_CHECK(validate_label(std::string("U+10FFFF ") + max_code_point, 16).ok());
}

UC_TEST(types, label_rejects_invalid_utf8_encodings) {
  const std::vector<std::string> invalid = {
      raw_bytes({0xC0, 0x80}),                      // overlong NUL
      raw_bytes({0xC1, 0xBF}),                      // overlong U+007F
      raw_bytes({0xE0, 0x80, 0xAF}),                // overlong U+002F
      raw_bytes({0xE0, 0x9F, 0xBF}),                // overlong U+07FF
      raw_bytes({0xF0, 0x80, 0x80, 0x80}),          // overlong U+0000
      raw_bytes({0xF0, 0x8F, 0xBF, 0xBF}),          // overlong U+FFFF
      raw_bytes({0xED, 0xA0, 0x80}),                // surrogate half U+D800
      raw_bytes({0xED, 0xBF, 0xBF}),                // surrogate half U+DFFF
      raw_bytes({0xC3}),                            // truncated 2-byte sequence
      raw_bytes({0xE2}),                            // truncated 3-byte sequence
      raw_bytes({0xE2, 0x82}),                      // truncated 3-byte sequence
      raw_bytes({0xF0}),                            // truncated 4-byte sequence
      raw_bytes({0xF0, 0x9F}),                      // truncated 4-byte sequence
      raw_bytes({0xF0, 0x9F, 0x94}),                // truncated 4-byte sequence
      raw_bytes({0xE2, 0x82, 0x41}),                // bad continuation byte
      raw_bytes({0xF0, 0x9F, 0x94, 0x41}),          // bad continuation byte
      raw_bytes({0xF4, 0x90, 0x80, 0x80}),          // U+110000, one above the maximum
      raw_bytes({0xF4, 0xBF, 0xBF, 0xBF}),          // U+13FFFF, the highest four-byte code point
      raw_bytes({0xF5, 0x80, 0x80, 0x80}),          // above U+10FFFF
      raw_bytes({0xF7, 0xBF, 0xBF, 0xBF}),          // above U+10FFFF
      raw_bytes({0xF8, 0x88, 0x80, 0x80, 0x80}),    // five-byte lead
      raw_bytes({0x80}),                            // bare continuation byte
      raw_bytes({0xBF}),                            // bare continuation byte
      raw_bytes({0x80, 0x80}),                      // bare continuation bytes
      raw_bytes({0xFE}),                            // never valid
      raw_bytes({0xFF}),                            // never valid
      std::string("ok") + raw_bytes({0xE2, 0x82}),  // truncated after a valid prefix
  };
  for (const std::string& text : invalid) {
    const Status status = validate_label(text, 128);
    UC_CHECK_MSG(!status.ok(),
                 "label of " + std::to_string(text.size()) + " bytes was accepted; it is not valid UTF-8");
    UC_CHECK_MSG(status.code() == StatusCode::InvalidArgument,
                 "invalid UTF-8 must be refused as invalid_argument, got " +
                     std::string(to_string(status.code())));
  }
}

UC_TEST(types, label_rejects_nul_c0_controls_and_del) {
  UC_CHECK(!validate_label(std::string("\0", 1), 8).ok());
  UC_CHECK(!validate_label(std::string("a\0b", 3), 8).ok());
  for (unsigned int value = 0; value < 0x20u; ++value) {
    const std::string text = byte_string(value);
    const Status status = validate_label(text, 8);
    UC_CHECK_MSG(!status.ok(), "C0 control byte " + std::to_string(value) + " was accepted");
    UC_CHECK_MSG(status.code() == StatusCode::InvalidArgument,
                 "C0 control byte " + std::to_string(value) + " must be invalid_argument, got " +
                     std::string(to_string(status.code())));
  }
  UC_CHECK(!validate_label(byte_string(0x7Fu), 8).ok());
  UC_CHECK(!validate_label("line\nbreak", 16).ok());
  UC_CHECK(!validate_label("tab\there", 16).ok());
}

UC_TEST(types, label_length_bound_is_inclusive) {
  UC_CHECK(validate_label(repeated('a', 8), 8).ok());
  UC_CHECK(validate_label(repeated('a', 9), 8).code() == StatusCode::LimitExceeded);
  UC_CHECK(validate_label(repeated('a', 256), 256).ok());
  UC_CHECK(validate_label(repeated('a', 257), 256).code() == StatusCode::LimitExceeded);
  UC_CHECK(validate_label(std::string(), 0).ok());
  // The byte length, not the code point count, is bounded.
  const std::string four_byte_code_point = raw_bytes({0xF0, 0x9F, 0x94, 0x8B});
  UC_CHECK(validate_label(four_byte_code_point, 4).ok());
  UC_CHECK(validate_label(four_byte_code_point, 3).code() == StatusCode::LimitExceeded);
}

UC_TEST(types, basic_id_parse_round_trips_and_orders) {
  const UpsId ups = UC_REQUIRE_OK(UpsId::parse("ups-under-test"));
  UC_CHECK_EQ(ups.value(), std::string("ups-under-test"));
  UC_CHECK_EQ(ups.view(), std::string_view("ups-under-test"));
  UC_CHECK(!ups.empty());
  UC_CHECK(UpsId{}.empty());
  UC_CHECK_EQ(UpsId{}.value(), std::string(""));

  const UpsId same = UC_REQUIRE_OK(UpsId::parse("ups-under-test"));
  UC_CHECK_EQ(ups, same);
  const UpsId other = UC_REQUIRE_OK(UpsId::parse("ups-under-test-2"));
  UC_CHECK_NE(ups, other);
  UC_CHECK(ups < other);
  UC_CHECK(other > ups);

  UC_REQUIRE_STATUS(UpsId::parse("bad/id"), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsId::parse(""), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(UpsId::parse("short", 4), StatusCode::LimitExceeded);
  UC_CHECK(UpsId::parse(repeated('x', 128)).ok());
  UC_REQUIRE_STATUS(UpsId::parse(repeated('x', 129)), StatusCode::LimitExceeded);
}

UC_TEST(types, identifier_families_are_distinct_types) {
  static_assert(std::is_same_v<UpsId, BasicId<IdFamily::Ups>>);
  static_assert(!std::is_same_v<UpsId, LoadId>);
  static_assert(!std::is_convertible_v<UpsId, LoadId>);
  static_assert(!std::is_convertible_v<LoadId, UpsId>);
  static_assert(!std::is_convertible_v<EvidenceId, SourceId>);
  static_assert(!std::is_convertible_v<SourceId, EvidenceId>);
  static_assert(!std::is_convertible_v<AuthorityRef, ObligationRef>);
  static_assert(!std::is_convertible_v<IdempotencyKey, AdapterId>);
  static_assert(!std::is_convertible_v<std::string, UpsId>);
  static_assert(!std::is_convertible_v<UpsId, std::string>);
  static_assert(!std::is_assignable_v<LoadId&, UpsId>);
  static_assert(!std::is_assignable_v<UpsId&, LoadId>);
  static_assert(!is_equality_comparable<UpsId, LoadId>::value);
  static_assert(!is_less_comparable<UpsId, LoadId>::value);
  static_assert(is_equality_comparable<UpsId, UpsId>::value);

  // The same text parsed into two families is not interchangeable at compile
  // time even though the stored bytes are equal.
  const UpsId ups = UC_REQUIRE_OK(UpsId::parse("shared-text"));
  const LoadId load = UC_REQUIRE_OK(LoadId::parse("shared-text"));
  UC_CHECK_EQ(ups.value(), load.value());
  UC_CHECK_EQ(ups.view(), load.view());
}

UC_TEST(types, fingerprint_generate_is_never_zero) {
  for (int iteration = 0; iteration < 64; ++iteration) {
    const StoreIdentity store = StoreIdentity::generate();
    UC_CHECK_MSG(!store.is_zero(), "store identity generate() returned the all-zero fingerprint");
    const SessionId session = SessionId::generate();
    UC_CHECK_MSG(!session.is_zero(), "session id generate() returned the all-zero fingerprint");
    UC_CHECK_EQ(store.to_hex().size(), std::size_t{32});
    UC_CHECK_EQ(store.to_hex(), UC_REQUIRE_OK(StoreIdentity::parse(store.to_hex())).to_hex());
  }
}

UC_TEST(types, fingerprint_parse_accepts_exactly_32_lowercase_hex_characters) {
  const StoreIdentity parsed = UC_REQUIRE_OK(StoreIdentity::parse("0123456789abcdeffedcba9876543210"));
  UC_CHECK_EQ(parsed.high(), std::uint64_t{0x0123456789abcdefull});
  UC_CHECK_EQ(parsed.low(), std::uint64_t{0xfedcba9876543210ull});
  UC_CHECK(!parsed.is_zero());

  const std::vector<std::string> rejected = {
      "",
      "0",
      "0123456789abcdeffedcba987654321",                                    // 31 characters
      "0123456789abcdeffedcba98765432100",                                  // 33 characters
      "0123456789abcdeffedcba98765432100123456789abcdeffedcba9876543210",   // 64 characters
      "0123456789ABCDEFFEDCBA9876543210",                                   // uppercase
      "0123456789Abcdeffedcba9876543210",                                   // mixed case
      "g123456789abcdeffedcba9876543210",                                   // non-hex at the front
      "0123456789abcdeffedcba987654321g",                                   // non-hex at the back
      "0123456789abcdeffedcba98 76543210",                                  // embedded space
      "0123456789abcdeffedcba98-76543210",                                  // punctuation
      "0123456789abcdeffedcba98x6543210",                                   // letter outside a-f
      "00000000000000000000000000000000",  // zero means unset, never a valid identity
  };
  for (const std::string& text : rejected) {
    const Status status = uc_test::status_of(StoreIdentity::parse(text));
    UC_CHECK_MSG(!status.ok(), "fingerprint '" + text + "' was accepted");
    UC_CHECK_MSG(status.code() == StatusCode::InvalidArgument,
                 "fingerprint '" + text + "' must be refused as invalid_argument, got " +
                     std::string(to_string(status.code())));
  }
}

UC_TEST(types, fingerprint_from_components_refuses_zero_and_round_trips_through_hex) {
  UC_REQUIRE_STATUS(StoreIdentity::from_components(0, 0), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(SessionId::from_components(0, 0), StatusCode::InvalidArgument);

  const StoreIdentity high_only = UC_REQUIRE_OK(StoreIdentity::from_components(1, 0));
  UC_CHECK(!high_only.is_zero());
  UC_CHECK_EQ(high_only.to_hex(), std::string("00000000000000010000000000000000"));
  const StoreIdentity low_only = UC_REQUIRE_OK(StoreIdentity::from_components(0, 1));
  UC_CHECK(!low_only.is_zero());
  UC_CHECK_EQ(low_only.to_hex(), std::string("00000000000000000000000000000001"));

  const SessionId session = UC_REQUIRE_OK(
      SessionId::from_components(std::uint64_t{0xdeadbeefcafebabeull},
                                 std::uint64_t{0x0f1e2d3c4b5a6978ull}));
  UC_CHECK_EQ(session.to_hex(), std::string("deadbeefcafebabe0f1e2d3c4b5a6978"));
  const SessionId reparsed = UC_REQUIRE_OK(SessionId::parse(session.to_hex()));
  UC_CHECK_EQ(reparsed, session);
  UC_CHECK_EQ(reparsed.high(), session.high());
  UC_CHECK_EQ(reparsed.low(), session.low());
  UC_CHECK_EQ(session.to_hex().size(), std::size_t{32});
}

UC_TEST(types, reserve_quantity_refuses_negative_values_and_per_unit_bounds) {
  ResourceLimits limits;
  limits.max_reserve_milliwatt_hours = 1'000'000;
  limits.max_reserve_seconds = 500;

  struct BoundCase {
    ReserveUnit unit;
    std::int64_t at_limit;
    std::int64_t over_limit;
    StatusCode over_status;
  };
  const std::vector<BoundCase> cases = {
      {ReserveUnit::MilliwattHours, 1'000'000, 1'000'001, StatusCode::LimitExceeded},
      {ReserveUnit::WattHours, 1'000, 1'001, StatusCode::LimitExceeded},
      {ReserveUnit::KilowattHours, 1, 2, StatusCode::LimitExceeded},
      {ReserveUnit::Seconds, 500, 501, StatusCode::LimitExceeded},
      {ReserveUnit::BasisPoints, 10'000, 10'001, StatusCode::InvalidArgument},
  };
  for (const BoundCase& item : cases) {
    const Status at = uc_test::status_of(make_reserve_quantity(item.unit, item.at_limit, limits));
    UC_CHECK_MSG(at.ok(), std::string("the configured bound of ") + to_string(item.unit) +
                              " must itself be accepted: " + at.message());
    const Status over = uc_test::status_of(make_reserve_quantity(item.unit, item.over_limit, limits));
    UC_CHECK_MSG(over.code() == item.over_status,
                 std::string("one over the bound of ") + to_string(item.unit) + " returned " +
                     to_string(over.code()) + ", expected " + to_string(item.over_status));
    const Status negative = uc_test::status_of(make_reserve_quantity(item.unit, -1, limits));
    UC_CHECK_MSG(negative.code() == StatusCode::InvalidArgument,
                 std::string("a negative quantity in ") + to_string(item.unit) + " returned " +
                     to_string(negative.code()) + ", expected invalid_argument");
    const Status minimum = uc_test::status_of(make_reserve_quantity(item.unit, kInt64Min, limits));
    UC_CHECK_MSG(minimum.code() == StatusCode::InvalidArgument,
                 std::string("INT64_MIN in ") + to_string(item.unit) + " returned " +
                     to_string(minimum.code()) + ", expected invalid_argument");
  }

  // The zero quantity is a legal known value in every unit; zero is a value,
  // not a stand-in for unknown.
  for (const BoundCase& item : cases) {
    UC_CHECK(uc_test::status_of(make_reserve_quantity(item.unit, 0, limits)).ok());
  }

  const ReserveQuantity hand_built{ReserveUnit::MilliwattHours, -5};
  UC_REQUIRE_STATUS(validate_reserve_quantity(hand_built, limits), StatusCode::InvalidArgument);
  const ReserveQuantity too_large{ReserveUnit::MilliwattHours, 1'000'001};
  UC_REQUIRE_STATUS(validate_reserve_quantity(too_large, limits), StatusCode::LimitExceeded);
  // With the default limits the documented magnitudes are accepted.
  UC_CHECK(uc_test::status_of(make_reserve_quantity(ReserveUnit::MilliwattHours, 1'000'000'000'000'000)).ok());
  UC_REQUIRE_STATUS(make_reserve_quantity(ReserveUnit::MilliwattHours, 1'000'000'000'000'001),
                    StatusCode::LimitExceeded);
  UC_CHECK(uc_test::status_of(make_reserve_quantity(ReserveUnit::Seconds, 1'000'000'000'000)).ok());
  UC_REQUIRE_STATUS(make_reserve_quantity(ReserveUnit::Seconds, 1'000'000'000'001),
                    StatusCode::LimitExceeded);
}

UC_TEST(types, reserve_units_are_comparable_exactly_within_their_class) {
  const std::vector<ReserveUnit> units = {ReserveUnit::MilliwattHours, ReserveUnit::WattHours,
                                          ReserveUnit::KilowattHours, ReserveUnit::Seconds,
                                          ReserveUnit::BasisPoints};
  for (const ReserveUnit left : units) {
    for (const ReserveUnit right : units) {
      const bool comparable = reserve_units_comparable(left, right);
      const bool expected = reserve_class(left) == reserve_class(right);
      UC_CHECK_MSG(comparable == expected, std::string("reserve_units_comparable(") +
                                               to_string(left) + ", " + to_string(right) +
                                               ") disagreed with reserve_class");
      const Result<int> comparison =
          compare_reserve(ReserveQuantity{left, 1}, ReserveQuantity{right, 1});
      UC_CHECK_MSG(comparison.ok() == expected,
                   std::string("compare_reserve across ") + to_string(left) + "/" +
                       to_string(right) + " returned " + to_string(comparison.status().code()));
      const Result<bool> at_least =
          reserve_at_least(ReserveQuantity{left, 1}, ReserveQuantity{right, 1});
      UC_CHECK_MSG(at_least.ok() == expected,
                   std::string("reserve_at_least across ") + to_string(left) + "/" +
                       to_string(right) + " returned " + to_string(at_least.status().code()));
      if (expected) {
        // The two quantities are only equal when they are the same unit with the
        // same value; two different units of one class are ordered, and the
        // ordering itself is proven separately.
        if (left == right) {
          UC_CHECK_EQ(comparison.value(), 0);
          UC_CHECK(at_least.value());
        }
        UC_CHECK_MSG(at_least.value() == (comparison.value() >= 0),
                     "reserve_at_least disagreed with compare_reserve for one class");
      }
    }
  }
  UC_CHECK(reserve_class(ReserveUnit::MilliwattHours) == ReserveClass::Energy);
  UC_CHECK(reserve_class(ReserveUnit::WattHours) == ReserveClass::Energy);
  UC_CHECK(reserve_class(ReserveUnit::KilowattHours) == ReserveClass::Energy);
  UC_CHECK(reserve_class(ReserveUnit::Seconds) == ReserveClass::Runtime);
  UC_CHECK(reserve_class(ReserveUnit::BasisPoints) == ReserveClass::NameplateFraction);
}

UC_TEST(types, convert_reserve_energy_conversions_are_exact_in_both_directions) {
  const ReserveQuantity one_kwh{ReserveUnit::KilowattHours, 1};
  const ReserveQuantity as_wh = UC_REQUIRE_OK(convert_reserve(one_kwh, ReserveUnit::WattHours));
  UC_CHECK_EQ(as_wh, (ReserveQuantity{ReserveUnit::WattHours, 1'000}));
  const ReserveQuantity as_mwh =
      UC_REQUIRE_OK(convert_reserve(one_kwh, ReserveUnit::MilliwattHours));
  UC_CHECK_EQ(as_mwh, (ReserveQuantity{ReserveUnit::MilliwattHours, 1'000'000}));

  const ReserveQuantity five_wh{ReserveUnit::WattHours, 5};
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(five_wh, ReserveUnit::MilliwattHours)),
              (ReserveQuantity{ReserveUnit::MilliwattHours, 5'000}));
  // 5 Wh is 0.005 kWh, which an integer number of kilowatt-hours cannot
  // represent, so the narrowing is refused rather than truncated to zero.
  UC_REQUIRE_STATUS(convert_reserve(five_wh, ReserveUnit::KilowattHours),
                    StatusCode::Unsupported);
  // A whole number of kilowatt-hours does narrow exactly.
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(ReserveQuantity{ReserveUnit::WattHours, 2'000},
                                            ReserveUnit::KilowattHours)),
              (ReserveQuantity{ReserveUnit::KilowattHours, 2}));
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(as_mwh, ReserveUnit::WattHours)),
              (ReserveQuantity{ReserveUnit::WattHours, 1'000}));
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(as_mwh, ReserveUnit::KilowattHours)), one_kwh);
  // A conversion to the same unit is the identity, and is never refused.
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(one_kwh, ReserveUnit::KilowattHours)), one_kwh);
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(ReserveQuantity{ReserveUnit::Seconds, 37},
                                            ReserveUnit::Seconds)),
              (ReserveQuantity{ReserveUnit::Seconds, 37}));
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 5'000},
                                            ReserveUnit::BasisPoints)),
              (ReserveQuantity{ReserveUnit::BasisPoints, 5'000}));
}

UC_TEST(types, convert_reserve_refuses_lossy_narrowing_and_cross_class_conversion) {
  // Exactness: 1500 mWh is 1.5 Wh, so a narrowing to Wh would lose 500 mWh.
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::MilliwattHours, 1'500},
                                    ReserveUnit::WattHours),
                    StatusCode::Unsupported);
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::MilliwattHours, 999},
                                    ReserveUnit::KilowattHours),
                    StatusCode::Unsupported);
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::WattHours, 999},
                                    ReserveUnit::KilowattHours),
                    StatusCode::Unsupported);
  UC_CHECK(convert_reserve(ReserveQuantity{ReserveUnit::WattHours, 1},
                           ReserveUnit::MilliwattHours)
               .ok());
  // Cross-class conversion is never performed, in either direction.
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::Seconds, 600},
                                    ReserveUnit::MilliwattHours),
                    StatusCode::Unsupported);
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::KilowattHours, 1},
                                    ReserveUnit::Seconds),
                    StatusCode::Unsupported);
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 5'000},
                                    ReserveUnit::MilliwattHours),
                    StatusCode::Unsupported);
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::Seconds, 600},
                                    ReserveUnit::BasisPoints),
                    StatusCode::Unsupported);
  // An invalid source quantity is refused before any conversion is attempted.
  UC_REQUIRE_STATUS(convert_reserve(ReserveQuantity{ReserveUnit::BasisPoints, 10'001},
                                    ReserveUnit::MilliwattHours),
                    StatusCode::InvalidArgument);
}

UC_TEST(types, compare_reserve_and_reserve_at_least_agree) {
  struct ComparisonCase {
    ReserveQuantity observed;
    ReserveQuantity required;
    int expected;
  };
  const std::vector<ComparisonCase> cases = {
      {{ReserveUnit::MilliwattHours, 1'000}, {ReserveUnit::WattHours, 1}, 0},
      {{ReserveUnit::WattHours, 1}, {ReserveUnit::MilliwattHours, 1'000}, 0},
      {{ReserveUnit::KilowattHours, 1}, {ReserveUnit::WattHours, 999}, 1},
      {{ReserveUnit::WattHours, 999}, {ReserveUnit::KilowattHours, 1}, -1},
      {{ReserveUnit::WattHours, 1}, {ReserveUnit::MilliwattHours, 999}, 1},
      {{ReserveUnit::WattHours, 1}, {ReserveUnit::MilliwattHours, 1'001}, -1},
      {{ReserveUnit::Seconds, 600}, {ReserveUnit::Seconds, 600}, 0},
      {{ReserveUnit::Seconds, 601}, {ReserveUnit::Seconds, 600}, 1},
      {{ReserveUnit::BasisPoints, 5'000}, {ReserveUnit::BasisPoints, 9'000}, -1},
      {{ReserveUnit::BasisPoints, 10'000}, {ReserveUnit::BasisPoints, 10'000}, 0},
      {{ReserveUnit::MilliwattHours, 0}, {ReserveUnit::MilliwattHours, 0}, 0},
      {{ReserveUnit::MilliwattHours, 0}, {ReserveUnit::MilliwattHours, 1}, -1},
  };
  for (const ComparisonCase& item : cases) {
    const int comparison = UC_REQUIRE_OK(compare_reserve(item.observed, item.required));
    UC_CHECK_MSG(comparison == item.expected,
                 "compare_reserve(" + format_reserve(item.observed) + ", " +
                     format_reserve(item.required) + ") returned " + std::to_string(comparison) +
                     ", expected " + std::to_string(item.expected));
    const bool at_least = UC_REQUIRE_OK(reserve_at_least(item.observed, item.required));
    UC_CHECK_MSG(at_least == (item.expected >= 0),
                 "reserve_at_least(" + format_reserve(item.observed) + ", " +
                     format_reserve(item.required) + ") returned " + (at_least ? "true" : "false"));
    const bool reverse = UC_REQUIRE_OK(reserve_at_least(item.required, item.observed));
    UC_CHECK_MSG(reverse == (item.expected <= 0),
                 "reserve_at_least in the reverse direction disagreed with compare_reserve");
    const int reverse_comparison = UC_REQUIRE_OK(compare_reserve(item.required, item.observed));
    UC_CHECK_EQ(reverse_comparison, -item.expected);
  }
}

UC_TEST(types, reserve_at_least_returns_an_error_for_incomparable_units) {
  const ReserveQuantity seconds{ReserveUnit::Seconds, 600};
  const ReserveQuantity energy{ReserveUnit::KilowattHours, 1};
  const ReserveQuantity basis_points{ReserveUnit::BasisPoints, 5'000};

  UC_REQUIRE_STATUS(compare_reserve(seconds, energy), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(reserve_at_least(seconds, energy), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(reserve_at_least(energy, seconds), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(reserve_at_least(seconds, basis_points), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(reserve_at_least(basis_points, energy), StatusCode::Unsupported);

  // A caller may not read the error as "satisfied": the only way to obtain a
  // value is to observe that the result is not ok.
  const Result<bool> indeterminate = reserve_at_least(seconds, energy);
  UC_CHECK(!indeterminate.ok());
  UC_CHECK(!indeterminate.status().ok());
  UC_CHECK(indeterminate.status().code() == StatusCode::Unsupported);
  UC_CHECK(indeterminate.status().message().find("not comparable") != std::string::npos);
}

UC_TEST(types, power_quantities_convert_exactly_and_compare_across_units) {
  UC_CHECK(uc_test::status_of(make_power_quantity(PowerUnit::Milliwatts, 0)).ok());
  UC_REQUIRE_STATUS(make_power_quantity(PowerUnit::Watts, -1), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(make_power_quantity(PowerUnit::Megawatts, kInt64Min),
                    StatusCode::InvalidArgument);

  const PowerQuantity one_kw{PowerUnit::Kilowatts, 1};
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_power(one_kw, PowerUnit::Watts)),
              (PowerQuantity{PowerUnit::Watts, 1'000}));
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_power(one_kw, PowerUnit::Milliwatts)),
              (PowerQuantity{PowerUnit::Milliwatts, 1'000'000}));
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_power(PowerQuantity{PowerUnit::Megawatts, 1},
                                          PowerUnit::Kilowatts)),
              (PowerQuantity{PowerUnit::Kilowatts, 1'000}));
  UC_CHECK_EQ(UC_REQUIRE_OK(convert_power(PowerQuantity{PowerUnit::Megawatts, 1},
                                          PowerUnit::Milliwatts)),
              (PowerQuantity{PowerUnit::Milliwatts, 1'000'000'000}));
  // A lossy narrowing is refused rather than rounded.
  UC_REQUIRE_STATUS(convert_power(PowerQuantity{PowerUnit::Watts, 1'500}, PowerUnit::Kilowatts),
                    StatusCode::Unsupported);
  UC_REQUIRE_STATUS(convert_power(PowerQuantity{PowerUnit::Milliwatts, 999}, PowerUnit::Watts),
                    StatusCode::Unsupported);
  // The configured bound applies to power as well.
  UC_REQUIRE_STATUS(make_power_quantity(PowerUnit::Megawatts, 10'000'000),
                    StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(make_power_quantity(PowerUnit::Milliwatts, 1'000'000'000'000'001),
                    StatusCode::LimitExceeded);

  UC_CHECK_EQ(UC_REQUIRE_OK(compare_power(one_kw, PowerQuantity{PowerUnit::Watts, 1'000})), 0);
  UC_CHECK_EQ(UC_REQUIRE_OK(compare_power(PowerQuantity{PowerUnit::Watts, 999}, one_kw)), -1);
  UC_CHECK_EQ(UC_REQUIRE_OK(compare_power(PowerQuantity{PowerUnit::Megawatts, 1},
                                          PowerQuantity{PowerUnit::Kilowatts, 999})),
              1);
  UC_CHECK_EQ(UC_REQUIRE_OK(compare_power(PowerQuantity{PowerUnit::Milliwatts, 1'000},
                                          PowerQuantity{PowerUnit::Watts, 1})),
              0);
  UC_CHECK_EQ(UC_REQUIRE_OK(compare_power(PowerQuantity{PowerUnit::Watts, 0},
                                          PowerQuantity{PowerUnit::Megawatts, 0})),
              0);
}

UC_TEST(types, checked_add_refuses_overflow_and_underflow) {
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(0, 0)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(kInt64Max, 0)), kInt64Max);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(0, kInt64Max)), kInt64Max);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(kInt64Min, 0)), kInt64Min);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(kInt64Max, kInt64Min)), std::int64_t{-1});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(kInt64Min, kInt64Max)), std::int64_t{-1});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_add(-1, 1)), std::int64_t{0});

  UC_REQUIRE_STATUS(checked_add(kInt64Max, 1), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_add(1, kInt64Max), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_add(kInt64Max, kInt64Max), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_add(kInt64Min, -1), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_add(-1, kInt64Min), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_add(kInt64Min, kInt64Min), StatusCode::LimitExceeded);
}

UC_TEST(types, checked_sub_refuses_overflow_and_underflow) {
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(kInt64Max, 0)), kInt64Max);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(kInt64Max, kInt64Max)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(kInt64Min, 0)), kInt64Min);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(0, kInt64Max)), kInt64Min + 1);
  // INT64_MIN - INT64_MIN is the only representable subtraction whose right
  // operand is INT64_MIN, and it must not be refused as an overflow.
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(kInt64Min, kInt64Min)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(-1, -1)), std::int64_t{0});

  UC_REQUIRE_STATUS(checked_sub(kInt64Min, 1), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_sub(kInt64Max, -1), StatusCode::LimitExceeded);
  // -INT64_MIN cannot be represented, so subtracting it is refused explicitly.
  UC_REQUIRE_STATUS(checked_sub(0, kInt64Min), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_sub(1, kInt64Min), StatusCode::LimitExceeded);
  // The nearest representable subtraction of a value one above INT64_MIN is
  // exact: INT64_MIN - (INT64_MIN + 1) == -1.
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_sub(kInt64Min, kInt64Min + 1)), std::int64_t{-1});
  // Every other subtraction of INT64_MIN overflows, in both directions.
  UC_REQUIRE_STATUS(checked_sub(kInt64Min + 1, kInt64Min), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_sub(-1, kInt64Min), StatusCode::LimitExceeded);
}

UC_TEST(types, checked_mul_refuses_overflow_at_the_boundaries) {
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(0, kInt64Min)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(kInt64Min, 0)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(0, kInt64Max)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(kInt64Max, 0)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(kInt64Max, 1)), kInt64Max);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(1, kInt64Max)), kInt64Max);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(kInt64Min, 1)), kInt64Min);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(-1, -1)), std::int64_t{1});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_mul(1'000, 1'000)), std::int64_t{1'000'000});

  // -1 * INT64_MIN is the one product whose mathematical value is not
  // representable in either order, and it is refused explicitly.
  UC_REQUIRE_STATUS(checked_mul(-1, kInt64Min), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_mul(kInt64Min, -1), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_mul(kInt64Max, 2), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_mul(2, kInt64Max), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_mul(2, kInt64Min), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_mul(kInt64Max, kInt64Max), StatusCode::LimitExceeded);
  UC_REQUIRE_STATUS(checked_mul(-2, kInt64Max), StatusCode::LimitExceeded);
}

UC_TEST(types, checked_div_exact_refuses_a_zero_divisor_and_a_remainder) {
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_div_exact(0, 5)), std::int64_t{0});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_div_exact(6, 3)), std::int64_t{2});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_div_exact(-6, 3)), std::int64_t{-2});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_div_exact(6, -3)), std::int64_t{-2});
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_div_exact(kInt64Max, 1)), kInt64Max);
  UC_CHECK_EQ(UC_REQUIRE_OK(checked_div_exact(kInt64Max, kInt64Max)), std::int64_t{1});

  UC_REQUIRE_STATUS(checked_div_exact(7, 2), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(checked_div_exact(-7, 2), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(checked_div_exact(1, kInt64Max), StatusCode::Unsupported);
  UC_REQUIRE_STATUS(checked_div_exact(0, 0), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(checked_div_exact(1, 0), StatusCode::InvalidArgument);
  UC_REQUIRE_STATUS(checked_div_exact(kInt64Max, 0), StatusCode::InvalidArgument);
}

UC_TEST(types, ordinals_are_distinct_types_and_are_ordered) {
  static_assert(!std::is_convertible_v<StoreGeneration, ControlEpoch>);
  static_assert(!std::is_convertible_v<ControlEpoch, StoreGeneration>);
  static_assert(!std::is_convertible_v<ControlEpoch, Incarnation>);
  static_assert(!std::is_convertible_v<Incarnation, ControlEpoch>);
  static_assert(!std::is_convertible_v<StateRevision, SourceRevision>);
  static_assert(!std::is_convertible_v<SourceRevision, ObligationRevision>);
  static_assert(!std::is_convertible_v<GrantRevision, AttemptId>);
  static_assert(!std::is_convertible_v<Tick, TickSpan>);
  static_assert(!std::is_convertible_v<TickSpan, Tick>);
  static_assert(!std::is_convertible_v<HardwareGeneration, StateRevision>);
  static_assert(!std::is_same_v<StoreGeneration, ControlEpoch>);
  static_assert(!is_equality_comparable<StoreGeneration, ControlEpoch>::value);
  static_assert(!is_equality_comparable<ControlEpoch, StoreGeneration>::value);
  static_assert(!is_less_comparable<StateRevision, SourceRevision>::value);
  static_assert(!is_less_comparable<SourceRevision, StateRevision>::value);
  static_assert(!std::is_assignable_v<ControlEpoch&, StoreGeneration>);
  static_assert(!std::is_assignable_v<StoreGeneration&, ControlEpoch>);
  // is_valid_instant takes exactly Tick; no other ordinal is an instant.
  static_assert(!accepts_instant<TickSpan>::value);
  static_assert(!accepts_instant<HardwareGeneration>::value);
  static_assert(!accepts_instant<StoreGeneration>::value);
  static_assert(!accepts_instant<ControlEpoch>::value);
  static_assert(accepts_instant<Tick>::value);
  static_assert(is_equality_comparable<StoreGeneration, StoreGeneration>::value);
  static_assert(is_less_comparable<StoreGeneration, StoreGeneration>::value);

  const StoreGeneration first{1};
  const StoreGeneration second{2};
  UC_CHECK(!first.is_zero());
  UC_CHECK(StoreGeneration{}.is_zero());
  UC_CHECK(first == StoreGeneration{1});
  UC_CHECK_NE(first, second);
  UC_CHECK(first < second);
  UC_CHECK(second > first);
  UC_CHECK(first <= first);
  UC_CHECK_EQ(second - first, std::uint64_t{1});
  UC_CHECK_EQ((first + std::uint64_t{4}).value(), std::uint64_t{5});
  UC_CHECK_EQ((second - std::uint64_t{1}).value(), std::uint64_t{1});
  StoreGeneration mutated{7};
  mutated += std::uint64_t{3};
  UC_CHECK_EQ(mutated.value(), std::uint64_t{10});

  const Tick tick{10};
  UC_CHECK_EQ((tick - Tick{4}), std::int64_t{6});
  UC_CHECK_EQ((tick + std::int64_t{5}).value(), std::int64_t{15});
  const HardwareGeneration hardware{3};
  UC_CHECK_EQ(hardware.value(), std::uint32_t{3});

  const ControlContext context{ControlEpoch{4}, Incarnation{5}};
  const ControlContext same{ControlEpoch{4}, Incarnation{5}};
  const ControlContext other{ControlEpoch{4}, Incarnation{6}};
  UC_CHECK(context == same);
  UC_CHECK_NE(context, other);
}

UC_TEST(types, is_valid_instant_is_true_only_for_strictly_positive_ticks) {
  UC_CHECK(!is_valid_instant(Tick{0}));
  UC_CHECK(!is_valid_instant(Tick{-1}));
  UC_CHECK(!is_valid_instant(Tick{kInt64Min}));
  UC_CHECK(is_valid_instant(Tick{1}));
  UC_CHECK(is_valid_instant(Tick{2}));
  UC_CHECK(is_valid_instant(Tick{kInt64Max}));
  // The reserved origin is exactly zero, not "any small value".
  for (std::int64_t value = -8; value <= 8; ++value) {
    UC_CHECK_MSG(is_valid_instant(Tick{value}) == (value > 0),
                 "is_valid_instant disagreed for tick " + std::to_string(value));
  }
}

UC_TEST(types, unit_tokens_round_trip_and_refuse_unknown_tokens) {
  const std::vector<ReserveUnit> reserve_units = {
      ReserveUnit::MilliwattHours, ReserveUnit::WattHours, ReserveUnit::KilowattHours,
      ReserveUnit::Seconds,        ReserveUnit::BasisPoints};
  for (const ReserveUnit unit : reserve_units) {
    const std::string token = to_string(unit);
    UC_CHECK_MSG(!token.empty() && token != "unknown", "no stable token for a reserve unit");
    const ReserveUnit parsed = UC_REQUIRE_OK(parse_reserve_unit(token));
    UC_CHECK_MSG(parsed == unit, "reserve unit token '" + token + "' did not round-trip");
  }
  const std::vector<std::string> unknown_reserve_tokens = {"",     "MWH",     "mWh", "wh ",
                                                           " mwh", "hours",   "percent", "mwhh"};
  for (const std::string& token : unknown_reserve_tokens) {
    UC_CHECK_MSG(!parse_reserve_unit(token).ok(),
                 "parse_reserve_unit accepted the unknown token '" + token + "'");
  }

  const std::vector<PowerUnit> power_units = {PowerUnit::Milliwatts, PowerUnit::Watts,
                                              PowerUnit::Kilowatts, PowerUnit::Megawatts};
  for (const PowerUnit unit : power_units) {
    const std::string token = to_string(unit);
    UC_CHECK_MSG(!token.empty() && token != "unknown", "no stable token for a power unit");
    const PowerUnit parsed = UC_REQUIRE_OK(parse_power_unit(token));
    UC_CHECK_MSG(parsed == unit, "power unit token '" + token + "' did not round-trip");
  }
  const std::vector<std::string> unknown_power_tokens = {"", "MW", "milliwatts", "w ", "megawatt"};
  for (const std::string& token : unknown_power_tokens) {
    UC_CHECK_MSG(!parse_power_unit(token).ok(),
                 "parse_power_unit accepted the unknown token '" + token + "'");
  }

  UC_CHECK_EQ(format_reserve(ReserveQuantity{ReserveUnit::WattHours, 12}), std::string("12 wh"));
  UC_CHECK_EQ(format_power(PowerQuantity{PowerUnit::Kilowatts, 3}), std::string("3 kw"));
}
