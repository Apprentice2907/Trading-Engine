#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/matching_engine.hpp"
#include "hft/spsc_queue.hpp"

#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>
#include <string_view>
#include <emmintrin.h>

namespace hft {

/**
 * @brief Lightweight risk evaluation result.
 */
struct RiskResult {
    RiskCode code{RiskCode::Approved};

    [[nodiscard]] constexpr bool approved() const noexcept {
        return code == RiskCode::Approved;
    }

    constexpr explicit operator bool() const noexcept {
        return approved();
    }
};

/**
 * @brief Configuration parameters for pre-trade risk thresholds.
 */
struct RiskConfig {
    Quantity max_order_quantity{100'000};
    uint64_t max_order_notional{50'000'000'00};
    Price    min_price{1};
    Price    max_price{100'000'00};
    Quantity max_exposure_quantity{500'000};
    uint32_t allowed_instrument_id{0};
};

/**
 * @brief Zero-allocation risk engine counters for monitoring and audit.
 */
struct RiskStats {
    uint64_t orders_checked{0};
    uint64_t orders_approved{0};
    uint64_t orders_rejected{0};
    uint64_t reject_invalid_side{0};
    uint64_t reject_invalid_price{0};
    uint64_t reject_invalid_quantity{0};
    uint64_t reject_invalid_instrument{0};
    uint64_t reject_max_quantity{0};
    uint64_t reject_max_notional{0};
    uint64_t reject_price_band{0};
    uint64_t reject_exposure_limit{0};

    void reset() noexcept {
        *this = RiskStats{};
    }
};

/**
 * @brief Zero-allocation wire-speed pre-trade risk engine.
 */
class PreTradeRiskEngine {
public:
    explicit PreTradeRiskEngine(RiskConfig config = RiskConfig{}) noexcept;
    ~PreTradeRiskEngine() = default;

    PreTradeRiskEngine(const PreTradeRiskEngine&) = delete;
    PreTradeRiskEngine& operator=(const PreTradeRiskEngine&) = delete;
    PreTradeRiskEngine(PreTradeRiskEngine&&) noexcept = default;
    PreTradeRiskEngine& operator=(PreTradeRiskEngine&&) noexcept = default;

    RiskResult check_order(const OrderCommand& cmd) noexcept;
    void on_order_approved(const OrderCommand& cmd) noexcept;
    void on_order_filled(Quantity fill_qty) noexcept;
    void on_order_cancelled(Quantity cancelled_qty) noexcept;
    void reset() noexcept;

    [[nodiscard]] const RiskConfig& config() const noexcept { return config_; }
    void set_config(const RiskConfig& cfg) noexcept { config_ = cfg; }
    [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }
    [[nodiscard]] Quantity current_exposure() const noexcept { return current_exposure_quantity_; }

private:
    RiskConfig config_;
    RiskStats  stats_{};
    Quantity   current_exposure_quantity_{0};
};

/**
 * @brief Translates OrderCommands into MatchingEngine operations and produces ExecutionReports.
 */
class OrderGateway {
public:
    static constexpr size_t DEFAULT_TRADE_BUFFER_CAPACITY = 256;

    explicit OrderGateway(size_t trade_buffer_capacity = DEFAULT_TRADE_BUFFER_CAPACITY);
    ~OrderGateway() = default;

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;
    OrderGateway(OrderGateway&&) noexcept = default;
    OrderGateway& operator=(OrderGateway&&) noexcept = default;

    template <typename Emitter>
    void process_command(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                         MatchingEngine& engine, Emitter&& emitter) {
        const RiskResult risk_res = risk.check_order(cmd);
        if (!risk_res.approved()) {
            ++exec_sequence_;
            emitter(ExecutionReport::make_risk_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, risk_res.code,
                exec_sequence_, cmd.timestamp));
            return;
        }

        if (cmd.type == EventType::Add) {
            process_add(cmd, risk, engine, emitter);
        } else if (cmd.type == EventType::Cancel) {
            process_cancel(cmd, risk, engine, emitter);
        } else if (cmd.type == EventType::Modify) {
            process_modify(cmd, risk, engine, emitter);
        }
    }

    void process_command(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                         MatchingEngine& engine, std::vector<ExecutionReport>& reports_out);

    [[nodiscard]] uint64_t total_executions() const noexcept { return exec_sequence_; }
    void reset() noexcept {
        exec_sequence_ = 0;
        trades_buf_.clear();
    }

private:
    template <typename Emitter>
    void process_add(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                     MatchingEngine& engine, Emitter&& emitter) {
        trades_buf_.clear();
        const OrderResult res = engine.submit_limit_order(cmd.order_id, cmd.side,
                                                          cmd.price, cmd.qty, trades_buf_);

        if (res == OrderResult::Accepted) {
            if (trades_buf_.empty()) {
                risk.on_order_approved(cmd);
                ++exec_sequence_;
                emitter(ExecutionReport::make_new(
                    cmd.order_id, cmd.instrument_id, cmd.client_id,
                    cmd.side, cmd.price, cmd.qty, exec_sequence_, cmd.timestamp));
            } else {
                Quantity total_filled = 0;
                for (const auto& trade : trades_buf_) {
                    total_filled += trade.quantity;
                    const Quantity leaves = (cmd.qty >= total_filled) ? (cmd.qty - total_filled) : 0;
                    ++exec_sequence_;
                    emitter(ExecutionReport::make_trade(
                        cmd.order_id, cmd.instrument_id, cmd.client_id,
                        cmd.side, trade.price, trade.quantity, leaves,
                        exec_sequence_, cmd.timestamp));
                }

                const Quantity remaining_leaves = (cmd.qty >= total_filled) ? (cmd.qty - total_filled) : 0;
                if (remaining_leaves > 0) {
                    OrderCommand resting_cmd = cmd;
                    resting_cmd.qty = remaining_leaves;
                    risk.on_order_approved(resting_cmd);
                }
            }
        } else {
            ++exec_sequence_;
            emitter(ExecutionReport::make_engine_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, res, exec_sequence_, cmd.timestamp));
        }
    }

    template <typename Emitter>
    void process_cancel(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                        MatchingEngine& engine, Emitter&& emitter) {
        const auto order_opt = engine.book().get_order(cmd.order_id);
        const Quantity cancelled_qty = order_opt ? order_opt->remaining_qty : 0;
        const OrderResult res = engine.cancel_order(cmd.order_id);

        if (res == OrderResult::Accepted) {
            risk.on_order_cancelled(cancelled_qty);
            ++exec_sequence_;
            emitter(ExecutionReport::make_cancelled(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                order_opt ? order_opt->side : cmd.side, cancelled_qty,
                exec_sequence_, cmd.timestamp));
        } else {
            ++exec_sequence_;
            emitter(ExecutionReport::make_engine_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, 0, 0, res, exec_sequence_, cmd.timestamp));
        }
    }

