#include "ups_control/units.hpp"

#include <string>

namespace ups_control {
namespace {

Result<std::int64_t> scale_up(std::int64_t value, std::int64_t factor) {
  return checked_mul(value, factor);
}

Status unit_mismatch(ReserveUnit left, ReserveUnit right) {
  return Status::error(StatusCode::Unsupported,
                       std::string("reserve units ") + to_string(left) + " and " + to_string(right) +
                           " are not comparable; this runtime does not convert across classes");
}

constexpr std::int64_t kThousand = 1000;
constexpr std::int64_t kMillion = 1000 * 1000;
constexpr std::int64_t kBasisPointMaximum = 10000;

}  // namespace

const char* to_string(ReserveUnit unit) noexcept {
  switch (unit) {
    case ReserveUnit::MilliwattHours: return "mwh";
    case ReserveUnit::WattHours: return "wh";
    case ReserveUnit::KilowattHours: return "kwh";
    case ReserveUnit::Seconds: return "s";
    case ReserveUnit::BasisPoints: return "bp";
  }
  return "unknown";
}

Result<ReserveUnit> parse_reserve_unit(std::string_view text) {
  if (text == "mwh") {
    return ReserveUnit::MilliwattHours;
  }
  if (text == "wh") {
    return ReserveUnit::WattHours;
  }
  if (text == "kwh") {
    return ReserveUnit::KilowattHours;
  }
  if (text == "s") {
    return ReserveUnit::Seconds;
  }
  if (text == "bp") {
    return ReserveUnit::BasisPoints;
  }
  return Status::error(StatusCode::InvalidArgument,
                       "unknown reserve unit '" + std::string(text) +
                           "'; accepted units are mwh, wh, kwh, s, bp");
}

ReserveClass reserve_class(ReserveUnit unit) noexcept {
  switch (unit) {
    case ReserveUnit::MilliwattHours:
    case ReserveUnit::WattHours:
    case ReserveUnit::KilowattHours:
      return ReserveClass::Energy;
    case ReserveUnit::Seconds:
      return ReserveClass::Runtime;
    case ReserveUnit::BasisPoints:
      return ReserveClass::NameplateFraction;
  }
  return ReserveClass::Energy;
}

Result<ReserveQuantity> make_reserve_quantity(ReserveUnit unit, std::int64_t value,
                                              const ResourceLimits& limits) {
  const ReserveQuantity quantity{unit, value};
  const Status status = validate_reserve_quantity(quantity, limits);
  if (!status.ok()) {
    return status;
  }
  return quantity;
}

Status validate_reserve_quantity(const ReserveQuantity& quantity, const ResourceLimits& limits) {
  if (quantity.value < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string("reserve quantity in ") + to_string(quantity.unit) +
                             " must not be negative, got " + std::to_string(quantity.value));
  }
  switch (quantity.unit) {
    case ReserveUnit::MilliwattHours:
      if (quantity.value > limits.max_reserve_milliwatt_hours) {
        return Status::error(StatusCode::LimitExceeded,
                             "reserve of " + std::to_string(quantity.value) +
                                 " mWh exceeds the configured maximum of " +
                                 std::to_string(limits.max_reserve_milliwatt_hours) + " mWh");
      }
      return Status::success();
    case ReserveUnit::WattHours:
      if (quantity.value > limits.max_reserve_milliwatt_hours / kThousand) {
        return Status::error(StatusCode::LimitExceeded,
                             "reserve of " + std::to_string(quantity.value) +
                                 " Wh exceeds the configured maximum");
      }
      return Status::success();
    case ReserveUnit::KilowattHours:
      if (quantity.value > limits.max_reserve_milliwatt_hours / kMillion) {
        return Status::error(StatusCode::LimitExceeded,
                             "reserve of " + std::to_string(quantity.value) +
                                 " kWh exceeds the configured maximum");
      }
      return Status::success();
    case ReserveUnit::Seconds:
      if (quantity.value > limits.max_reserve_seconds) {
        return Status::error(StatusCode::LimitExceeded,
                             "reserve of " + std::to_string(quantity.value) +
                                 " s exceeds the configured maximum of " +
                                 std::to_string(limits.max_reserve_seconds) + " s");
      }
      return Status::success();
    case ReserveUnit::BasisPoints:
      if (quantity.value > kBasisPointMaximum) {
        return Status::error(StatusCode::InvalidArgument,
                             "basis points must lie in 0..10000, got " +
                                 std::to_string(quantity.value));
      }
      return Status::success();
  }
  return Status::error(StatusCode::InvalidArgument, "unrecognized reserve unit");
}

