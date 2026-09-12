#include "hft/pipeline/execution_pipeline.hpp"

namespace hft {

OrderExecutionPipeline::OrderExecutionPipeline(size_t queue_capacity, RiskConfig risk_config)
    : ingress_queue_(std::make_unique<SpscQueue<OrderCommand, DEFAULT_QUEUE_CAPACITY, true>>()),
      egress_queue_(std::make_unique<SpscQueue<ExecutionReport, DEFAULT_QUEUE_CAPACITY, true>>()),
      risk_(risk_config),
      gateway_() {
    (void)queue_capacity;
}

OrderExecutionPipeline::~OrderExecutionPipeline() {
    if (running_.load(std::memory_order_relaxed)) {
        stop_and_join();
    }
}

void OrderExecutionPipeline::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // Already running
    }
    worker_thread_ = std::thread(&OrderExecutionPipeline::execution_loop, this);
}

void OrderExecutionPipeline::stop_and_join() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // Already stopped
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool OrderExecutionPipeline::submit_order(const OrderCommand& cmd) noexcept {
    if (ingress_queue_->try_push(cmd)) {
        orders_submitted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    dropped_ingress_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void OrderExecutionPipeline::submit_order_wait(const OrderCommand& cmd) noexcept {
    while (!ingress_queue_->try_push(cmd)) {
        _mm_pause();
    }
    orders_submitted_.fetch_add(1, std::memory_order_relaxed);
}

bool OrderExecutionPipeline::poll_execution(ExecutionReport& rep) noexcept {
    return egress_queue_->try_pop(rep);
}

void OrderExecutionPipeline::execution_loop() {
    OrderCommand cmd{};
    while (running_.load(std::memory_order_relaxed)) {
        if (ingress_queue_->try_pop(cmd)) {
            process_one_command(cmd);
        } else {
            _mm_pause();
        }
    }

    // Drain all remaining orders before exiting
    while (ingress_queue_->try_pop(cmd)) {
        process_one_command(cmd);
    }
}

void OrderExecutionPipeline::process_one_command(const OrderCommand& cmd) {
    orders_processed_.fetch_add(1, std::memory_order_relaxed);
    gateway_.process_command(cmd, risk_, engine_, [this](const ExecutionReport& rep) {
        if (egress_queue_->try_push(rep)) {
            executions_emitted_.fetch_add(1, std::memory_order_relaxed);
        } else {
            dropped_egress_.fetch_add(1, std::memory_order_relaxed);
        }
    });
}

} // namespace hft
