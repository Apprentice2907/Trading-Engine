#pragma once

#include "hft/event.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/matching_engine.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <emmintrin.h>

namespace hft {

/**
 * @brief Threaded Event Pipeline connecting an external Producer to the Matching Engine via an SPSC Queue.
 *
 * Strict Ownership Model:
 *  - Producer thread ONLY calls enqueue methods on the SpscQueue.
 *  - Consumer thread is the SOLE owner of the MatchingEngine, mutating OrderBook, OrderPool, and trade state.
 *  - Core MatchingEngine remains single-threaded, lock-free, and mutex-free.
 */
class MatchingEnginePipeline {
public:
    static constexpr size_t DEFAULT_QUEUE_CAPACITY = 65536;

    explicit MatchingEnginePipeline(size_t queue_capacity = DEFAULT_QUEUE_CAPACITY,
                                    size_t engine_order_capacity = 65536);

    ~MatchingEnginePipeline();

    // Non-copyable, non-movable
    MatchingEnginePipeline(const MatchingEnginePipeline&) = delete;
    MatchingEnginePipeline& operator=(const MatchingEnginePipeline&) = delete;
    MatchingEnginePipeline(MatchingEnginePipeline&&) = delete;
    MatchingEnginePipeline& operator=(MatchingEnginePipeline&&) = delete;

    /**
     * @brief Start consumer processing thread.
     */
    void start();

    /**
     * @brief Signal stop, drain all remaining events in the queue, and join consumer thread.
     */
    void stop_and_join();

    /**
     * @brief Non-blocking event submission by Producer thread.
     * @return true if enqueued, false if queue is full
     */
    bool enqueue_event(const OrderEvent& ev) noexcept;

    /**
     * @brief Blocking event submission by Producer thread (spins/yields until slot available).
     */
    void enqueue_event_wait(const OrderEvent& ev) noexcept;

    // Direct access to results (safe after stop_and_join())
    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const std::vector<Trade>& trades() const noexcept { return trades_; }
    [[nodiscard]] const std::vector<OrderResult>& results() const noexcept { return results_; }
    [[nodiscard]] uint64_t events_processed() const noexcept { return events_processed_.load(std::memory_order_relaxed); }

private:
    void consumer_loop();
    void process_event(const OrderEvent& ev);

    // Queue storage
    std::unique_ptr<SpscQueue<OrderEvent, DEFAULT_QUEUE_CAPACITY, true>> queue_;

    // Thread management
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> events_processed_{0};
    std::thread consumer_thread_;

    // Engine state owned exclusively by consumer thread
    MatchingEngine engine_;
    std::vector<Trade> trades_;
    std::vector<Trade> step_trades_buf_;
    std::vector<OrderResult> results_;
};

} // namespace hft