bool reserve_units_comparable(ReserveUnit left, ReserveUnit right) noexcept {
  return reserve_class(left) == reserve_class(right);
}

Result<ReserveQuantity> convert_reserve(const ReserveQuantity& quantity, ReserveUnit target) {
  const Status valid = validate_reserve_quantity(quantity, ResourceLimits{});
  if (!valid.ok()) {
    return valid;
  }
  if (quantity.unit == target) {
    return quantity;
  }
  if (!reserve_units_comparable(quantity.unit, target)) {
    return unit_mismatch(quantity.unit, target);
  }
  // Within a class the only conversion performed is to the finer unit, which is
  // always exact. A narrowing conversion is refused rather than rounded.
  const auto to_milliwatt_hours = [](const ReserveQuantity& source) -> Result<std::int64_t> {
    switch (source.unit) {
      case ReserveUnit::MilliwattHours:
        return source.value;
      case ReserveUnit::WattHours:
        return scale_up(source.value, kThousand);
      case ReserveUnit::KilowattHours:
        return scale_up(source.value, kMillion);
      default:
        return source.value;
    }
  };
  const Result<std::int64_t> current = to_milliwatt_hours(quantity);
  if (!current.ok()) {
    return current.status();
  }
  switch (target) {
    case ReserveUnit::MilliwattHours:
      return ReserveQuantity{target, current.value()};
    case ReserveUnit::WattHours: {
      const Result<std::int64_t> converted = checked_div_exact(current.value(), kThousand);
      if (!converted.ok()) {
        return converted.status();
      }
      return ReserveQuantity{target, converted.value()};
    }
    case ReserveUnit::KilowattHours: {
      const Result<std::int64_t> converted = checked_div_exact(current.value(), kMillion);
      if (!converted.ok()) {
        return converted.status();
      }
      return ReserveQuantity{target, converted.value()};
    }
    default:
      return unit_mismatch(quantity.unit, target);
  }
}

std::string format_reserve(const ReserveQuantity& quantity) {
  return std::to_string(quantity.value) + " " + to_string(quantity.unit);
}

Result<int> compare_reserve(const ReserveQuantity& observed, const ReserveQuantity& required) {
  if (!reserve_units_comparable(observed.unit, required.unit)) {
    return unit_mismatch(observed.unit, required.unit);
  }
  const ReserveUnit canonical =
      reserve_class(observed.unit) == ReserveClass::Energy ? ReserveUnit::MilliwattHours
                                                           : observed.unit;
  const Result<ReserveQuantity> left = convert_reserve(observed, canonical);
  if (!left.ok()) {
    return left.status();
  }
  const Result<ReserveQuantity> right = convert_reserve(required, canonical);
  if (!right.ok()) {
    return right.status();
  }
  if (left.value().value < right.value().value) {
    return -1;
  }
  if (left.value().value > right.value().value) {
    return 1;
  }
  return 0;
}

Result<bool> reserve_at_least(const ReserveQuantity& observed, const ReserveQuantity& required) {
  const Result<int> comparison = compare_reserve(observed, required);
  if (!comparison.ok()) {
    return comparison.status();
  }
  return comparison.value() >= 0;
}

const char* to_string(PowerUnit unit) noexcept {
  switch (unit) {
    case PowerUnit::Milliwatts: return "mw";
    case PowerUnit::Watts: return "w";
    case PowerUnit::Kilowatts: return "kw";
    case PowerUnit::Megawatts: return "mega_w";
  }
  return "unknown";
}

