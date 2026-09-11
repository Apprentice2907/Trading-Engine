#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"

#include <map>
#include <unordered_map>
#include <list>
#include <vector>
#include <optional>
#include <string>

namespace hft::baseline {

struct PriceLevel {
    Price price{0};
    Quantity total_quantity{0};
    std::list<Order> orders;
};

struct LevelView {
    Price price{0};
    Quantity total_quantity{0};
    size_t order_count{0};
};

class OrderBook {

public:
    OrderBook() = default;
    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

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

    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const;

private:
    struct OrderLocation {
        Side side;
        Price price;
        std::list<Order>::iterator iter;
    };

    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>> asks_;
    std::unordered_map<OrderId, OrderLocation> order_lookup_;
};

} // namespace hft::baseline
