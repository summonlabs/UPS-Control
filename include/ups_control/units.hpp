#pragma once

#include <cstdint>
#include <string_view>

#include "ups_control/arith.hpp"
#include "ups_control/limits.hpp"
#include "ups_control/status.hpp"

namespace ups_control {

// ---------------------------------------------------------------------------
// Reserve quantities
// ---------------------------------------------------------------------------
//
// Scale and unknown semantics are explicit and are part of the public contract:
//
//  * MilliwattHours, WattHours and KilowattHours are absolute stored-energy
//    quantities reported by the device. They form one comparable class, whose
//    canonical member is MilliwattHours.
//  * Seconds is a device- or operator-reported *runtime estimate at the present
//    load*. This runtime does not derive it and does not model discharge physics;
//    it is accepted as external evidence and compared only against a requirement
//    stated in Seconds.
//  * BasisPoints is a fraction of nameplate rated capacity on a fixed scale of
//    0..10000, where 10000 is exactly 100 percent. It is a ratio of nameplate, not
//    of present health, and is not convertible to or from the other units by this
//    runtime because that conversion needs a nameplate capacity and a discharge
//    model that are outside this systems boundary.
//
// A quantity never encodes "unknown". Unknown is a distinct observation state
// (see ReserveObservation) and is never represented as zero.

enum class ReserveUnit : std::uint8_t {
  /// Absolute stored energy, milliwatt-hours.
  MilliwattHours = 1,
  /// Absolute stored energy, watt-hours. Exactly 1000 milliwatt-hours.
  WattHours = 2,
  /// Absolute stored energy, kilowatt-hours. Exactly 1'000'000 milliwatt-hours.
  KilowattHours = 3,
  /// Reported runtime at the present load, seconds.
  Seconds = 4,
  /// Fraction of nameplate rated capacity in hundredths of a percent, 0..10000.
  BasisPoints = 5,
};

const char* to_string(ReserveUnit unit) noexcept;

/// Parses the canonical token (\c mwh, \c wh, \c kwh, \c s, \c bp).
Result<ReserveUnit> parse_reserve_unit(std::string_view text);

/// The equivalence class used for comparison. Two quantities are comparable only
/// when they share a class; nothing is ever converted across classes.
enum class ReserveClass : std::uint8_t { Energy = 1, Runtime = 2, NameplateFraction = 3 };

ReserveClass reserve_class(ReserveUnit unit) noexcept;

/// An exact, non-negative reserve quantity.
struct ReserveQuantity {
  ReserveUnit unit = ReserveUnit::MilliwattHours;
  std::int64_t value = 0;

  friend bool operator==(const ReserveQuantity&, const ReserveQuantity&) = default;
};

/// Builds a quantity, enforcing the unit's own range and the configured bound.
Result<ReserveQuantity> make_reserve_quantity(ReserveUnit unit, std::int64_t value,
                                              const ResourceLimits& limits = ResourceLimits{});

Status validate_reserve_quantity(const ReserveQuantity& quantity, const ResourceLimits& limits);

/// True when the two units belong to the same comparable class.
bool reserve_units_comparable(ReserveUnit left, ReserveUnit right) noexcept;

/// Converts to a finer (never lossy) unit of the same class. Conversion across
/// classes, and conversion that would truncate, is refused with
/// \c StatusCode::Unsupported rather than rounded.
Result<ReserveQuantity> convert_reserve(const ReserveQuantity& quantity, ReserveUnit target);

/// Renders a quantity in its own unit, for diagnostics only.
std::string format_reserve(const ReserveQuantity& quantity);

/// Compares an observed quantity with a required one. Returns an error with
/// \c StatusCode::Unsupported when the two are not comparable; a caller must
/// treat that as indeterminate, never as satisfied.
Result<int> compare_reserve(const ReserveQuantity& observed, const ReserveQuantity& required);

/// True only when \c observed is known to be at least \c required. An error
/// result means "not established" and must not be read as true.
Result<bool> reserve_at_least(const ReserveQuantity& observed, const ReserveQuantity& required);

// ---------------------------------------------------------------------------
// Power quantities
// ---------------------------------------------------------------------------

enum class PowerUnit : std::uint8_t {
  Milliwatts = 1,
  Watts = 2,
  Kilowatts = 3,
  Megawatts = 4,
};

const char* to_string(PowerUnit unit) noexcept;
Result<PowerUnit> parse_power_unit(std::string_view text);

struct PowerQuantity {
  PowerUnit unit = PowerUnit::Milliwatts;
  std::int64_t value = 0;

  friend bool operator==(const PowerQuantity&, const PowerQuantity&) = default;
};

Result<PowerQuantity> make_power_quantity(PowerUnit unit, std::int64_t value,
                                          const ResourceLimits& limits = ResourceLimits{});
Status validate_power_quantity(const PowerQuantity& quantity, const ResourceLimits& limits);
Result<PowerQuantity> convert_power(const PowerQuantity& quantity, PowerUnit target);
Result<int> compare_power(const PowerQuantity& left, const PowerQuantity& right);
std::string format_power(const PowerQuantity& quantity);

}  // namespace ups_control
