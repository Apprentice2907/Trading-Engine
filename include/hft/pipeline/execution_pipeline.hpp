#pragma once

#include "hft/order_command.hpp"
#include "hft/execution_report.hpp"
#include "hft/risk/risk_engine.hpp"
#include "hft/gateway/order_gateway.hpp"
#include "hft/matching_engine.hpp"
#include "hft/spsc_queue.hpp"

#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>
#include <emmintrin.h>

namespace hft {

/**
 * @brief Threaded Execution Pipeline connecting Producer and Consumer via dual lock-free SPSC queues.
 *
 * Strict Ownership Model:
 *  - Producer thread submits OrderCommands into ingress SpscQueue.
 *  - Execution worker thread exclusively owns PreTradeRiskEngine, OrderGateway, and MatchingEngine.
 *  - Consumer thread polls ExecutionReports from egress SpscQueue.
 *  - Core MatchingEngine remains 100% single-threaded with zero mutexes or locks.
 */
class OrderExecutionPipeline {
public:
    static constexpr size_t DEFAULT_QUEUE_CAPACITY = 65536;

    explicit OrderExecutionPipeline(size_t queue_capacity = DEFAULT_QUEUE_CAPACITY,
                                    RiskConfig risk_config = RiskConfig{});
    ~OrderExecutionPipeline();

    // Non-copyable, non-movable
    OrderExecutionPipeline(const OrderExecutionPipeline&) = delete;
    OrderExecutionPipeline& operator=(const OrderExecutionPipeline&) = delete;
    OrderExecutionPipeline(OrderExecutionPipeline&&) = delete;
    OrderExecutionPipeline& operator=(OrderExecutionPipeline&&) = delete;

    /**
     * @brief Starts the background execution worker thread.
     */
    void start();

    /**
     * @brief Signals stop, drains all remaining ingress orders, and joins the worker thread.
     */
    void stop_and_join();

    /**
     * @brief Producer API: Non-blocking order submission.
     * @return true if successfully enqueued; false if ingress queue is full.
     */
    bool submit_order(const OrderCommand& cmd) noexcept;

    /**
     * @brief Producer API: Blocking/spinning order submission.
     */
    void submit_order_wait(const OrderCommand& cmd) noexcept;

    /**
     * @brief Consumer API: Non-blocking execution report polling.
     * @return true if report popped; false if egress queue is empty.
     */
    bool poll_execution(ExecutionReport& rep) noexcept;

    // Direct access to state (safe when thread is stopped or after stop_and_join)
    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const PreTradeRiskEngine& risk() const noexcept { return risk_; }
    [[nodiscard]] const OrderGateway& gateway() const noexcept { return gateway_; }

    // Metrics
    [[nodiscard]] uint64_t orders_submitted() const noexcept { return orders_submitted_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t orders_processed() const noexcept { return orders_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t executions_emitted() const noexcept { return executions_emitted_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_ingress() const noexcept { return dropped_ingress_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_egress() const noexcept { return dropped_egress_.load(std::memory_order_relaxed); }

private:
    void execution_loop();
    void process_one_command(const OrderCommand& cmd);

    // Lock-Free SPSC Queues
    std::unique_ptr<SpscQueue<OrderCommand, DEFAULT_QUEUE_CAPACITY, true>> ingress_queue_;
    std::unique_ptr<SpscQueue<ExecutionReport, DEFAULT_QUEUE_CAPACITY, true>> egress_queue_;

    // Concurrency Controls
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> orders_submitted_{0};
    std::atomic<uint64_t> orders_processed_{0};
    std::atomic<uint64_t> executions_emitted_{0};
    std::atomic<uint64_t> dropped_ingress_{0};
    std::atomic<uint64_t> dropped_egress_{0};
    std::thread worker_thread_;

    // Exclusively owned by execution worker thread
    PreTradeRiskEngine risk_;
    OrderGateway gateway_;
    MatchingEngine engine_;
};

} // namespace hft
