#pragma once

#include "hft/order_command.hpp"
#include "hft/execution_report.hpp"
#include "hft/risk/risk_engine.hpp"
#include "hft/matching_engine.hpp"

#include <vector>
#include <cstdint>
#include <type_traits>

namespace hft {

/**
 * @brief Low-latency Order Gateway translating order commands into matching engine operations.
 *
 * Coordinates pre-trade risk evaluation, matching engine submission, and execution report generation.
 * Guaranteed 0 dynamic heap allocations on the hot path via pre-reserved trade buffers.
 */
class OrderGateway {
public:
    static constexpr size_t DEFAULT_TRADE_BUFFER_CAPACITY = 256;

    explicit OrderGateway(size_t trade_buffer_capacity = DEFAULT_TRADE_BUFFER_CAPACITY);
    ~OrderGateway() = default;

    // Movable, non-copyable
    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;
    OrderGateway(OrderGateway&&) noexcept = default;
    OrderGateway& operator=(OrderGateway&&) noexcept = default;

    /**
     * @brief Processes an incoming OrderCommand through risk evaluation and matching engine.
     *
     * @tparam Emitter Inlined callback receiving generated ExecutionReport instances.
     * @param cmd Ingress order request
     * @param risk Pre-trade risk validator
     * @param engine Target matching engine
     * @param emitter Inlined callable invoked for each generated ExecutionReport
     */
    template <typename Emitter>
    void process_command(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                         MatchingEngine& engine, Emitter&& emitter) {
        // 1. Pre-Trade Risk Check
        const RiskResult risk_res = risk.check_order(cmd);
        if (!risk_res.approved()) {
            ++exec_sequence_;
            emitter(ExecutionReport::make_risk_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, risk_res.code,
                exec_sequence_, cmd.timestamp));
            return;
        }

        // 2. Dispatch to Matching Engine
        if (cmd.type == EventType::Add) {
            process_add(cmd, risk, engine, emitter);
        } else if (cmd.type == EventType::Cancel) {
            process_cancel(cmd, risk, engine, emitter);
        } else if (cmd.type == EventType::Modify) {
            process_modify(cmd, risk, engine, emitter);
        }
    }

    /**
     * @brief Convenience overload collecting reports into a preallocated vector.
     */
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
                // Completely resting in the order book
                risk.on_order_approved(cmd);
                ++exec_sequence_;
                emitter(ExecutionReport::make_new(
                    cmd.order_id, cmd.instrument_id, cmd.client_id,
                    cmd.side, cmd.price, cmd.qty, exec_sequence_, cmd.timestamp));
            } else {
                // Trades occurred
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
                    // Residual resting quantity
                    OrderCommand resting_cmd = cmd;
                    resting_cmd.qty = remaining_leaves;
                    risk.on_order_approved(resting_cmd);
                }
            }
        } else {
            // Engine rejected (e.g. duplicate ID or invalid parameter)
            ++exec_sequence_;
            emitter(ExecutionReport::make_engine_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, res, exec_sequence_, cmd.timestamp));
        }
    }

    template <typename Emitter>
    void process_cancel(const OrderCommand& cmd, PreTradeRiskEngine& risk,
                        MatchingEngine& engine, Emitter&& emitter) {
        const auto existing = engine.book().get_order(cmd.order_id);
        const Quantity open_qty = existing ? existing->remaining_qty : 0;
        const Side order_side = existing ? existing->side : cmd.side;

        const OrderResult res = engine.cancel_order(cmd.order_id);
        ++exec_sequence_;

        if (res == OrderResult::Accepted) {
            risk.on_order_cancelled(open_qty);
            emitter(ExecutionReport::make_cancelled(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                order_side, open_qty, exec_sequence_, cmd.timestamp));
        } else {
            emitter(ExecutionReport::make_engine_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, res, exec_sequence_, cmd.timestamp));
        }
    }

    template <typename Emitter>
    void process_modify(const OrderCommand& cmd, [[maybe_unused]] PreTradeRiskEngine& risk,
                        MatchingEngine& engine, Emitter&& emitter) {
        trades_buf_.clear();
        const OrderResult res = engine.modify_order(cmd.order_id, cmd.price, cmd.qty, trades_buf_);
        ++exec_sequence_;

        if (res == OrderResult::Accepted) {
            // Check if any crossing trades occurred on modify
            if (!trades_buf_.empty()) {
                for (const auto& trade : trades_buf_) {
                    ++exec_sequence_;
                    emitter(ExecutionReport::make_trade(
                        cmd.order_id, cmd.instrument_id, cmd.client_id,
                        cmd.side, trade.price, trade.quantity, 0,
                        exec_sequence_, cmd.timestamp));
                }
            }
            emitter(ExecutionReport::make_modified(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, exec_sequence_, cmd.timestamp));
        } else {
            emitter(ExecutionReport::make_engine_rejected(
                cmd.order_id, cmd.instrument_id, cmd.client_id,
                cmd.side, cmd.price, cmd.qty, res, exec_sequence_, cmd.timestamp));
        }
    }

    uint64_t exec_sequence_{0};
    std::vector<Trade> trades_buf_;
};

} // namespace hft
