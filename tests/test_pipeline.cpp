#include "test_framework.hpp"
#include "hft/order.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/matching_engine.hpp"
#include "hft/trading_pipeline.hpp"

#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <string>

using namespace hft;

// ============================================================================
// 1. SPSC Queue Single-Threaded Correctness Tests
// ============================================================================

TEST_CASE(SpscQueue_EmptyPop) {
    SpscQueue<OrderCommand, 16> queue;
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(queue.size(), 0ULL);

    OrderCommand cmd{};
    ASSERT_FALSE(queue.try_pop(cmd));
    ASSERT_TRUE(queue.empty());
}

TEST_CASE(SpscQueue_SinglePushPop) {
    SpscQueue<OrderCommand, 16> queue;
    OrderCommand in = OrderCommand::make_add(101, 3045, 1, Side::Buy, 10000, 50);

    ASSERT_TRUE(queue.try_push(in));
    ASSERT_FALSE(queue.empty());
    ASSERT_EQ(queue.size(), 1ULL);

    OrderCommand out{};
    ASSERT_TRUE(queue.try_pop(out));
    ASSERT_TRUE(queue.empty());
    ASSERT_EQ(queue.size(), 0ULL);

    ASSERT_EQ(out.order_id, 101ULL);
    ASSERT_EQ(out.price, 10000);
    ASSERT_EQ(out.qty, 50ULL);
}

TEST_CASE(SpscQueue_FIFOOrdering) {
    SpscQueue<OrderCommand, 16> queue;
    for (uint64_t i = 1; i <= 5; ++i) {
        ASSERT_TRUE(queue.try_push(OrderCommand::make_add(i, 3045, 1, Side::Buy, 10000 + static_cast<Price>(i), 10)));
    }
    ASSERT_EQ(queue.size(), 5ULL);

    for (uint64_t i = 1; i <= 5; ++i) {
        OrderCommand cmd{};
        ASSERT_TRUE(queue.try_pop(cmd));
        ASSERT_EQ(cmd.order_id, i);
        ASSERT_EQ(cmd.price, 10000 + static_cast<Price>(i));
    }
    ASSERT_TRUE(queue.empty());
}

TEST_CASE(SpscQueue_FillToCapacityAndRejection) {
    constexpr size_t Cap = 8;
    SpscQueue<OrderCommand, Cap> queue;

    for (size_t i = 0; i < Cap; ++i) {
        ASSERT_TRUE(queue.try_push(OrderCommand::make_add(i + 1, 3045, 1, Side::Buy, 100, 10)));
    }
    ASSERT_EQ(queue.size(), Cap);

    OrderCommand overflow = OrderCommand::make_add(999, 3045, 1, Side::Sell, 200, 20);
    ASSERT_FALSE(queue.try_push(overflow));
    ASSERT_EQ(queue.size(), Cap);

    OrderCommand popped{};
    ASSERT_TRUE(queue.try_pop(popped));
    ASSERT_EQ(popped.order_id, 1ULL);
    ASSERT_EQ(queue.size(), Cap - 1);

    ASSERT_TRUE(queue.try_push(overflow));
    ASSERT_EQ(queue.size(), Cap);
}

TEST_CASE(SpscQueue_Wraparound) {
    constexpr size_t Cap = 4;
    SpscQueue<OrderCommand, Cap> queue;

    for (size_t cycle = 0; cycle < 100; ++cycle) {
        for (size_t i = 0; i < Cap; ++i) {
            uint64_t id = cycle * 10 + i;
            ASSERT_TRUE(queue.try_push(OrderCommand::make_add(id, 3045, 1, Side::Buy, 100, 10)));
        }
        for (size_t i = 0; i < Cap; ++i) {
            OrderCommand cmd{};
            ASSERT_TRUE(queue.try_pop(cmd));
            ASSERT_EQ(cmd.order_id, cycle * 10 + i);
        }
        ASSERT_TRUE(queue.empty());
    }
}

