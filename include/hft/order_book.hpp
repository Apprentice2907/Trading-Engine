#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"

#include <map>
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>
#include <limits>
#include <stdexcept>

namespace hft {

using OrderIndex = uint32_t;
inline constexpr OrderIndex INVALID_INDEX = std::numeric_limits<OrderIndex>::max();

/**
 * @brief Contiguous pool node encapsulating order storage and intrusive list pointers.
 */
struct PoolOrderNode {
    Order order;
    OrderIndex prev{INVALID_INDEX};
    OrderIndex next{INVALID_INDEX};
    bool in_use{false};
};

/**
 * @brief Preallocated contiguous memory pool for orders.
 *
 * Eliminates per-order dynamic heap allocations by recycling slots via an O(1) free list.
 */
class OrderPool {
public:
    explicit OrderPool(size_t initial_capacity = 65536) {
        reserve(initial_capacity);
    }

    ~OrderPool() = default;

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;
    OrderPool(OrderPool&&) noexcept = default;
    OrderPool& operator=(OrderPool&&) noexcept = default;

    void reserve(size_t capacity) {
        if (capacity <= nodes_.size()) {
            return;
        }

        const size_t old_size = nodes_.size();
        nodes_.resize(capacity);

        for (size_t i = old_size; i < capacity; ++i) {
            nodes_[i].in_use = false;
            nodes_[i].prev = INVALID_INDEX;
            nodes_[i].next = (i + 1 < capacity) ? static_cast<OrderIndex>(i + 1) : free_head_;
        }
        free_head_ = static_cast<OrderIndex>(old_size);
    }

    OrderIndex allocate(const Order& order) {
        if (free_head_ == INVALID_INDEX) {
            const size_t new_cap = (nodes_.empty()) ? 65536 : nodes_.size() * 2;
            reserve(new_cap);
        }

        const OrderIndex allocated_idx = free_head_;
        PoolOrderNode& node = nodes_[allocated_idx];
        free_head_ = node.next;

        node.order = order;
        node.prev = INVALID_INDEX;
        node.next = INVALID_INDEX;
        node.in_use = true;

        ++allocated_count_;
        return allocated_idx;
    }

    void deallocate(OrderIndex idx) noexcept {
        if (idx >= nodes_.size() || !nodes_[idx].in_use) {
            return;
        }

        PoolOrderNode& node = nodes_[idx];
        node.in_use = false;
        node.prev = INVALID_INDEX;
        node.next = free_head_;
        free_head_ = idx;

        --allocated_count_;
    }

    [[nodiscard]] PoolOrderNode& get(OrderIndex idx) noexcept {
        return nodes_[idx];
    }

    [[nodiscard]] const PoolOrderNode& get(OrderIndex idx) const noexcept {
        return nodes_[idx];
    }

    [[nodiscard]] Order& order(OrderIndex idx) noexcept {
        return nodes_[idx].order;
    }

    [[nodiscard]] const Order& order(OrderIndex idx) const noexcept {
        return nodes_[idx].order;
    }

    [[nodiscard]] size_t size() const noexcept { return allocated_count_; }
    [[nodiscard]] size_t capacity() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return allocated_count_ == 0; }

    void clear() noexcept {
        allocated_count_ = 0;
        const size_t cap = nodes_.size();
        for (size_t i = 0; i < cap; ++i) {
            nodes_[i].in_use = false;
            nodes_[i].prev = INVALID_INDEX;
            nodes_[i].next = (i + 1 < cap) ? static_cast<OrderIndex>(i + 1) : INVALID_INDEX;
        }
        free_head_ = (cap > 0) ? 0 : INVALID_INDEX;
    }

private:
    std::vector<PoolOrderNode> nodes_;
    OrderIndex free_head_{INVALID_INDEX};
    size_t allocated_count_{0};
};

/**
 * @brief Represents an aggregated price level with an intrusive FIFO queue of orders.
 */
struct PriceLevel {
    Price price{0};
    Quantity total_quantity{0};
    OrderIndex head{INVALID_INDEX};
    OrderIndex tail{INVALID_INDEX};
    size_t count{0};
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
 * @brief Memory-optimized deterministic Limit Order Book using OrderPool.
 */
class OrderBook {
public:
    explicit OrderBook(size_t initial_capacity = 65536)
        : order_pool_(initial_capacity) {
        order_lookup_.reserve(initial_capacity);
    }

    ~OrderBook() = default;

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;

    void reserve(size_t order_capacity) {
        order_pool_.reserve(order_capacity);
        order_lookup_.reserve(order_capacity);
    }

    OrderResult add_resting_order(const Order& order);
    size_t match(Order& incoming, std::vector<Trade>& trades, uint64_t& trade_seq);
    OrderResult process_order(Order incoming, std::vector<Trade>& trades, uint64_t& trade_seq);
    OrderResult cancel(OrderId id);
    OrderResult modify(OrderId id, Price new_price, Quantity new_qty,
                       std::vector<Trade>& trades, uint64_t& trade_seq);

    // Convenience API for direct OrderBook testing / usage
    OrderResult add_order(const Order& order) {
        return add_resting_order(order);
    }
    OrderResult cancel_order(OrderId id) {
        return cancel(id);
    }
    OrderResult modify_order(OrderId id, Price new_price, Quantity new_qty) {
        std::vector<Trade> dummy_trades;
        uint64_t dummy_seq = 0;
        return modify(id, new_price, new_qty, dummy_trades, dummy_seq);
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] Quantity best_bid_qty() const noexcept;
    [[nodiscard]] Quantity best_ask_qty() const noexcept;

    [[nodiscard]] bool has_order(OrderId id) const noexcept;
    [[nodiscard]] std::optional<Order> get_order(OrderId id) const;

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

    // Bids sorted descending
    std::map<Price, PriceLevel, std::greater<Price>> bids_;

    // Asks sorted ascending
    std::map<Price, PriceLevel, std::less<Price>> asks_;

    // Fast O(1) order lookup by OrderId
    std::unordered_map<OrderId, OrderLocation> order_lookup_;

    // Preallocated contiguous order storage
    OrderPool order_pool_;
};

using MapOrderBook = OrderBook;

} // namespace hft
