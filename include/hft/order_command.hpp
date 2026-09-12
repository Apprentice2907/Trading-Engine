#pragma once

#include "hft/types.hpp"
#include "hft/event.hpp"

#include <cstdint>
#include <type_traits>
#include <ostream>

namespace hft {

/**
 * @brief Cache-aligned 64-byte normalized order command representation.
 *
 * Designed for low-latency ingress queues, pre-trade risk validation, and gateway dispatch.
 * Fits exactly one standard 64-byte hardware cache line with zero heap allocations.
 */
struct alignas(64) OrderCommand {
    OrderId   order_id{0};                  // Unique order identifier (8B)
    uint32_t  instrument_id{0};             // Instrument / security token (4B)
    uint32_t  client_id{0};                 // Client or strategy account ID (4B)
    EventType type{EventType::Add};         // Action requested: Add, Cancel, Modify (1B)
    Side      side{Side::Buy};               // Order side: Buy or Sell (1B)
    OrderType order_type{OrderType::Limit};  // Order execution type: Limit (1B)
    uint8_t   pad[5]{0};                    // Alignment padding (5B)
    Price     price{0};                     // Limit price in discrete integer ticks / paise (8B)
    Quantity  qty{0};                       // Order quantity (8B)
    Timestamp timestamp{0};                 // Ingress monotonic timestamp in nanoseconds (8B)
    uint8_t   reserved[16]{0};              // Future expansion padding to exactly 64 bytes (16B)

    static constexpr OrderCommand make_add(OrderId id, uint32_t instrument, uint32_t client,
                                          Side side, Price price, Quantity qty,
                                          Timestamp ts = 0) noexcept {
        OrderCommand cmd{};
        cmd.order_id = id;
        cmd.instrument_id = instrument;
        cmd.client_id = client;
        cmd.type = EventType::Add;
        cmd.side = side;
        cmd.order_type = OrderType::Limit;
        cmd.price = price;
        cmd.qty = qty;
        cmd.timestamp = ts;
        return cmd;
    }

    static constexpr OrderCommand make_cancel(OrderId id, uint32_t instrument, uint32_t client,
                                             Timestamp ts = 0) noexcept {
        OrderCommand cmd{};
        cmd.order_id = id;
        cmd.instrument_id = instrument;
        cmd.client_id = client;
        cmd.type = EventType::Cancel;
        cmd.timestamp = ts;
        return cmd;
    }

    static constexpr OrderCommand make_modify(OrderId id, uint32_t instrument, uint32_t client,
                                             Side side, Price new_price, Quantity new_qty,
                                             Timestamp ts = 0) noexcept {
        OrderCommand cmd{};
        cmd.order_id = id;
        cmd.instrument_id = instrument;
        cmd.client_id = client;
        cmd.type = EventType::Modify;
        cmd.side = side;
        cmd.order_type = OrderType::Limit;
        cmd.price = new_price;
        cmd.qty = new_qty;
        cmd.timestamp = ts;
        return cmd;
    }

    constexpr bool operator==(const OrderCommand& other) const noexcept {
        return order_id == other.order_id &&
               instrument_id == other.instrument_id &&
               client_id == other.client_id &&
               type == other.type &&
               side == other.side &&
               order_type == other.order_type &&
               price == other.price &&
               qty == other.qty &&
               timestamp == other.timestamp;
    }
};

static_assert(sizeof(OrderCommand) == 64, "OrderCommand must be exactly 64 bytes (1 hardware cache line)");
static_assert(alignof(OrderCommand) == 64, "OrderCommand must be 64-byte cache-line aligned");
static_assert(std::is_trivially_copyable_v<OrderCommand>, "OrderCommand must be trivially copyable");
static_assert(std::is_standard_layout_v<OrderCommand>, "OrderCommand must have standard layout");

inline std::ostream& operator<<(std::ostream& os, const OrderCommand& cmd) {
    return os << "OrderCommand{id=" << cmd.order_id
              << ", inst=" << cmd.instrument_id
              << ", client=" << cmd.client_id
              << ", type=" << cmd.type
              << ", side=" << cmd.side
              << ", price=" << cmd.price
              << ", qty=" << cmd.qty
              << ", ts=" << cmd.timestamp << "}";
}

} // namespace hft
