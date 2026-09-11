#include "test_framework.hpp"
#include "hft/event.hpp"
#include "hft/spsc_queue.hpp"

#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

using namespace hft;

// ============================================================================
// Step 4: Single-Threaded Correctness Tests
// ============================================================================

TEST_CASE(SpscQueue_EmptyPop) {
    SpscQueue<OrderEvent, 16> queue;
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(queue.size(), 0ULL);

    OrderEvent ev{};
    ASSERT_FALSE(queue.try_pop(ev));
    ASSERT_TRUE(queue.empty());
}

TEST_CASE(SpscQueue_SinglePushPop) {
    SpscQueue<OrderEvent, 16> queue;
    OrderEvent in = OrderEvent::make_add(101, Side::Buy, 10000, 50);

    ASSERT_TRUE(queue.try_push(in));
    ASSERT_FALSE(queue.empty());
    ASSERT_EQ(queue.size(), 1ULL);

    OrderEvent out{};
    ASSERT_TRUE(queue.try_pop(out));
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(queue.size(), 0ULL);

    ASSERT_TRUE(in == out);
    ASSERT_EQ(out.id, 101ULL);
    ASSERT_EQ(out.price, 10000);
    ASSERT_EQ(out.qty, 50ULL);
}

TEST_CASE(SpscQueue_FIFOOrdering) {
    SpscQueue<OrderEvent, 16> queue;
    for (uint64_t i = 1; i <= 5; ++i) {
        ASSERT_TRUE(queue.try_push(OrderEvent::make_add(i, Side::Buy, 10000 + static_cast<Price>(i), 10)));
    }
    ASSERT_EQ(queue.size(), 5ULL);

    for (uint64_t i = 1; i <= 5; ++i) {
        OrderEvent ev{};
        ASSERT_TRUE(queue.try_pop(ev));
        ASSERT_EQ(ev.id, i);
        ASSERT_EQ(ev.price, 10000 + static_cast<Price>(i));
    }
    ASSERT_TRUE(queue.empty());
}

TEST_CASE(SpscQueue_FillToCapacityAndRejection) {
    constexpr size_t Cap = 8;
    SpscQueue<OrderEvent, Cap> queue;

    for (size_t i = 0; i < Cap; ++i) {
        ASSERT_TRUE(queue.try_push(OrderEvent::make_add(i + 1, Side::Buy, 100, 10)));
    }
    ASSERT_EQ(queue.size(), Cap);

    // Queue is full -> next push must fail without modifying state or allocating
    OrderEvent overflow = OrderEvent::make_add(999, Side::Sell, 200, 20);
    ASSERT_FALSE(queue.try_push(overflow));
    ASSERT_EQ(queue.size(), Cap);

    // Drain one item -> can push again
    OrderEvent popped{};
    ASSERT_TRUE(queue.try_pop(popped));
    ASSERT_EQ(popped.id, 1ULL);
    ASSERT_EQ(queue.size(), Cap - 1);

    ASSERT_TRUE(queue.try_push(overflow));
    ASSERT_EQ(queue.size(), Cap);
}

TEST_CASE(SpscQueue_Wraparound) {
    constexpr size_t Cap = 4;
    SpscQueue<OrderEvent, Cap> queue;

    for (size_t cycle = 0; cycle < 100; ++cycle) {
        for (size_t i = 0; i < Cap; ++i) {
            uint64_t id = cycle * 10 + i;
            ASSERT_TRUE(queue.try_push(OrderEvent::make_add(id, Side::Buy, 100, 10)));
        }
        for (size_t i = 0; i < Cap; ++i) {
            OrderEvent ev{};
            ASSERT_TRUE(queue.try_pop(ev));
            ASSERT_EQ(ev.id, cycle * 10 + i);
        }
        ASSERT_TRUE(queue.empty());
    }
}

TEST_CASE(SpscQueue_AlternatingPushPop) {
    constexpr size_t Cap = 8;
    SpscQueue<OrderEvent, Cap> queue;

    for (uint64_t i = 1; i <= 10000; ++i) {
        ASSERT_TRUE(queue.try_push(OrderEvent::make_add(i, Side::Buy, 100, 10)));
        OrderEvent ev{};
        ASSERT_TRUE(queue.try_pop(ev));
        ASSERT_EQ(ev.id, i);
        ASSERT_TRUE(queue.empty());
    }
}

TEST_CASE(SpscQueue_LargeDeterministicSequence) {
    constexpr size_t Cap = 1024;
    SpscQueue<OrderEvent, Cap> queue;
    constexpr size_t Total = 100000;

    size_t pushed = 0;
    size_t popped = 0;

    while (popped < Total) {
        // Push up to 200 items if space available
        for (size_t k = 0; k < 200 && pushed < Total; ++k) {
            if (!queue.try_push(OrderEvent::make_add(pushed + 1, Side::Buy, 1000, 10))) {
                break;
            }
            ++pushed;
        }

        // Pop up to 150 items
        for (size_t k = 0; k < 150 && popped < pushed; ++k) {
            OrderEvent ev{};
            if (!queue.try_pop(ev)) {
                break;
            }
            ASSERT_EQ(ev.id, popped + 1);
            ++popped;
        }
    }

    ASSERT_EQ(pushed, Total);
    ASSERT_EQ(popped, Total);
    ASSERT_TRUE(queue.empty());
}

// ============================================================================
// Step 5: Concurrent 1P / 1C Stress Test
// ============================================================================

TEST_CASE(SpscQueue_Concurrent1P1C_StressTest) {
    constexpr size_t QueueCapacity = 1024;
    constexpr uint64_t EventCount = 2000000; // 2 Million events

    auto queue = std::make_unique<SpscQueue<OrderEvent, QueueCapacity, true>>();
    std::atomic<bool> producer_done{false};
    std::atomic<bool> test_failed{false};
    std::string failure_reason;

    // Producer Thread
    std::thread producer([&]() {
        for (uint64_t i = 1; i <= EventCount; ++i) {
            OrderEvent ev = OrderEvent::make_add(i, (i % 2 == 0) ? Side::Buy : Side::Sell, 10000 + static_cast<Price>(i % 50), 10);
            while (!queue->try_push(ev)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer Thread
    uint64_t expected_id = 1;
    uint64_t total_received = 0;

    while (total_received < EventCount) {
        OrderEvent ev{};
        if (queue->try_pop(ev)) {
            if (ev.id != expected_id) {
                test_failed.store(true, std::memory_order_relaxed);
                failure_reason = "Out-of-order or corrupted event. Expected " +
                                 std::to_string(expected_id) + " got " + std::to_string(ev.id);
                break;
            }
            ++expected_id;
            ++total_received;
        } else if (producer_done.load(std::memory_order_acquire) && queue->empty()) {
            // Producer is done and queue is empty, but we didn't receive all events -> lost events!
            test_failed.store(true, std::memory_order_relaxed);
            failure_reason = "Lost events! Expected " + std::to_string(EventCount) +
                             " but queue drained at " + std::to_string(total_received);
            break;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();

    ASSERT_FALSE(test_failed.load());
    ASSERT_EQ(total_received, EventCount);
    ASSERT_TRUE(queue->empty());
}