TEST_CASE(SpscQueue_AlternatingPushPop) {
    constexpr size_t Cap = 8;
    SpscQueue<OrderCommand, Cap> queue;

    for (uint64_t i = 1; i <= 10000; ++i) {
        ASSERT_TRUE(queue.try_push(OrderCommand::make_add(i, 3045, 1, Side::Buy, 100, 10)));
        OrderCommand cmd{};
        ASSERT_TRUE(queue.try_pop(cmd));
        ASSERT_EQ(cmd.order_id, i);
        ASSERT_TRUE(queue.empty());
    }
}

TEST_CASE(SpscQueue_LargeDeterministicSequence) {
    constexpr size_t Cap = 1024;
    SpscQueue<OrderCommand, Cap> queue;
    constexpr size_t Total = 100000;

    size_t pushed = 0;
    size_t popped = 0;

    while (popped < Total) {
        for (size_t k = 0; k < 200 && pushed < Total; ++k) {
            if (!queue.try_push(OrderCommand::make_add(pushed + 1, 3045, 1, Side::Buy, 1000, 10))) {
                break;
            }
            ++pushed;
        }

        for (size_t k = 0; k < 150 && popped < pushed; ++k) {
            OrderCommand cmd{};
            if (!queue.try_pop(cmd)) {
                break;
            }
            ASSERT_EQ(cmd.order_id, popped + 1);
            ++popped;
        }
    }

    ASSERT_EQ(pushed, Total);
    ASSERT_EQ(popped, Total);
    ASSERT_TRUE(queue.empty());
}

// ============================================================================
// 2. SPSC Queue Concurrent 1P / 1C Stress Test (2M Items)
// ============================================================================

