#pragma once

#include "hft/types.hpp"
#include <cstdint>
#include <type_traits>

namespace hft {

/**
 * @brief Cache-aligned 64-byte normalized market data event.
 *
 * Represents top-of-book and trade observations from external broker feeds.
 * Strictly decoupled from matching engine OrderEvents.
 * Fits exactly one standard 64-byte CPU cache line with zero heap allocation.
 */
struct alignas(64) MarketEvent {
    uint32_t instrument_token{0};   // Numeric instrument token / security ID (4B)
    uint8_t  exchange_type{0};      // Exchange identifier: 1=NSE_CM, 2=NSE_FO, etc. (1B)
    uint8_t  subscription_mode{0};  // Mode received: 1=LTP, 2=Quote, 3=SnapQuote (1B)
    uint16_t pad{0};                // Explicit alignment padding (2B)
    uint64_t sequence_number{0};    // Packet sequence number from exchange (8B)
    uint64_t exchange_timestamp{0}; // Provider timestamp in epoch nanoseconds (8B)
    uint64_t receive_timestamp{0};  // Monotonic steady_clock in nanoseconds (8B)
    int64_t  last_price{0};         // Last traded price in discrete ticks / paise (8B)
    uint64_t last_quantity{0};      // Last traded quantity (8B)
    int64_t  best_bid_price{0};     // Best bid price in discrete ticks / paise (8B)
    uint64_t best_bid_quantity{0};  // Quantity resting at best bid (8B)
    int64_t  best_ask_price{0};     // Best ask price in discrete ticks / paise (8B)
    uint64_t best_ask_quantity{0};  // Quantity resting at best ask (8B)
    uint64_t volume{0};             // Cumulative daily volume traded (8B)
    uint8_t  reserved[40]{0};       // Reserved padding to exactly 128 bytes (2 hardware cache lines) (40B)

    constexpr bool operator==(const MarketEvent& other) const noexcept {
        return instrument_token == other.instrument_token &&
               exchange_type == other.exchange_type &&
               subscription_mode == other.subscription_mode &&
               sequence_number == other.sequence_number &&
               exchange_timestamp == other.exchange_timestamp &&
               receive_timestamp == other.receive_timestamp &&
               last_price == other.last_price &&
               last_quantity == other.last_quantity &&
               best_bid_price == other.best_bid_price &&
               best_bid_quantity == other.best_bid_quantity &&
               best_ask_price == other.best_ask_price &&
               best_ask_quantity == other.best_ask_quantity &&
               volume == other.volume;
    }
};

static_assert(sizeof(MarketEvent) == 128, "MarketEvent must be exactly 128 bytes (2 hardware cache lines)");
static_assert(alignof(MarketEvent) == 64, "MarketEvent must be 64-byte cache-line aligned");
static_assert(std::is_trivially_copyable_v<MarketEvent>, "MarketEvent must be trivially copyable");
static_assert(std::is_standard_layout_v<MarketEvent>, "MarketEvent must have standard layout");

} // namespace hft
