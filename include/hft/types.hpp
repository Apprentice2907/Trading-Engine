#pragma once

#include <cstdint>
#include <string_view>
#include <ostream>

namespace hft {

// Core numerical types
using Price = int64_t;
using Quantity = uint64_t;
using OrderId = uint64_t;
using TradeId = uint64_t;
using Timestamp = uint64_t;
using InstrumentId = uint32_t;
using ClientId = uint32_t;

// Sentinels
inline constexpr Price INVALID_PRICE = 0;
inline constexpr Quantity INVALID_QUANTITY = 0;
inline constexpr OrderId INVALID_ORDER_ID = 0;

enum class Side : uint8_t {
    Buy = 1,
    Sell = 2
};

enum class OrderType : uint8_t {
    Limit = 1
};

enum class EventType : uint8_t {
    Add = 1,
    Cancel = 2,
    Modify = 3
};

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

enum class OrderResult : uint8_t {
    Accepted = 0,
    RejectedInvalidPrice = 1,
    RejectedInvalidQuantity = 2,
    RejectedDuplicateId = 3,
    RejectedOrderNotFound = 4,
    RejectedUnchanged = 5
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

constexpr std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::Add:    return "ADD";
        case EventType::Cancel: return "CANCEL";
        case EventType::Modify: return "MODIFY";
    }
    return "UNKNOWN";
}

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

inline std::ostream& operator<<(std::ostream& os, Side side) { return os << to_string(side); }
inline std::ostream& operator<<(std::ostream& os, OrderType type) { return os << to_string(type); }
inline std::ostream& operator<<(std::ostream& os, EventType type) { return os << to_string(type); }
inline std::ostream& operator<<(std::ostream& os, RiskCode code) { return os << to_string(code); }
inline std::ostream& operator<<(std::ostream& os, OrderResult res) { return os << to_string(res); }

} // namespace hft