TEST_CASE(SpscQueue_Concurrent1P1C_StressTest) {
    constexpr size_t QueueCapacity = 1024;
    constexpr uint64_t EventCount = 2000000;

    auto queue = std::make_unique<SpscQueue<OrderCommand, QueueCapacity, true>>();
    std::atomic<bool> producer_done{false};
    std::atomic<bool> test_failed{false};
    std::string failure_reason;

    std::thread producer([&]() {
        for (uint64_t i = 1; i <= EventCount; ++i) {
            OrderCommand cmd = OrderCommand::make_add(
                i, 3045, 1, (i % 2 == 0) ? Side::Buy : Side::Sell, 10000 + static_cast<Price>(i % 50), 10);
            while (!queue->try_push(cmd)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    uint64_t expected_id = 1;
    uint64_t total_received = 0;

    while (total_received < EventCount) {
        OrderCommand cmd{};
        if (queue->try_pop(cmd)) {
            if (cmd.order_id != expected_id) {
                test_failed.store(true, std::memory_order_relaxed);
                failure_reason = "Out-of-order or corrupted event. Expected " +
                                 std::to_string(expected_id) + " got " + std::to_string(cmd.order_id);
                break;
            }
            ++expected_id;
            ++total_received;
        } else if (producer_done.load(std::memory_order_acquire) && queue->empty()) {
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

// ============================================================================
// 3. Execution Data Structure Layout
// ============================================================================

TEST_CASE(OrderCommand_LayoutAndAlignment) {
    ASSERT_EQ(sizeof(OrderCommand), 64u);
    ASSERT_EQ(alignof(OrderCommand), 64u);
    ASSERT_TRUE(std::is_trivially_copyable_v<OrderCommand>);
    ASSERT_TRUE(std::is_standard_layout_v<OrderCommand>);
}

TEST_CASE(ExecutionReport_LayoutAndAlignment) {
    ASSERT_EQ(sizeof(ExecutionReport), 64u);
    ASSERT_EQ(alignof(ExecutionReport), 64u);
    ASSERT_TRUE(std::is_trivially_copyable_v<ExecutionReport>);
    ASSERT_TRUE(std::is_standard_layout_v<ExecutionReport>);
}

// ============================================================================
// 4. Pre-Trade Risk Engine Tests
// ============================================================================

TEST_CASE(PreTradeRisk_OrderValidation) {
    RiskConfig config{};
    config.max_order_quantity = 1000;
    config.max_order_notional = 10000000;
    config.min_price = 100;
    config.max_price = 100000;
    config.allowed_instrument_id = 3045;

    PreTradeRiskEngine risk(config);

    OrderCommand valid = OrderCommand::make_add(1, 3045, 10, Side::Buy, 5000, 100);
    ASSERT_TRUE(risk.check_order(valid).approved());

    OrderCommand zero_id = OrderCommand::make_add(0, 3045, 10, Side::Buy, 5000, 100);
    RiskResult r_zero_id = risk.check_order(zero_id);
    ASSERT_FALSE(r_zero_id.approved());
    ASSERT_EQ(r_zero_id.code, RiskCode::InvalidQuantity);

    OrderCommand zero_qty = OrderCommand::make_add(2, 3045, 10, Side::Buy, 5000, 0);
    RiskResult r_qty = risk.check_order(zero_qty);
    ASSERT_FALSE(r_qty.approved());
    ASSERT_EQ(r_qty.code, RiskCode::InvalidQuantity);

    OrderCommand zero_price = OrderCommand::make_add(3, 3045, 10, Side::Buy, 0, 100);
    RiskResult r_price = risk.check_order(zero_price);
    ASSERT_FALSE(r_price.approved());
    ASSERT_EQ(r_price.code, RiskCode::InvalidPrice);

    OrderCommand wrong_inst = OrderCommand::make_add(4, 9999, 10, Side::Buy, 5000, 100);
    RiskResult r_inst = risk.check_order(wrong_inst);
    ASSERT_FALSE(r_inst.approved());
    ASSERT_EQ(r_inst.code, RiskCode::InvalidInstrument);
}

TEST_CASE(PreTradeRisk_MaxQuantityAndNotional) {
    RiskConfig config{};
    config.max_order_quantity = 500;
    config.max_order_notional = 1000000;

    PreTradeRiskEngine risk(config);

    OrderCommand big_qty = OrderCommand::make_add(1, 0, 1, Side::Buy, 1000, 501);
    RiskResult r_qty = risk.check_order(big_qty);
    ASSERT_FALSE(r_qty.approved());
    ASSERT_EQ(r_qty.code, RiskCode::MaxQuantityExceeded);

    OrderCommand big_notional = OrderCommand::make_add(2, 0, 1, Side::Buy, 3000, 400);
    RiskResult r_notional = risk.check_order(big_notional);
    ASSERT_FALSE(r_notional.approved());
    ASSERT_EQ(r_notional.code, RiskCode::MaxNotionalExceeded);

    OrderCommand boundary = OrderCommand::make_add(3, 0, 1, Side::Buy, 2000, 500);
    ASSERT_TRUE(risk.check_order(boundary).approved());
}

TEST_CASE(PreTradeRisk_PriceBands) {
    RiskConfig config{};
    config.min_price = 1000;
    config.max_price = 5000;

    PreTradeRiskEngine risk(config);

    OrderCommand low_p = OrderCommand::make_add(1, 0, 1, Side::Buy, 999, 10);
    RiskResult r_low = risk.check_order(low_p);
    ASSERT_FALSE(r_low.approved());
    ASSERT_EQ(r_low.code, RiskCode::PriceBandViolation);

    OrderCommand high_p = OrderCommand::make_add(2, 0, 1, Side::Buy, 5001, 10);
    RiskResult r_high = risk.check_order(high_p);
    ASSERT_FALSE(r_high.approved());
    ASSERT_EQ(r_high.code, RiskCode::PriceBandViolation);

    OrderCommand ok_p = OrderCommand::make_add(3, 0, 1, Side::Buy, 2500, 10);
    ASSERT_TRUE(risk.check_order(ok_p).approved());
}

TEST_CASE(PreTradeRisk_ExposureTrackingAndRelease) {
    RiskConfig config{};
    config.max_order_quantity = 10000;
    config.max_order_notional = 1000000000;
    config.max_exposure_quantity = 200;

    PreTradeRiskEngine risk(config);

    OrderCommand cmd1 = OrderCommand::make_add(1, 0, 1, Side::Buy, 1000, 150);
    ASSERT_TRUE(risk.check_order(cmd1).approved());
    risk.on_order_approved(cmd1);
    ASSERT_EQ(risk.current_exposure(), 150u);

    OrderCommand cmd2 = OrderCommand::make_add(2, 0, 1, Side::Buy, 1000, 60);
    RiskResult r_exp = risk.check_order(cmd2);
    ASSERT_FALSE(r_exp.approved());
    ASSERT_EQ(r_exp.code, RiskCode::ExposureLimitExceeded);

    risk.on_order_filled(50);
    ASSERT_EQ(risk.current_exposure(), 100u);

    ASSERT_TRUE(risk.check_order(cmd2).approved());
    risk.on_order_approved(cmd2);
    ASSERT_EQ(risk.current_exposure(), 160u);

    risk.on_order_cancelled(60);
    ASSERT_EQ(risk.current_exposure(), 100u);
}

TEST_CASE(PreTradeRisk_CountersAudit) {
    RiskConfig config{};
    config.max_order_quantity = 100;
    PreTradeRiskEngine risk(config);

    OrderCommand ok1 = OrderCommand::make_add(1, 0, 1, Side::Buy, 1000, 50);
    OrderCommand ok2 = OrderCommand::make_add(2, 0, 1, Side::Buy, 1000, 50);
    OrderCommand bad_qty = OrderCommand::make_add(3, 0, 1, Side::Buy, 1000, 150);

    risk.check_order(ok1);
    risk.check_order(ok2);
    risk.check_order(bad_qty);

    ASSERT_EQ(risk.stats().orders_checked, 3u);
    ASSERT_EQ(risk.stats().orders_approved, 2u);
    ASSERT_EQ(risk.stats().orders_rejected, 1u);
    ASSERT_EQ(risk.stats().reject_max_quantity, 1u);
}

// ============================================================================
// 5. Order Gateway Lifecycle Tests
// ============================================================================

TEST_CASE(OrderGateway_RestingLimitOrder) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    OrderCommand cmd = OrderCommand::make_add(101, 3045, 1, Side::Buy, 83000, 100, 12345);
    gateway.process_command(cmd, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    const auto& rep = reports[0];
    ASSERT_EQ(rep.order_id, 101u);
    ASSERT_EQ(rep.exec_type, ExecutionType::New);
    ASSERT_EQ(rep.side, Side::Buy);
    ASSERT_EQ(rep.price, 83000);
    ASSERT_EQ(rep.last_qty, 0u);
    ASSERT_EQ(rep.leaves_qty, 100u);
    ASSERT_EQ(rep.risk_code, RiskCode::Approved);
    ASSERT_EQ(rep.engine_result, OrderResult::Accepted);

    ASSERT_TRUE(engine.book().has_order(101));
    ASSERT_EQ(engine.book().best_bid().value_or(0), 83000);
}

TEST_CASE(OrderGateway_ImmediateFullFill) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    OrderCommand resting_sell = OrderCommand::make_add(201, 3045, 1, Side::Sell, 83000, 50);
    gateway.process_command(resting_sell, risk, engine, reports);
    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::New);

    reports.clear();
    OrderCommand crossing_buy = OrderCommand::make_add(202, 3045, 2, Side::Buy, 83000, 50);
    gateway.process_command(crossing_buy, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    const auto& rep = reports[0];
    ASSERT_EQ(rep.order_id, 202u);
    ASSERT_EQ(rep.exec_type, ExecutionType::Trade);
    ASSERT_EQ(rep.price, 83000);
    ASSERT_EQ(rep.last_qty, 50u);
    ASSERT_EQ(rep.leaves_qty, 0u);
}

TEST_CASE(OrderGateway_PartialFillAndResidualResting) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    OrderCommand sell_cmd = OrderCommand::make_add(301, 3045, 1, Side::Sell, 83000, 40);
    gateway.process_command(sell_cmd, risk, engine, reports);

    reports.clear();
    OrderCommand buy_cmd = OrderCommand::make_add(302, 3045, 2, Side::Buy, 83000, 100);
    gateway.process_command(buy_cmd, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    const auto& fill_rep = reports[0];
    ASSERT_EQ(fill_rep.order_id, 302u);
    ASSERT_EQ(fill_rep.exec_type, ExecutionType::Trade);
    ASSERT_EQ(fill_rep.last_qty, 40u);
    ASSERT_EQ(fill_rep.leaves_qty, 60u);

    ASSERT_TRUE(engine.book().has_order(302));
    const auto resting = engine.book().get_order(302);
    ASSERT_TRUE(resting.has_value());
    ASSERT_EQ(resting->remaining_qty, 60u);
}

TEST_CASE(OrderGateway_CancellationLifecycle) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    OrderCommand add_cmd = OrderCommand::make_add(401, 3045, 1, Side::Buy, 83000, 100);
    gateway.process_command(add_cmd, risk, engine, reports);

    reports.clear();
    OrderCommand cancel_cmd = OrderCommand::make_cancel(401, 3045, 1);
    gateway.process_command(cancel_cmd, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].order_id, 401u);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::Cancelled);
    ASSERT_EQ(reports[0].last_qty, 100u);
    ASSERT_EQ(reports[0].leaves_qty, 0u);
    ASSERT_FALSE(engine.book().has_order(401));

    reports.clear();
    OrderCommand bad_cancel = OrderCommand::make_cancel(9999, 3045, 1);
    gateway.process_command(bad_cancel, risk, engine, reports);
    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::EngineRejected);
    ASSERT_EQ(reports[0].engine_result, OrderResult::RejectedOrderNotFound);
}

TEST_CASE(OrderGateway_DuplicateIdRejection) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    OrderCommand cmd1 = OrderCommand::make_add(501, 3045, 1, Side::Buy, 83000, 100);
    gateway.process_command(cmd1, risk, engine, reports);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::New);

    reports.clear();
    OrderCommand duplicate = OrderCommand::make_add(501, 3045, 1, Side::Buy, 83000, 100);
    gateway.process_command(duplicate, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::EngineRejected);
    ASSERT_EQ(reports[0].engine_result, OrderResult::RejectedDuplicateId);
}