Result<PowerUnit> parse_power_unit(std::string_view text) {
  if (text == "mw") {
    return PowerUnit::Milliwatts;
  }
  if (text == "w") {
    return PowerUnit::Watts;
  }
  if (text == "kw") {
    return PowerUnit::Kilowatts;
  }
  if (text == "mega_w" || text == "megaw") {
    return PowerUnit::Megawatts;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown power unit '" + std::string(text) +
                                                        "'; accepted units are mw, w, kw, mega_w");
}

Result<PowerQuantity> make_power_quantity(PowerUnit unit, std::int64_t value,
                                          const ResourceLimits& limits) {
  const PowerQuantity quantity{unit, value};
  const Status status = validate_power_quantity(quantity, limits);
  if (!status.ok()) {
    return status;
  }
  return quantity;
}

namespace {

Result<std::int64_t> power_to_milliwatts(const PowerQuantity& quantity) {
  switch (quantity.unit) {
    case PowerUnit::Milliwatts:
      return quantity.value;
    case PowerUnit::Watts:
      return checked_mul(quantity.value, kThousand);
    case PowerUnit::Kilowatts:
      return checked_mul(quantity.value, kMillion);
    case PowerUnit::Megawatts:
      return checked_mul(quantity.value, kMillion * kThousand);
  }
  return Status::error(StatusCode::InvalidArgument, "unrecognized power unit");
}

}  // namespace

Status validate_power_quantity(const PowerQuantity& quantity, const ResourceLimits& limits) {
  if (quantity.value < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string("power quantity in ") + to_string(quantity.unit) +
                             " must not be negative, got " + std::to_string(quantity.value));
  }
  const Result<std::int64_t> milliwatts = power_to_milliwatts(quantity);
  if (!milliwatts.ok()) {
    return milliwatts.status();
  }
  if (milliwatts.value() > limits.max_reserve_milliwatt_hours) {
    return Status::error(StatusCode::LimitExceeded,
                         "power quantity of " + std::to_string(quantity.value) + " " +
                             to_string(quantity.unit) + " exceeds the configured maximum");
  }
  return Status::success();
}

Result<PowerQuantity> convert_power(const PowerQuantity& quantity, PowerUnit target) {
  const Status valid = validate_power_quantity(quantity, ResourceLimits{});
  if (!valid.ok()) {
    return valid;
  }
  const Result<std::int64_t> milliwatts = power_to_milliwatts(quantity);
  if (!milliwatts.ok()) {
    return milliwatts.status();
  }
  const std::int64_t factor = [target]() -> std::int64_t {
    switch (target) {
      case PowerUnit::Milliwatts: return 1;
      case PowerUnit::Watts: return kThousand;
      case PowerUnit::Kilowatts: return kMillion;
      case PowerUnit::Megawatts: return kMillion * kThousand;
    }
    return 1;
  }();
  const Result<std::int64_t> converted = checked_div_exact(milliwatts.value(), factor);
  if (!converted.ok()) {
    return converted.status();
  }
  return PowerQuantity{target, converted.value()};
}

Result<int> compare_power(const PowerQuantity& left, const PowerQuantity& right) {
  const Result<std::int64_t> left_milliwatts = power_to_milliwatts(left);
  if (!left_milliwatts.ok()) {
    return left_milliwatts.status();
  }
  const Result<std::int64_t> right_milliwatts = power_to_milliwatts(right);
  if (!right_milliwatts.ok()) {
    return right_milliwatts.status();
  }
  if (left_milliwatts.value() < right_milliwatts.value()) {
    return -1;
  }
  if (left_milliwatts.value() > right_milliwatts.value()) {
    return 1;
  }
  return 0;
}

std::string format_power(const PowerQuantity& quantity) {
  return std::to_string(quantity.value) + " " + to_string(quantity.unit);
}

}  // namespace ups_control
