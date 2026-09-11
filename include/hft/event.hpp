#pragma once

#include "hft/types.hpp"
#include <type_traits>
#include <ostream>

namespace hft {

/**
 * @brief Identifies the action requested by an incoming event.
 */
enum class EventType : uint8_t {
    Add = 1,
    Cancel = 2,
    Modify = 3
};

/**
 * @brief Compact, trivially copyable 32-byte event representation for the hot path.
 *
 * Designed to eliminate heap allocation and indirection, fitting exactly two events
 * per standard 64-byte hardware cache line.
 */
struct OrderEvent {
    EventType type{EventType::Add};
    Side      side{Side::Buy};
    uint8_t   pad[6]{0};        // Explicit padding for 8-byte field alignment
    OrderId   id{0};            // Unique order identifier
    Price     price{0};         // Limit price (Add) or replacement price (Modify)
    Quantity  qty{0};           // Order quantity (Add) or replacement quantity (Modify)

    static constexpr OrderEvent make_add(OrderId order_id, Side order_side, Price limit_price, Quantity quantity) noexcept {
        OrderEvent ev{};
        ev.type = EventType::Add;
        ev.side = order_side;
        ev.id = order_id;
        ev.price = limit_price;
        ev.qty = quantity;
        return ev;
    }

    static constexpr OrderEvent make_cancel(OrderId order_id) noexcept {
        OrderEvent ev{};
        ev.type = EventType::Cancel;
        ev.id = order_id;
        return ev;
    }

    static constexpr OrderEvent make_modify(OrderId order_id, Price new_price, Quantity new_qty) noexcept {
        OrderEvent ev{};
        ev.type = EventType::Modify;
        ev.id = order_id;
        ev.price = new_price;
        ev.qty = new_qty;
        return ev;
    }

    constexpr bool operator==(const OrderEvent& other) const noexcept {
        return type == other.type &&
               side == other.side &&
               id == other.id &&
               price == other.price &&
               qty == other.qty;
    }
};

static_assert(sizeof(OrderEvent) == 32, "OrderEvent must be exactly 32 bytes");
static_assert(std::is_trivially_copyable_v<OrderEvent>, "OrderEvent must be trivially copyable");
static_assert(std::is_standard_layout_v<OrderEvent>, "OrderEvent must have standard layout");

inline std::ostream& operator<<(std::ostream& os, EventType type) {
    switch (type) {
        case EventType::Add:    return os << "ADD";
        case EventType::Cancel: return os << "CANCEL";
        case EventType::Modify: return os << "MODIFY";
    }
    return os << "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, const OrderEvent& ev) {
    return os << "OrderEvent{type=" << ev.type
              << ", id=" << ev.id
              << ", side=" << ev.side
              << ", price=" << ev.price
              << ", qty=" << ev.qty << "}";
}

} // namespace hft