// ============================================================================
// 6. Threaded Execution Pipeline Tests
// ============================================================================

TEST_CASE(Pipeline_ThreadedExecution_DeterministicSequence) {
    OrderExecutionPipeline pipeline;
    pipeline.start();

    const size_t order_count = 1000;
    std::vector<ExecutionReport> received_reports;
    received_reports.reserve(order_count);

    for (size_t i = 1; i <= order_count; ++i) {
        OrderCommand cmd = OrderCommand::make_add(
            i, 3045, 1, Side::Buy, 80000 + static_cast<Price>(i % 50), 10);
        pipeline.submit_order_wait(cmd);
    }

    ExecutionReport rep{};
    const auto start_wait = std::chrono::steady_clock::now();
    while (received_reports.size() < order_count) {
        if (pipeline.poll_execution(rep)) {
            received_reports.push_back(rep);
        } else {
            std::this_thread::yield();
        }
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_wait).count() > 5) {
            break;
        }
    }

    pipeline.stop_and_join();

    while (pipeline.poll_execution(rep)) {
        received_reports.push_back(rep);
    }

    ASSERT_EQ(received_reports.size(), order_count);
    ASSERT_EQ(pipeline.orders_submitted(), order_count);
    ASSERT_EQ(pipeline.orders_processed(), order_count);
    ASSERT_EQ(pipeline.executions_emitted(), order_count);
    ASSERT_EQ(pipeline.dropped_ingress(), 0u);
    ASSERT_EQ(pipeline.dropped_egress(), 0u);

    for (size_t i = 0; i < order_count; ++i) {
        ASSERT_EQ(received_reports[i].order_id, i + 1);
        ASSERT_EQ(received_reports[i].exec_type, ExecutionType::New);
    }
}