    template <typename Emitter>
    void process_modify(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                        MatchingEngine& engine, Emitter&& emitter) {
        trades_buf_.clear();
        const OrderResult res = engine.modify_order(cmd.order_id, cmd.price, cmd.qty, trades_buf_);

        if (res == OrderResult::Accepted) {
            ++exec_sequence_;
            emitter(ExecutionReport::make_modified(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, exec_sequence_, cmd.timestamp));
        } else {
            ++exec_sequence_;
            emitter(ExecutionReport::make_engine_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, res, exec_sequence_, cmd.timestamp));
        }
    }

    uint64_t exec_sequence_{0};
    std::vector<Trade> trades_buf_;
};

/**
 * @brief Threaded execution pipeline connecting Producer and Consumer via dual SPSC queues.
 */
class OrderExecutionPipeline {
public:
    static constexpr size_t DEFAULT_QUEUE_CAPACITY = 65536;

    explicit OrderExecutionPipeline(size_t queue_capacity = DEFAULT_QUEUE_CAPACITY,
                                    RiskConfig risk_config = RiskConfig{});
    ~OrderExecutionPipeline();

    OrderExecutionPipeline(const OrderExecutionPipeline&) = delete;
    OrderExecutionPipeline& operator=(const OrderExecutionPipeline&) = delete;
    OrderExecutionPipeline(OrderExecutionPipeline&&) = delete;
    OrderExecutionPipeline& operator=(OrderExecutionPipeline&&) = delete;

    void start();
    void stop_and_join();

    bool submit_order(const OrderCommand& cmd) noexcept;
    void submit_order_wait(const OrderCommand& cmd) noexcept;
    bool poll_execution(ExecutionReport& rep) noexcept;

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const PreTradeRiskEngine& risk() const noexcept { return risk_; }
    [[nodiscard]] const OrderGateway& gateway() const noexcept { return gateway_; }

    [[nodiscard]] uint64_t orders_submitted() const noexcept { return orders_submitted_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t orders_processed() const noexcept { return orders_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t executions_emitted() const noexcept { return executions_emitted_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_ingress() const noexcept { return dropped_ingress_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t dropped_egress() const noexcept { return dropped_egress_.load(std::memory_order_relaxed); }

private:
    void execution_loop();
    void process_one_command(const OrderCommand& cmd);

    std::unique_ptr<SpscQueue<OrderCommand, DEFAULT_QUEUE_CAPACITY, true>> ingress_queue_;
    std::unique_ptr<SpscQueue<ExecutionReport, DEFAULT_QUEUE_CAPACITY, true>> egress_queue_;

    PreTradeRiskEngine risk_;
    OrderGateway       gateway_;
    MatchingEngine     engine_;

    std::thread        worker_thread_;
    std::atomic<bool>  running_{false};

    alignas(64) std::atomic<uint64_t> orders_submitted_{0};
    alignas(64) std::atomic<uint64_t> orders_processed_{0};
    alignas(64) std::atomic<uint64_t> executions_emitted_{0};
    alignas(64) std::atomic<uint64_t> dropped_ingress_{0};
    alignas(64) std::atomic<uint64_t> dropped_egress_{0};
};

} // namespace hft
