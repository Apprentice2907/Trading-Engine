#include "hft/market_data_pipeline.hpp"
#include <immintrin.h>

namespace hft {

MarketDataPipeline::MarketDataPipeline(size_t /*capacity*/) {
    // SpscQueue template uses DEFAULT_QUEUE_CAPACITY
}

MarketDataPipeline::~MarketDataPipeline() {
    stop_and_join();
}

void MarketDataPipeline::start() {
    if (running_.load(std::memory_order_relaxed)) return;

    running_.store(true, std::memory_order_release);
    consumer_thread_ = std::make_unique<std::thread>(&MarketDataPipeline::consumer_worker, this);
}

void MarketDataPipeline::stop_and_join() {
    if (!running_.load(std::memory_order_relaxed)) return;

    running_.store(false, std::memory_order_release);
    if (consumer_thread_ && consumer_thread_->joinable()) {
        consumer_thread_->join();
        consumer_thread_.reset();
    }
}

bool MarketDataPipeline::enqueue_event(const MarketEvent& ev) noexcept {
    stats_.normalized_events.fetch_add(1, std::memory_order_relaxed);
    if (queue_.try_push(ev)) {
        stats_.queued_events.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    stats_.dropped_events.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void MarketDataPipeline::enqueue_event_wait(const MarketEvent& ev) noexcept {
    stats_.normalized_events.fetch_add(1, std::memory_order_relaxed);
    while (!queue_.try_push(ev)) {
#if defined(_MSC_VER)
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#else
        std::this_thread::yield();
#endif
    }
    stats_.queued_events.fetch_add(1, std::memory_order_relaxed);
}

std::optional<MarketEvent> MarketDataPipeline::latest_event() const noexcept {
    if (latest_seq_.load(std::memory_order_acquire) == 0) {
        return std::nullopt;
    }
    return latest_event_;
}

void MarketDataPipeline::consumer_worker() {
    MarketEvent ev{};

    while (running_.load(std::memory_order_relaxed)) {
        if (queue_.try_pop(ev)) {
            consumed_count_.fetch_add(1, std::memory_order_relaxed);
            latest_event_ = ev;
            latest_seq_.store(ev.sequence_number, std::memory_order_release);
            if (listener_) {
                listener_(ev);
            }
        } else {
#if defined(_MSC_VER)
            _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#else
            std::this_thread::yield();
#endif
        }
    }

    // Drain remaining buffered events upon shutdown
    while (queue_.try_pop(ev)) {
        consumed_count_.fetch_add(1, std::memory_order_relaxed);
        latest_event_ = ev;
        latest_seq_.store(ev.sequence_number, std::memory_order_release);
        if (listener_) {
            listener_(ev);
        }
    }
}

} // namespace hft
