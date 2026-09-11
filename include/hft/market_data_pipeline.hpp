#pragma once

#include "hft/market_event.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/broker/angel_types.hpp"

#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <optional>

namespace hft {

/**
 * @brief Threaded SPSC pipeline for real-time market data ingestion and observation.
 *
 * Implements strict single-producer / single-consumer boundary:
 * - Producer thread pushes normalized MarketEvents with non-blocking backpressure.
 * - Consumer thread owns mutable observer state and forwards to the binary recorder.
 */
class MarketDataPipeline {
public:
    static constexpr size_t DEFAULT_QUEUE_CAPACITY = 16384; // 16K events (~1 MB buffer)

    using MarketEventListener = std::function<void(const MarketEvent&)>;

    explicit MarketDataPipeline(size_t capacity = DEFAULT_QUEUE_CAPACITY);
    ~MarketDataPipeline();

    // Non-copyable, non-movable
    MarketDataPipeline(const MarketDataPipeline&) = delete;
    MarketDataPipeline& operator=(const MarketDataPipeline&) = delete;
    MarketDataPipeline(MarketDataPipeline&&) = delete;
    MarketDataPipeline& operator=(MarketDataPipeline&&) = delete;

    /**
     * @brief Starts the background consumer thread.
     */
    void start();

    /**
     * @brief Stops consumer thread and flushes remaining queue elements.
     */
    void stop_and_join();

    /**
     * @brief Producer enqueue method (called from network/broker thread).
     * Non-blocking: if queue is full, event is dropped and drop counter incremented.
     *
     * @param ev Normalized MarketEvent
     * @return true if successfully queued, false if dropped due to backpressure
     */
    bool enqueue_event(const MarketEvent& ev) noexcept;

    /**
     * @brief Producer enqueue method with spin-wait (useful for zero-drop simulation/benchmarks).
     */
    void enqueue_event_wait(const MarketEvent& ev) noexcept;

    /**
     * @brief Registers consumer listener callback invoked on each popped MarketEvent.
     */
    void set_event_listener(MarketEventListener listener) {
        listener_ = std::move(listener);
    }

    // Pipeline status and stats inspection
    [[nodiscard]] uint64_t total_received() const noexcept {
        return stats_.received_packets.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t total_normalized() const noexcept {
        return stats_.normalized_events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t total_queued() const noexcept {
        return stats_.queued_events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t total_dropped() const noexcept {
        return stats_.dropped_events.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t total_consumed() const noexcept {
        return consumed_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::optional<MarketEvent> latest_event() const noexcept;
    [[nodiscard]] broker::BrokerStats& stats() noexcept { return stats_; }

private:
    void consumer_worker();

    SpscQueue<MarketEvent, DEFAULT_QUEUE_CAPACITY, true> queue_;
    broker::BrokerStats stats_;
    std::atomic<uint64_t> consumed_count_{0};
    std::atomic<bool> running_{false};

    MarketEventListener listener_;
    std::unique_ptr<std::thread> consumer_thread_;

    // Observer state protected by cache-aligned padding
    alignas(64) mutable std::atomic<uint64_t> latest_seq_{0};
    mutable MarketEvent latest_event_{};
};

} // namespace hft
