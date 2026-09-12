#pragma once

#include "hft/types.hpp"
#include "hft/risk/risk_types.hpp"

#include <cstdint>
#include <type_traits>
#include <string_view>
#include <ostream>

namespace hft {

/**
 * @brief Execution report event types emitted by the Order Gateway.
 */
enum class ExecutionType : uint8_t {
    New = 1,              // Order successfully accepted and resting in the order book
    RiskRejected = 2,     // Order rejected by pre-trade risk engine
    EngineRejected = 3,   // Order rejected by matching engine (e.g. duplicate ID, invalid price)
    Trade = 4,            // Execution fill or partial fill occurred
    Cancelled = 5,        // Order successfully cancelled from the book
    Modified = 6          // Order price and/or quantity successfully modified
};

constexpr std::string_view to_string(ExecutionType type) noexcept {
    switch (type) {
        case ExecutionType::New:            return "NEW";
        case ExecutionType::RiskRejected:   return "RISK_REJECTED";
        case ExecutionType::EngineRejected: return "ENGINE_REJECTED";
        case ExecutionType::Trade:          return "TRADE";
        case ExecutionType::Cancelled:      return "CANCELLED";
        case ExecutionType::Modified:       return "MODIFIED";
    }
    return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, ExecutionType type) {
    return os << to_string(type);
}

/**
 * @brief Cache-aligned 64-byte normalized execution report.
 *
 * Represents an execution update, fill event, or rejection emitted by the gateway.
 * Fits exactly one standard 64-byte hardware cache line with zero heap allocations.
 */
struct alignas(64) ExecutionReport {
    OrderId       order_id{0};          // Target order ID (8B)
    uint64_t      exec_id{0};           // Monotonic execution sequence number (8B)
    Price         price{0};             // Execution fill price or order limit price (8B)
    Quantity      last_qty{0};          // Executed quantity for this fill event (8B)
    Quantity      leaves_qty{0};        // Remaining resting quantity in book (8B)
    Timestamp     timestamp{0};         // Execution timestamp in nanoseconds (8B)
    uint32_t      instrument_id{0};     // Security / instrument token (4B)
    uint32_t      client_id{0};         // Client / strategy identifier (4B)
    ExecutionType exec_type{ExecutionType::New}; // Execution event category (1B)
    Side          side{Side::Buy};      // Order side: Buy or Sell (1B)
    RiskCode      risk_code{RiskCode::Approved}; // Risk rejection reason if rejected (1B)
    OrderResult   engine_result{OrderResult::Accepted}; // Matching engine status code (1B)
    uint8_t       pad[4]{0};            // Explicit padding to exactly 64 bytes (4B)

    static constexpr ExecutionReport make_risk_rejected(OrderId id, uint32_t inst, uint32_t client,
                                                        Side side, Price price, [[maybe_unused]] Quantity qty,
                                                        RiskCode code, uint64_t exec_id,
                                                        Timestamp ts = 0) noexcept {
        ExecutionReport rep{};
        rep.order_id = id;
        rep.exec_id = exec_id;
        rep.price = price;
        rep.last_qty = 0;
        rep.leaves_qty = 0;
        rep.timestamp = ts;
        rep.instrument_id = inst;
        rep.client_id = client;
        rep.exec_type = ExecutionType::RiskRejected;
        rep.side = side;
        rep.risk_code = code;
        rep.engine_result = OrderResult::Accepted;
        return rep;
    }

    static constexpr ExecutionReport make_engine_rejected(OrderId id, uint32_t inst, uint32_t client,
                                                          Side side, Price price, [[maybe_unused]] Quantity qty,
                                                          OrderResult res, uint64_t exec_id,
                                                          Timestamp ts = 0) noexcept {
        ExecutionReport rep{};
        rep.order_id = id;
        rep.exec_id = exec_id;
        rep.price = price;
        rep.last_qty = 0;
        rep.leaves_qty = 0;
        rep.timestamp = ts;
        rep.instrument_id = inst;
        rep.client_id = client;
        rep.exec_type = ExecutionType::EngineRejected;
        rep.side = side;
        rep.risk_code = RiskCode::Approved;
        rep.engine_result = res;
        return rep;
    }