TEST_CASE(Pipeline_Backpressure_DropTracking) {
    OrderExecutionPipeline pipeline;

    const size_t capacity = OrderExecutionPipeline::DEFAULT_QUEUE_CAPACITY;
    size_t accepted = 0;
    size_t dropped = 0;

    for (size_t i = 1; i <= capacity + 500; ++i) {
        OrderCommand cmd = OrderCommand::make_add(i, 3045, 1, Side::Buy, 80000, 10);
        if (pipeline.submit_order(cmd)) {
            ++accepted;
        } else {
            ++dropped;
        }
    }

    ASSERT_EQ(accepted, capacity);
    ASSERT_EQ(dropped, 500u);
    ASSERT_EQ(pipeline.orders_submitted(), capacity);
    ASSERT_EQ(pipeline.dropped_ingress(), 500u);
}

// ============================================================================
// 7. Differential & Equivalence Pipeline Testing
// ============================================================================

TEST_CASE(Differential_DirectEngineVsGatewayPipeline) {
    MatchingEngine direct_engine;
    PreTradeRiskEngine risk;
    OrderGateway gateway;
    MatchingEngine gateway_engine;

    std::vector<Trade> direct_trades;
    std::vector<ExecutionReport> gateway_reports;

    const size_t test_orders = 2000;
    for (size_t i = 1; i <= test_orders; ++i) {
        const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        const Price price = (side == Side::Buy) ? (83000 + static_cast<Price>(i % 30))
                                                : (83020 + static_cast<Price>(i % 30));
        const Quantity qty = 10 + (i % 15);

        direct_engine.submit_limit_order(i, side, price, qty, direct_trades);

        OrderCommand cmd = OrderCommand::make_add(i, 3045, 1, side, price, qty);
        gateway.process_command(cmd, risk, gateway_engine, gateway_reports);
    }

    ASSERT_EQ(direct_engine.total_trades_generated(), gateway_engine.total_trades_generated());
    ASSERT_EQ(direct_engine.book().best_bid().value_or(0), gateway_engine.book().best_bid().value_or(0));
    ASSERT_EQ(direct_engine.book().best_ask().value_or(0), gateway_engine.book().best_ask().value_or(0));
    ASSERT_EQ(direct_engine.book().total_orders(), gateway_engine.book().total_orders());
}

