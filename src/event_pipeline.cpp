#include "hft/event_pipeline.hpp"

namespace hft {

MatchingEnginePipeline::MatchingEnginePipeline(size_t /*queue_capacity*/,
                                               size_t engine_order_capacity)
    : queue_(std::make_unique<SpscQueue<OrderEvent, DEFAULT_QUEUE_CAPACITY, true>>()) {
    engine_.reserve(engine_order_capacity);
    trades_.reserve(engine_order_capacity);
    step_trades_buf_.reserve(128);
    results_.reserve(engine_order_capacity);
}

MatchingEnginePipeline::~MatchingEnginePipeline() {
    stop_and_join();
}

void MatchingEnginePipeline::start() {
    if (!running_.load(std::memory_order_acquire)) {
        running_.store(true, std::memory_order_release);
        consumer_thread_ = std::thread(&MatchingEnginePipeline::consumer_loop, this);
    }
}

void MatchingEnginePipeline::stop_and_join() {
    if (consumer_thread_.joinable()) {
        running_.store(false, std::memory_order_release);
        consumer_thread_.join();
    }
}

bool MatchingEnginePipeline::enqueue_event(const OrderEvent& ev) noexcept {
    return queue_->try_push(ev);
}

void MatchingEnginePipeline::enqueue_event_wait(const OrderEvent& ev) noexcept {
    while (!queue_->try_push(ev)) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }
}

void MatchingEnginePipeline::consumer_loop() {
    while (running_.load(std::memory_order_relaxed) || !queue_->empty()) {
        OrderEvent ev{};
        if (queue_->try_pop(ev)) {
            process_event(ev);
            events_processed_.fetch_add(1, std::memory_order_relaxed);
        } else {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }
}

void MatchingEnginePipeline::process_event(const OrderEvent& ev) {
    step_trades_buf_.clear();
    OrderResult res = OrderResult::RejectedUnchanged;

    switch (ev.type) {
        case EventType::Add:
            res = engine_.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, step_trades_buf_);
            break;
        case EventType::Cancel:
            res = engine_.cancel_order(ev.id);
            break;
        case EventType::Modify:
            res = engine_.modify_order(ev.id, ev.price, ev.qty, step_trades_buf_);
            break;
    }

    results_.push_back(res);
    for (const auto& trade : step_trades_buf_) {
        trades_.push_back(trade);
    }
}

} // namespace hft
