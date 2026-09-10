#pragma once

#include <cstdint>
#include <string_view>
#include <string>
#include <ostream>


namespace hft {

/**
 * @brief Fixed-point integer price representation.
 *
 * Floating-point representation (e.g. float/double) is strictly avoided in
 * the core matching engine because:
 *  1. IEEE-754 binary floating point cannot precisely represent decimal fractions
 *     (e.g., 0.1 + 0.2 != 0.3), introducing cumulative rounding errors.
 *  2. Equality comparisons between floating point values are indeterminate without
 *     an arbitrary epsilon, violating deterministic matching.
 *  3. Different compiler optimizations, architectures, or FPU registers (e.g. x87 vs SSE)
 *     can evaluate floating-point expressions differently, breaking replay determinism.
 *
 * In this engine, Price is stored as an integer multiple of the instrument's minimum
 * price variation (tick). For example, 10025 represents 100.25 when tick scale is 100.
 */
using Price = int64_t;

/**
 * @brief Order and trade quantity representation (e.g. number of shares or lots).
 */
using Quantity = uint64_t;

/**
 * @brief Unique order identifier across the lifetime of the engine.
 */
using OrderId = uint64_t;

/**
 * @brief Unique trade execution identifier.
 */
using TradeId = uint64_t;

/**
 * @brief Monotonic sequence or timestamp in nanoseconds.
 */
using Timestamp = uint64_t;

/**
 * @brief Order side: Buy (bid) or Sell (ask).
 */
enum class Side : uint8_t {
    Buy,
    Sell
};

/**
 * @brief Supported order types for Phase 1.
 */
enum class OrderType : uint8_t {
    Limit
};

/**
 * @brief Status returned by order submission, cancellation, and modification.
 */
enum class OrderResult : uint8_t {
    Accepted,
    RejectedInvalidPrice,
    RejectedInvalidQuantity,
    RejectedDuplicateId,
    RejectedOrderNotFound,
    RejectedUnchanged
};

constexpr std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:  return "BUY";
        case Side::Sell: return "SELL";
    }
    return "UNKNOWN";
}

constexpr std::string_view to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Limit: return "LIMIT";
    }
    return "UNKNOWN";
}

constexpr std::string_view to_string(OrderResult res) noexcept {
    switch (res) {
        case OrderResult::Accepted:                 return "ACCEPTED";
        case OrderResult::RejectedInvalidPrice:     return "REJECTED_INVALID_PRICE";
        case OrderResult::RejectedInvalidQuantity:  return "REJECTED_INVALID_QUANTITY";
        case OrderResult::RejectedDuplicateId:      return "REJECTED_DUPLICATE_ID";
        case OrderResult::RejectedOrderNotFound:    return "REJECTED_ORDER_NOT_FOUND";
        case OrderResult::RejectedUnchanged:        return "REJECTED_UNCHANGED";
    }
    return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, Side side) {
    return os << to_string(side);
}

inline std::ostream& operator<<(std::ostream& os, OrderType type) {
    return os << to_string(type);
}

inline std::ostream& operator<<(std::ostream& os, OrderResult res) {
    return os << to_string(res);
}

} // namespace hft

