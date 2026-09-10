#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"

#include <map>
#include <unordered_map>
#include <list>
#include <vector>
#include <optional>
#include <string>

namespace hft {

/**
 * @brief Represents an aggregated price level with a FIFO queue of resting orders.
 */
struct PriceLevel {
    Price price{0};
    Quantity total_quantity{0};
    std::list<Order> orders;
};

/**
 * @brief Lightweight snapshot of a price level for display and inspection.
 */
struct LevelView {
    Price price{0};
    Quantity total_quantity{0};
    size_t order_count{0};
};

/**
 * @brief Deterministic, single-threaded Limit Order Book.
 *
 * Implements standard price-time priority (FIFO at each price level).
 * Baseline implementation using std::map, std::list, and std::unordered_map
 * as a robust control implementation for future optimization comparisons.
 */
class OrderBook {
public:
    OrderBook() = default;
    ~OrderBook() = default;

    // Non-copyable, movable
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    /**
     * @brief Direct insertion of a passive resting order without matching.
     * @return OrderResult::Accepted or error status.
     */
    OrderResult add_resting_order(const Order& order);

    /**
     * @brief Match an incoming order against the resting opposite book.
     * Generates trade events for each execution. Does NOT add residual quantity to the book.
     * @param incoming The incoming order (its remaining_qty is updated).
     * @param trades Output vector where generated trades are appended.
     * @param trade_seq Reference to monotonic trade ID sequence generator.
     * @return Number of trades generated.
     */
    size_t match(Order& incoming, std::vector<Trade>& trades, uint64_t& trade_seq);

    /**
     * @brief Process an incoming limit order: matches against opposite side,
     * and if unfilled remaining quantity exists, places it as a resting order in the book.
     */
    OrderResult process_order(Order incoming, std::vector<Trade>& trades, uint64_t& trade_seq);

    /**
     * @brief Cancel an existing order by ID.
     * @return OrderResult::Accepted if found and removed, OrderResult::RejectedOrderNotFound otherwise.
     */
    OrderResult cancel(OrderId id);

    /**
     * @brief Modify an existing resting order's price and/or quantity.
     * Semantics:
     *  - If price changes: Order loses priority, removed from old level, re-processed at new price.
     *  - If price same, qty decreases: In-place reduction, priority RETAINED.
     *  - If price same, qty increases: Priority LOST, moved to tail of same price level.
     */
    OrderResult modify(OrderId id, Price new_price, Quantity new_qty,
                       std::vector<Trade>& trades, uint64_t& trade_seq);

    // Book state queries
    [[nodiscard]] bool has_order(OrderId id) const noexcept;
    [[nodiscard]] std::optional<Order> get_order(OrderId id) const;

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] Quantity best_bid_qty() const noexcept;
    [[nodiscard]] Quantity best_ask_qty() const noexcept;

    [[nodiscard]] size_t bid_depth() const noexcept;
    [[nodiscard]] size_t ask_depth() const noexcept;
    [[nodiscard]] size_t total_orders() const noexcept;
    [[nodiscard]] Quantity total_bid_qty() const noexcept;
    [[nodiscard]] Quantity total_ask_qty() const noexcept;

    [[nodiscard]] std::vector<LevelView> get_bid_levels() const;
    [[nodiscard]] std::vector<LevelView> get_ask_levels() const;

    /**
     * @brief Systematically validates all order book invariants.
     * Returns true if all invariants hold; otherwise returns false and populates error_out.
     */
    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;

private:
    struct OrderLocation {
        Side side;
        Price price;
        std::list<Order>::iterator iter;
    };

    // Bids sorted in descending price order (std::greater)
    std::map<Price, PriceLevel, std::greater<Price>> bids_;

    // Asks sorted in ascending price order (std::less)
    std::map<Price, PriceLevel, std::less<Price>> asks_;

    // Fast O(1) order lookup by OrderId
    std::unordered_map<OrderId, OrderLocation> order_lookup_;
};

} // namespace hft
