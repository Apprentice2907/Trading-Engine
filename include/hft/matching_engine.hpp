#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/order_book.hpp"

#include <vector>
#include <optional>
#include <string>

namespace hft {

/**
 * @brief Top-level Matching Engine coordinating order routing, sequence numbering,
 * trade generation, and order book state.
 */
class MatchingEngine {
public:
    MatchingEngine() = default;
    ~MatchingEngine() = default;

    // Non-copyable, movable
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) noexcept = default;
    MatchingEngine& operator=(MatchingEngine&&) noexcept = default;

    /**
     * @brief Submit a new Limit Order to the matching engine.
     * Matches aggressively against opposite resting orders. Any residual quantity rests in the book.
     *
     * @param id Unique order ID
     * @param side Buy or Sell
     * @param price Limit price (integer ticks)
     * @param qty Order quantity (> 0)
     * @param trades Output vector collecting generated execution events
     * @return OrderResult status
     */
    OrderResult submit_limit_order(OrderId id, Side side, Price price, Quantity qty,
                                  std::vector<Trade>& trades);

    /**
     * @brief Cancel an existing live order.
     */
    OrderResult cancel_order(OrderId id);

    /**
     * @brief Modify an existing live order's price and/or quantity.
     */
    OrderResult modify_order(OrderId id, Price new_price, Quantity new_qty,
                            std::vector<Trade>& trades);

    // Engine and book inspection
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] uint64_t total_trades_generated() const noexcept { return trade_sequence_; }
    [[nodiscard]] Timestamp current_sequence() const noexcept { return sequence_number_; }

    /**
     * @brief Validates all engine and order book invariants.
     */
    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const {
        return book_.verify_invariants(error_out);
    }

    /**
     * @brief Resets the engine to a clean state.
     */
    void reset();

private:
    OrderBook book_;
    uint64_t trade_sequence_{0};
    Timestamp sequence_number_{0};
};

} // namespace hft