    static constexpr ExecutionReport make_new(OrderId id, uint32_t inst, uint32_t client,
                                             Side side, Price price, Quantity resting_qty,
                                             uint64_t exec_id, Timestamp ts = 0) noexcept {
        ExecutionReport rep{};
        rep.order_id = id;
        rep.exec_id = exec_id;
        rep.price = price;
        rep.last_qty = 0;
        rep.leaves_qty = resting_qty;
        rep.timestamp = ts;
        rep.instrument_id = inst;
        rep.client_id = client;
        rep.exec_type = ExecutionType::New;
        rep.side = side;
        rep.risk_code = RiskCode::Approved;
        rep.engine_result = OrderResult::Accepted;
        return rep;
    }

    static constexpr ExecutionReport make_trade(OrderId id, uint32_t inst, uint32_t client,
                                               Side side, Price exec_price, Quantity fill_qty,
                                               Quantity remaining_leaves, uint64_t exec_id,
                                               Timestamp ts = 0) noexcept {
        ExecutionReport rep{};
        rep.order_id = id;
        rep.exec_id = exec_id;
        rep.price = exec_price;
        rep.last_qty = fill_qty;
        rep.leaves_qty = remaining_leaves;
        rep.timestamp = ts;
        rep.instrument_id = inst;
        rep.client_id = client;
        rep.exec_type = ExecutionType::Trade;
        rep.side = side;
        rep.risk_code = RiskCode::Approved;
        rep.engine_result = OrderResult::Accepted;
        return rep;
    }

    static constexpr ExecutionReport make_cancelled(OrderId id, uint32_t inst, uint32_t client,
                                                    Side side, Quantity cancelled_qty,
                                                    uint64_t exec_id, Timestamp ts = 0) noexcept {
        ExecutionReport rep{};
        rep.order_id = id;
        rep.exec_id = exec_id;
        rep.price = 0;
        rep.last_qty = cancelled_qty;
        rep.leaves_qty = 0;
        rep.timestamp = ts;
        rep.instrument_id = inst;
        rep.client_id = client;
        rep.exec_type = ExecutionType::Cancelled;
        rep.side = side;
        rep.risk_code = RiskCode::Approved;
        rep.engine_result = OrderResult::Accepted;
        return rep;
    }

    static constexpr ExecutionReport make_modified(OrderId id, uint32_t inst, uint32_t client,
                                                   Side side, Price new_price, Quantity remaining_qty,
                                                   uint64_t exec_id, Timestamp ts = 0) noexcept {
        ExecutionReport rep{};
        rep.order_id = id;
        rep.exec_id = exec_id;
        rep.price = new_price;
        rep.last_qty = 0;
        rep.leaves_qty = remaining_qty;
        rep.timestamp = ts;
        rep.instrument_id = inst;
        rep.client_id = client;
        rep.exec_type = ExecutionType::Modified;
        rep.side = side;
        rep.risk_code = RiskCode::Approved;
        rep.engine_result = OrderResult::Accepted;
        return rep;
    }

    constexpr bool operator==(const ExecutionReport& other) const noexcept {
        return order_id == other.order_id &&
               exec_id == other.exec_id &&
               price == other.price &&
               last_qty == other.last_qty &&
               leaves_qty == other.leaves_qty &&
               timestamp == other.timestamp &&
               instrument_id == other.instrument_id &&
               client_id == other.client_id &&
               exec_type == other.exec_type &&
               side == other.side &&
               risk_code == other.risk_code &&
               engine_result == other.engine_result;
    }
};

static_assert(sizeof(ExecutionReport) == 64, "ExecutionReport must be exactly 64 bytes (1 hardware cache line)");
static_assert(alignof(ExecutionReport) == 64, "ExecutionReport must be 64-byte cache-line aligned");
static_assert(std::is_trivially_copyable_v<ExecutionReport>, "ExecutionReport must be trivially copyable");
static_assert(std::is_standard_layout_v<ExecutionReport>, "ExecutionReport must have standard layout");

inline std::ostream& operator<<(std::ostream& os, const ExecutionReport& rep) {
    return os << "ExecutionReport{id=" << rep.order_id
              << ", exec_id=" << rep.exec_id
              << ", type=" << rep.exec_type
              << ", side=" << rep.side
              << ", price=" << rep.price
              << ", last_qty=" << rep.last_qty
              << ", leaves_qty=" << rep.leaves_qty
              << ", risk=" << rep.risk_code
              << ", res=" << rep.engine_result << "}";
}

} // namespace hft
