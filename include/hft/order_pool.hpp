#pragma once

#include "hft/order.hpp"
#include <vector>
#include <cstdint>
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
 * Maintains stable integer indices (OrderIndex) that serve as stable handles without
 * relying on raw heap pointers or iterator invalidation.
 */
class OrderPool {
public:
    explicit OrderPool(size_t initial_capacity = 65536) {
        reserve(initial_capacity);
    }

    ~OrderPool() = default;

    // Non-copyable, movable
    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;
    OrderPool(OrderPool&&) noexcept = default;
    OrderPool& operator=(OrderPool&&) noexcept = default;

    /**
     * @brief Preallocates and chains free slots.
     */
    void reserve(size_t capacity) {
        if (capacity <= nodes_.size()) {
            return;
        }

        const size_t old_size = nodes_.size();
        nodes_.resize(capacity);

        // Chain new slots onto the free list
        for (size_t i = old_size; i < capacity; ++i) {
            nodes_[i].in_use = false;
            nodes_[i].prev = INVALID_INDEX;
            nodes_[i].next = (i + 1 < capacity) ? static_cast<OrderIndex>(i + 1) : free_head_;
        }
        free_head_ = static_cast<OrderIndex>(old_size);
    }

    /**
     * @brief Allocates an order slot in O(1) time and stores the given order.
     */
    OrderIndex allocate(const Order& order) {
        if (free_head_ == INVALID_INDEX) {
            // Expand pool capacity
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

    /**
     * @brief Returns an order slot to the free list in O(1) time.
     */
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

    [[nodiscard]] size_t capacity() const noexcept { return nodes_.size(); }
    [[nodiscard]] size_t size() const noexcept { return allocated_count_; }
    [[nodiscard]] bool empty() const noexcept { return allocated_count_ == 0; }

    /**
     * @brief Reinitializes all slots to free list without reallocating underlying storage.
     */
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

} // namespace hft
