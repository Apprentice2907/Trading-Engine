#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/order_pool.hpp"
#include "hft/order_book.hpp"

#include <vector>
#include <unordered_map>
#include <optional>
#include <string>

namespace hft {

/**
 * @brief Experimental Limit Order Book using contiguous sorted vectors for price levels.
 *
 * Designed to maximize cache locality and hardware prefetching during order matching
 * and depth sweeps, while eliminating Red-Black tree node allocations.
 *
 * Best bid and best ask are available at index 0 (O(1)).
 * Price lookups use binary search (std::lower_bound) on contiguous cache lines.
 */
class FlatOrderBook {
public:
    explicit FlatOrderBook(size_t initial_order_capacity = 65536,
                           size_t initial_level_capacity = 1024)
        : order_pool_(initial_order_capacity) {
        order_lookup_.reserve(initial_order_capacity);
        bids_.reserve(initial_level_capacity);
        asks_.reserve(initial_level_capacity);
    }

    ~FlatOrderBook() = default;

    // Non-copyable, movable
    FlatOrderBook(const FlatOrderBook&) = delete;
    FlatOrderBook& operator=(const FlatOrderBook&) = delete;
    FlatOrderBook(FlatOrderBook&&) noexcept = default;
    FlatOrderBook& operator=(FlatOrderBook&&) noexcept = default;

    /**
     * @brief Pre-reserves capacity for orders, hash lookup table, and price levels.
     */
    void reserve(size_t order_capacity, size_t level_capacity = 1024) {
        order_pool_.reserve(order_capacity);
        order_lookup_.reserve(order_capacity);
        bids_.reserve(level_capacity);
        asks_.reserve(level_capacity);
    }

    OrderResult add_resting_order(const Order& order);
    size_t match(Order& incoming, std::vector<Trade>& trades, uint64_t& trade_seq);
    OrderResult process_order(Order incoming, std::vector<Trade>& trades, uint64_t& trade_seq);
    OrderResult cancel(OrderId id);
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

    [[nodiscard]] const OrderPool& pool() const noexcept { return order_pool_; }

    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;

private:
    struct OrderLocation {
        Side side{Side::Buy};
        Price price{0};
        OrderIndex pool_index{INVALID_INDEX};
    };

    void detach_order_from_level(PriceLevel& level, OrderIndex idx) noexcept;
    void append_order_to_level(PriceLevel& level, OrderIndex idx) noexcept;

    // Fast contiguous binary search helpers
    static bool bid_descending_cmp(const PriceLevel& level, Price price) noexcept {
        return level.price > price;
    }
    static bool ask_ascending_cmp(const PriceLevel& level, Price price) noexcept {
        return level.price < price;
    }

    // Contiguous sorted price levels
    // Bids sorted strictly descending (best bid at index 0)
    std::vector<PriceLevel> bids_;

    // Asks sorted strictly ascending (best ask at index 0)
    std::vector<PriceLevel> asks_;

    // Fast O(1) order lookup by OrderId
    std::unordered_map<OrderId, OrderLocation> order_lookup_;

    // Preallocated contiguous order storage
    OrderPool order_pool_;
};

} // namespace hft
