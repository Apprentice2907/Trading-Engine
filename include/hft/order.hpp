#pragma once

#include "hft/types.hpp"
#include <tuple>

namespace hft {

/**
 * @brief Representation of an order within the exchange engine.
 */
struct Order {
    OrderId   id{0};
    Price     price{0};
    Quantity  initial_qty{0};
    Quantity  remaining_qty{0};
    Side      side{Side::Buy};
    OrderType type{OrderType::Limit};
    Timestamp timestamp{0};

    [[nodiscard]] constexpr bool is_filled() const noexcept {
        return remaining_qty == 0;
    }

    [[nodiscard]] constexpr Quantity executed_qty() const noexcept {
        return initial_qty - remaining_qty;
    }

    constexpr bool operator==(const Order& other) const noexcept {
        return id == other.id &&
               price == other.price &&
               initial_qty == other.initial_qty &&
               remaining_qty == other.remaining_qty &&
               side == other.side &&
               type == other.type &&
               timestamp == other.timestamp;
    }
};

/**
 * @brief Event emitted whenever a trade execution occurs.
 */
struct Trade {
    TradeId   trade_id{0};
    OrderId   resting_order_id{0};
    OrderId   incoming_order_id{0};
    Price     price{0};
    Quantity  quantity{0};
    Side      aggressor_side{Side::Buy};
    Timestamp timestamp{0};

    constexpr bool operator==(const Trade& other) const noexcept {
        return trade_id == other.trade_id &&
               resting_order_id == other.resting_order_id &&
               incoming_order_id == other.incoming_order_id &&
               price == other.price &&
               quantity == other.quantity &&
               aggressor_side == other.aggressor_side &&
               timestamp == other.timestamp;
    }
};

} // namespace hft