static void run_pipeline_equivalence_test(uint64_t seed, int operations_count) {
    uint64_t lcg_state = seed;
    auto lcg_next = [&lcg_state](uint64_t min, uint64_t max) -> uint64_t {
        lcg_state = lcg_state * 6364136223846793005ULL + 1ULL;
        uint64_t x = lcg_state >> 32;
        return min + (x % (max - min + 1));
    };

    OrderId next_id = 1;
    std::vector<OrderId> active_ids;
    std::vector<OrderCommand> commands;
    commands.reserve(static_cast<size_t>(operations_count));

    for (int step = 0; step < operations_count; ++step) {
        const uint64_t op = lcg_next(1, 100);

        if (op <= 60 || active_ids.empty()) {
            OrderId id = next_id++;
            Side side = (lcg_next(0, 1) == 0) ? Side::Buy : Side::Sell;
            Price price = static_cast<Price>(lcg_next(9950, 10050));
            Quantity qty = static_cast<Quantity>(lcg_next(5, 100));
            commands.push_back(OrderCommand::make_add(id, 3045, 1, side, price, qty));
            active_ids.push_back(id);
        } else if (op <= 80) {
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();
            commands.push_back(OrderCommand::make_cancel(id, 3045, 1));
        } else {
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            Price new_price = static_cast<Price>(lcg_next(9960, 10040));
            Quantity new_qty = static_cast<Quantity>(lcg_next(10, 80));
            Side side = (lcg_next(0, 1) == 0) ? Side::Buy : Side::Sell;
            commands.push_back(OrderCommand::make_modify(id, 3045, 1, side, new_price, new_qty));
        }
    }

    // Direct synchronous Gateway execution
    MatchingEngine direct_engine;
    PreTradeRiskEngine direct_risk;
    OrderGateway direct_gateway;
    std::vector<ExecutionReport> direct_reports;
    direct_reports.reserve(commands.size() * 2);

    for (const auto& cmd : commands) {
        direct_gateway.process_command(cmd, direct_risk, direct_engine, direct_reports);
    }

    // Threaded SPSC Execution Pipeline
    OrderExecutionPipeline pipeline;
    pipeline.start();

    for (const auto& cmd : commands) {
        pipeline.submit_order_wait(cmd);
    }

    pipeline.stop_and_join();

    std::vector<ExecutionReport> pipe_reports;
    pipe_reports.reserve(commands.size() * 2);
    ExecutionReport rep{};
    while (pipeline.poll_execution(rep)) {
        pipe_reports.push_back(rep);
    }

    ASSERT_EQ(direct_reports.size(), pipe_reports.size());
    for (size_t i = 0; i < direct_reports.size(); ++i) {
        ASSERT_EQ(direct_reports[i].order_id, pipe_reports[i].order_id);
        ASSERT_EQ(direct_reports[i].exec_type, pipe_reports[i].exec_type);
        ASSERT_EQ(direct_reports[i].side, pipe_reports[i].side);
        ASSERT_EQ(direct_reports[i].price, pipe_reports[i].price);
        ASSERT_EQ(direct_reports[i].last_qty, pipe_reports[i].last_qty);
        ASSERT_EQ(direct_reports[i].leaves_qty, pipe_reports[i].leaves_qty);
    }

    const auto& db = direct_engine.book();
    const auto& pb = pipeline.engine().book();
    ASSERT_EQ(db.best_bid(), pb.best_bid());
    ASSERT_EQ(db.best_ask(), pb.best_ask());
    ASSERT_EQ(db.best_bid_qty(), pb.best_bid_qty());
    ASSERT_EQ(db.best_ask_qty(), pb.best_ask_qty());
    ASSERT_EQ(db.total_orders(), pb.total_orders());

    std::string d_err, p_err;
    bool d_ok = direct_engine.verify_invariants(&d_err);
    if (!d_ok) std::cerr << "DIRECT ENGINE INVARIANT ERROR: " << d_err << "\n";
    ASSERT_TRUE(d_ok);
    bool p_ok = pipeline.engine().verify_invariants(&p_err);
    if (!p_ok) std::cerr << "PIPELINE ENGINE INVARIANT ERROR: " << p_err << "\n";
    ASSERT_TRUE(p_ok);
}

TEST_CASE(Pipeline_DeterministicEquivalence_Seed1) {
    run_pipeline_equivalence_test(12345ULL, 2000);
}

TEST_CASE(Pipeline_DeterministicEquivalence_Seed2) {
    run_pipeline_equivalence_test(99999ULL, 2000);
}

TEST_CASE(Pipeline_DeterministicEquivalence_Seed3) {
    run_pipeline_equivalence_test(77777ULL, 2000);
}
