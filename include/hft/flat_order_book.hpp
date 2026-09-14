#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/order_book.hpp"

#include <vector>
#include <unordered_map>
#include <optional>
#include <string>

namespace hft {

/**
 * @brief Experimental Limit Order Book using contiguous sorted vectors for price levels.
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

    FlatOrderBook(const FlatOrderBook&) = delete;
    FlatOrderBook& operator=(const FlatOrderBook&) = delete;
    FlatOrderBook(FlatOrderBook&&) noexcept = default;
    FlatOrderBook& operator=(FlatOrderBook&&) noexcept = default;

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

    static bool bid_descending_cmp(const PriceLevel& level, Price price) noexcept {
        return level.price > price;
    }
    static bool ask_ascending_cmp(const PriceLevel& level, Price price) noexcept {
        return level.price < price;
    }

    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    std::unordered_map<OrderId, OrderLocation> order_lookup_;
    OrderPool order_pool_;
};

/**
 * @brief Matching Engine wrapping the experimental FlatOrderBook.
 */
class FlatMatchingEngine {
public:
    FlatMatchingEngine() = default;
    ~FlatMatchingEngine() = default;

    FlatMatchingEngine(const FlatMatchingEngine&) = delete;
    FlatMatchingEngine& operator=(const FlatMatchingEngine&) = delete;
    FlatMatchingEngine(FlatMatchingEngine&&) noexcept = default;
    FlatMatchingEngine& operator=(FlatMatchingEngine&&) noexcept = default;

    OrderResult submit_limit_order(OrderId id, Side side, Price price, Quantity qty,
                                  std::vector<Trade>& trades);

    OrderResult cancel_order(OrderId id);

    OrderResult modify_order(OrderId id, Price new_price, Quantity new_qty,
                            std::vector<Trade>& trades);

    void reserve(size_t order_capacity, size_t level_capacity = 1024) {
        book_.reserve(order_capacity, level_capacity);
    }

    [[nodiscard]] const FlatOrderBook& book() const noexcept { return book_; }
    [[nodiscard]] FlatOrderBook& book() noexcept { return book_; }
    [[nodiscard]] uint64_t total_trades_generated() const noexcept { return trade_sequence_; }
    [[nodiscard]] Timestamp current_sequence() const noexcept { return sequence_number_; }

    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const {
        return book_.verify_invariants(error_out);
    }

    void reset();

private:
    FlatOrderBook book_;
    uint64_t trade_sequence_{0};
    Timestamp sequence_number_{0};
};

} // namespace hft
