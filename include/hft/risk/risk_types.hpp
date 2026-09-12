#pragma once

#include "hft/types.hpp"
#include <cstdint>
#include <string_view>
#include <ostream>

namespace hft {

/**
 * @brief Pre-trade risk evaluation codes.
 */
enum class RiskCode : uint8_t {
    Approved = 0,
    InvalidSide = 1,
    InvalidPrice = 2,
    InvalidQuantity = 3,
    InvalidInstrument = 4,
    MaxQuantityExceeded = 5,
    MaxNotionalExceeded = 6,
    PriceBandViolation = 7,
    ExposureLimitExceeded = 8
};

constexpr std::string_view to_string(RiskCode code) noexcept {
    switch (code) {
        case RiskCode::Approved:              return "APPROVED";
        case RiskCode::InvalidSide:           return "INVALID_SIDE";
        case RiskCode::InvalidPrice:          return "INVALID_PRICE";
        case RiskCode::InvalidQuantity:       return "INVALID_QUANTITY";
        case RiskCode::InvalidInstrument:     return "INVALID_INSTRUMENT";
        case RiskCode::MaxQuantityExceeded:   return "MAX_QUANTITY_EXCEEDED";
        case RiskCode::MaxNotionalExceeded:   return "MAX_NOTIONAL_EXCEEDED";
        case RiskCode::PriceBandViolation:    return "PRICE_BAND_VIOLATION";
        case RiskCode::ExposureLimitExceeded: return "EXPOSURE_LIMIT_EXCEEDED";
    }
    return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, RiskCode code) {
    return os << to_string(code);
}

/**
 * @brief Lightweight risk evaluation result.
 */
struct RiskResult {
    RiskCode code{RiskCode::Approved};

    [[nodiscard]] constexpr bool approved() const noexcept {
        return code == RiskCode::Approved;
    }

    constexpr explicit operator bool() const noexcept {
        return approved();
    }
};

/**
 * @brief Configuration parameters for pre-trade risk thresholds.
 */
struct RiskConfig {
    Quantity max_order_quantity{100'000};        // Max quantity allowed in a single order
    uint64_t max_order_notional{50'000'000'00};  // Max notional value (price * quantity) in paise (e.g. 50 Lakhs)
    Price    min_price{1};                       // Minimum allowed limit price (ticks)
    Price    max_price{100'000'00};              // Maximum allowed limit price (ticks)
    Quantity max_exposure_quantity{500'000};     // Max cumulative open/resting order quantity
    uint32_t allowed_instrument_id{0};           // If non-zero, orders for other instruments are rejected
};

/**
 * @brief Zero-allocation risk engine counters for monitoring and audit.
 */
struct RiskStats {
    uint64_t orders_checked{0};
    uint64_t orders_approved{0};
    uint64_t orders_rejected{0};
    uint64_t reject_invalid_side{0};
    uint64_t reject_invalid_price{0};
    uint64_t reject_invalid_quantity{0};
    uint64_t reject_invalid_instrument{0};
    uint64_t reject_max_quantity{0};
    uint64_t reject_max_notional{0};
    uint64_t reject_price_band{0};
    uint64_t reject_exposure_limit{0};

    void reset() noexcept {
        *this = RiskStats{};
    }
};

} // namespace hft
