#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace hft {

constexpr size_t HARDWARE_CACHE_LINE_SIZE = 64;

/**
 * @brief High-performance bounded Single-Producer / Single-Consumer (SPSC) lock-free ring buffer.
 *
 * Implements acquire-release memory ordering, local index caching to minimize cross-core bus invalidation,
 * and optional 64-byte cache-line separation to eliminate false sharing.
 *
 * @tparam T Element type (preferably trivially copyable, e.g. OrderEvent)
 * @tparam Capacity Power-of-two queue capacity
 * @tparam CacheAligned When true, separates producer and consumer indices onto distinct cache lines
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // structure was padded due to alignment specifier (intended for false-sharing avoidance)
#endif

template <typename T, size_t Capacity, bool CacheAligned = true>
class SpscQueue {
    static_assert((Capacity >= 2) && ((Capacity & (Capacity - 1)) == 0),
                  "SpscQueue capacity must be a power of two >= 2");

public:
    using value_type = T;

    explicit SpscQueue()
        : buffer_(std::make_unique<T[]>(Capacity)) {}

    ~SpscQueue() = default;

    // Non-copyable, non-movable (pinned thread-safe queue boundary)
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    /**
     * @brief Pushes an item into the queue.
     * Can only be called by the single PRODUCER thread.
     *
     * @param item Item to copy
     * @return true if pushed, false if queue is full (zero overwrite, zero allocation)
     */
    bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        // Check if queue appears full against locally cached tail
        if (current_head - cached_tail_ >= Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (current_head - cached_tail_ >= Capacity) {
                return false; // Queue is full
            }
        }

        buffer_[current_head & MASK] = item;
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Moves an item into the queue.
     * Can only be called by the single PRODUCER thread.
     */
    bool try_push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t current_head = head_.load(std::memory_order_relaxed);

        if (current_head - cached_tail_ >= Capacity) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (current_head - cached_tail_ >= Capacity) {
                return false;
            }
        }

        buffer_[current_head & MASK] = std::move(item);
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pops an item from the queue.
     * Can only be called by the single CONSUMER thread.
     *
     * @param item Output reference receiving the popped element
     * @return true if popped, false if queue is empty
     */
    bool try_pop(T& item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check if queue appears empty against locally cached head
        if (cached_head_ == current_tail) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (cached_head_ == current_tail) {
                return false; // Queue is empty
            }
        }

        item = std::move(buffer_[current_tail & MASK]);
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Approximate current count of elements in the queue.
     */
    [[nodiscard]] size_t size() const noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return (h >= t) ? (h - t) : 0;
    }

    /**
     * @brief Checks if the queue is empty.
     */
    [[nodiscard]] bool empty() const noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return h == t;
    }

    /**
     * @brief Returns the fixed capacity of the queue.
     */
    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    static constexpr size_t ALIGNMENT = CacheAligned ? HARDWARE_CACHE_LINE_SIZE : alignof(std::atomic<size_t>);

    // Producer state (written by Producer only)
    alignas(ALIGNMENT) std::atomic<size_t> head_{0};
    size_t cached_tail_{0};

    // Consumer state (written by Consumer only)
    alignas(ALIGNMENT) std::atomic<size_t> tail_{0};
    size_t cached_head_{0};

    // Storage buffer
    alignas(ALIGNMENT) std::unique_ptr<T[]> buffer_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace hft
