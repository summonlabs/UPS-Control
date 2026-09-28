#pragma once

#include <cstdint>
#include <limits>

#include "ups_control/status.hpp"

namespace ups_control {

/// Checked integer arithmetic for authoritative quantities.
///
/// Every authoritative calculation in this runtime is exact integer arithmetic.
/// Overflow is refused, never wrapped and never silently clamped. Floating point
/// is not used for any reserve, limit, duration, or comparison.
inline Result<std::int64_t> checked_add(std::int64_t left, std::int64_t right) {
  if (right > 0 && left > (std::numeric_limits<std::int64_t>::max)() - right) {
    return Status::error(StatusCode::LimitExceeded, "integer overflow adding " +
                                                        std::to_string(left) + " and " +
                                                        std::to_string(right));
  }
  if (right < 0 && left < (std::numeric_limits<std::int64_t>::min)() - right) {
    return Status::error(StatusCode::LimitExceeded, "integer underflow adding " +
                                                        std::to_string(left) + " and " +
                                                        std::to_string(right));
  }
  return left + right;
}

inline Result<std::int64_t> checked_sub(std::int64_t left, std::int64_t right) {
  if (right == (std::numeric_limits<std::int64_t>::min)()) {
    // Negating the most negative value is not representable, so the general path
    // cannot be used. Exactly one subtraction with this operand is representable,
    // and it is computed rather than refused: refusing it would report an overflow
    // that does not exist.
    if (left == (std::numeric_limits<std::int64_t>::min)()) {
      return std::int64_t{0};
    }
    return Status::error(StatusCode::LimitExceeded,
                         "integer overflow subtracting " + std::to_string(right) + " from " +
                             std::to_string(left));
  }
  return checked_add(left, -right);
}

/// Multiplication that refuses overflow.
///
/// The product is bounded by division *before* it is formed, so the operator
/// itself is only ever evaluated on operands whose result is representable. A
/// post-hoc check of a computed product would evaluate a signed overflow, which is
/// undefined behaviour, and would rely on the wrap-around it happens to produce.
inline Result<std::int64_t> checked_mul(std::int64_t left, std::int64_t right) {
  constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
  constexpr std::int64_t kMin = (std::numeric_limits<std::int64_t>::min)();
  const auto overflow = [left, right]() {
    return Status::error(StatusCode::LimitExceeded,
                         "integer overflow multiplying " + std::to_string(left) + " by " +
                             std::to_string(right));
  };
  if (left == 0 || right == 0) {
    return std::int64_t{0};
  }
  if (left == -1) {
    return right == kMin ? overflow() : Result<std::int64_t>{-right};
  }
  if (right == -1) {
    return left == kMin ? overflow() : Result<std::int64_t>{-left};
  }
  if (left > 0) {
    if (right > 0) {
      if (left > kMax / right) {
        return overflow();
      }
    } else if (left > kMin / right) {
      return overflow();
    }
  } else {
    if (right > 0) {
      if (left < kMin / right) {
        return overflow();
      }
    } else if (left < kMax / right) {
      return overflow();
    }
  }
  return left * right;
}

/// Exact division. Refuses a zero divisor and a non-zero remainder, so a caller
/// can never mistake a truncated result for an exact one.
inline Result<std::int64_t> checked_div_exact(std::int64_t dividend, std::int64_t divisor) {
  if (divisor == 0) {
    return Status::error(StatusCode::InvalidArgument, "division by zero");
  }
  // The one division whose result is not representable. It is refused before the
  // operator is evaluated, because evaluating it is undefined behaviour.
  if (dividend == (std::numeric_limits<std::int64_t>::min)() && divisor == -1) {
    return Status::error(StatusCode::LimitExceeded,
                         "the quotient of " + std::to_string(dividend) + " and " +
                             std::to_string(divisor) + " is not representable");
  }
  if (dividend % divisor != 0) {
    return Status::error(StatusCode::Unsupported,
                         std::to_string(dividend) + " is not an exact multiple of " +
                             std::to_string(divisor) +
                             "; this runtime refuses a lossy unit conversion rather than "
                             "rounding it");
  }
  return dividend / divisor;
}

}  // namespace ups_control
