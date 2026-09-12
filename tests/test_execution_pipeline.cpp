#include "test_framework.hpp"

#include "hft/order_command.hpp"
#include "hft/execution_report.hpp"
#include "hft/risk/risk_engine.hpp"
#include "hft/gateway/order_gateway.hpp"
#include "hft/pipeline/execution_pipeline.hpp"
#include "hft/matching_engine.hpp"

#include <vector>
#include <chrono>
#include <thread>

using namespace hft;

// ============================================================================
// 1. Data Structure Layout & Alignment
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
// 2. Pre-Trade Risk Engine Tests
// ============================================================================

TEST_CASE(PreTradeRisk_OrderValidation) {
    RiskConfig config{};
    config.max_order_quantity = 1000;
    config.max_order_notional = 10000000;
    config.min_price = 100;
    config.max_price = 100000;
    config.allowed_instrument_id = 3045;

    PreTradeRiskEngine risk(config);

    // 1. Valid order
    OrderCommand valid = OrderCommand::make_add(1, 3045, 10, Side::Buy, 5000, 100);
    ASSERT_TRUE(risk.check_order(valid).approved());

    // 2. Invalid OrderId (0)
    OrderCommand zero_id = OrderCommand::make_add(0, 3045, 10, Side::Buy, 5000, 100);
    RiskResult r_zero_id = risk.check_order(zero_id);
    ASSERT_FALSE(r_zero_id.approved());
    ASSERT_EQ(r_zero_id.code, RiskCode::InvalidQuantity);

    // 3. Zero quantity
    OrderCommand zero_qty = OrderCommand::make_add(2, 3045, 10, Side::Buy, 5000, 0);
    RiskResult r_qty = risk.check_order(zero_qty);
    ASSERT_FALSE(r_qty.approved());
    ASSERT_EQ(r_qty.code, RiskCode::InvalidQuantity);

    // 4. Invalid price (<= 0)
    OrderCommand zero_price = OrderCommand::make_add(3, 3045, 10, Side::Buy, 0, 100);
    RiskResult r_price = risk.check_order(zero_price);
    ASSERT_FALSE(r_price.approved());
    ASSERT_EQ(r_price.code, RiskCode::InvalidPrice);

    // 5. Invalid instrument
    OrderCommand wrong_inst = OrderCommand::make_add(4, 9999, 10, Side::Buy, 5000, 100);
    RiskResult r_inst = risk.check_order(wrong_inst);
    ASSERT_FALSE(r_inst.approved());
    ASSERT_EQ(r_inst.code, RiskCode::InvalidInstrument);
}

TEST_CASE(PreTradeRisk_MaxQuantityAndNotional) {
    RiskConfig config{};
    config.max_order_quantity = 500;
    config.max_order_notional = 1000000; // 10,000.00 in paise

    PreTradeRiskEngine risk(config);

    // 1. Quantity exceeds max
    OrderCommand big_qty = OrderCommand::make_add(1, 0, 1, Side::Buy, 1000, 501);
    RiskResult r_qty = risk.check_order(big_qty);
    ASSERT_FALSE(r_qty.approved());
    ASSERT_EQ(r_qty.code, RiskCode::MaxQuantityExceeded);

    // 2. Notional exceeds max (price * qty = 3000 * 400 = 1,200,000 > 1,000,000)
    OrderCommand big_notional = OrderCommand::make_add(2, 0, 1, Side::Buy, 3000, 400);
    RiskResult r_notional = risk.check_order(big_notional);
    ASSERT_FALSE(r_notional.approved());
    ASSERT_EQ(r_notional.code, RiskCode::MaxNotionalExceeded);

    // 3. Exactly at limit (price * qty = 2000 * 500 = 1,000,000)
    OrderCommand boundary = OrderCommand::make_add(3, 0, 1, Side::Buy, 2000, 500);
    ASSERT_TRUE(risk.check_order(boundary).approved());
}

TEST_CASE(PreTradeRisk_PriceBands) {
    RiskConfig config{};
    config.min_price = 1000; // 10.00
    config.max_price = 5000; // 50.00

    PreTradeRiskEngine risk(config);

    // Below minimum
    OrderCommand low_p = OrderCommand::make_add(1, 0, 1, Side::Buy, 999, 10);
    RiskResult r_low = risk.check_order(low_p);
    ASSERT_FALSE(r_low.approved());
    ASSERT_EQ(r_low.code, RiskCode::PriceBandViolation);

    // Above maximum
    OrderCommand high_p = OrderCommand::make_add(2, 0, 1, Side::Buy, 5001, 10);
    RiskResult r_high = risk.check_order(high_p);
    ASSERT_FALSE(r_high.approved());
    ASSERT_EQ(r_high.code, RiskCode::PriceBandViolation);

    // Within band
    OrderCommand ok_p = OrderCommand::make_add(3, 0, 1, Side::Buy, 2500, 10);
    ASSERT_TRUE(risk.check_order(ok_p).approved());
}

TEST_CASE(PreTradeRisk_ExposureTrackingAndRelease) {
    RiskConfig config{};
    config.max_order_quantity = 10000;
    config.max_order_notional = 1000000000;
    config.max_exposure_quantity = 200; // Max 200 open units

    PreTradeRiskEngine risk(config);

    // 1. First order 150 units approved
    OrderCommand cmd1 = OrderCommand::make_add(1, 0, 1, Side::Buy, 1000, 150);
    ASSERT_TRUE(risk.check_order(cmd1).approved());
    risk.on_order_approved(cmd1);
    ASSERT_EQ(risk.current_exposure(), 150u);

    // 2. Second order 60 units breaches 200 limit (150 + 60 = 210 > 200)
    OrderCommand cmd2 = OrderCommand::make_add(2, 0, 1, Side::Buy, 1000, 60);
    RiskResult r_exp = risk.check_order(cmd2);
    ASSERT_FALSE(r_exp.approved());
    ASSERT_EQ(r_exp.code, RiskCode::ExposureLimitExceeded);

    // 3. Partial fill of 50 units on first order releases exposure
    risk.on_order_filled(50);
    ASSERT_EQ(risk.current_exposure(), 100u);

    // 4. Now second order of 60 units fits (100 + 60 = 160 <= 200)
    ASSERT_TRUE(risk.check_order(cmd2).approved());
    risk.on_order_approved(cmd2);
    ASSERT_EQ(risk.current_exposure(), 160u);

    // 5. Cancel order 2 releases 60 units
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
// 3. Order Gateway Tests
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

    // Verify order rests in engine book
    ASSERT_TRUE(engine.book().has_order(101));
    ASSERT_EQ(engine.book().best_bid().value_or(0), 83000);
}

TEST_CASE(OrderGateway_ImmediateFullFill) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    // 1. Place resting Sell order
    OrderCommand resting_sell = OrderCommand::make_add(201, 3045, 1, Side::Sell, 83000, 50);
    gateway.process_command(resting_sell, risk, engine, reports);
    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::New);

    // 2. Place matching Buy order
    reports.clear();
    OrderCommand crossing_buy = OrderCommand::make_add(202, 3045, 2, Side::Buy, 83000, 50);
    gateway.process_command(crossing_buy, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    const auto& rep = reports[0];
    ASSERT_EQ(rep.order_id, 202u);
    ASSERT_EQ(rep.exec_type, ExecutionType::Trade);
    ASSERT_EQ(rep.price, 83000);
    ASSERT_EQ(rep.last_qty, 50u);
    ASSERT_EQ(rep.leaves_qty, 0u); // Fully filled
}

TEST_CASE(OrderGateway_PartialFillAndResidualResting) {
    OrderGateway gateway;
    PreTradeRiskEngine risk;
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;

    // Resting Sell for 40 shares
    OrderCommand sell_cmd = OrderCommand::make_add(301, 3045, 1, Side::Sell, 83000, 40);
    gateway.process_command(sell_cmd, risk, engine, reports);

    // Aggressive Buy for 100 shares
    reports.clear();
    OrderCommand buy_cmd = OrderCommand::make_add(302, 3045, 2, Side::Buy, 83000, 100);
    gateway.process_command(buy_cmd, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    const auto& fill_rep = reports[0];
    ASSERT_EQ(fill_rep.order_id, 302u);
    ASSERT_EQ(fill_rep.exec_type, ExecutionType::Trade);
    ASSERT_EQ(fill_rep.last_qty, 40u);
    ASSERT_EQ(fill_rep.leaves_qty, 60u); // 60 shares remaining

    // Verify residual 60 shares rest in book
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

    // Add order
    OrderCommand add_cmd = OrderCommand::make_add(401, 3045, 1, Side::Buy, 83000, 100);
    gateway.process_command(add_cmd, risk, engine, reports);

    // Cancel order
    reports.clear();
    OrderCommand cancel_cmd = OrderCommand::make_cancel(401, 3045, 1);
    gateway.process_command(cancel_cmd, risk, engine, reports);

    ASSERT_EQ(reports.size(), 1u);
    ASSERT_EQ(reports[0].order_id, 401u);
    ASSERT_EQ(reports[0].exec_type, ExecutionType::Cancelled);
    ASSERT_EQ(reports[0].last_qty, 100u); // Cancelled 100 open units
    ASSERT_EQ(reports[0].leaves_qty, 0u);
    ASSERT_FALSE(engine.book().has_order(401));

    // Cancel non-existent order
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
// 4. Threaded Execution Pipeline Tests
// ============================================================================

TEST_CASE(Pipeline_ThreadedExecution_DeterministicSequence) {
    OrderExecutionPipeline pipeline;
    pipeline.start();

    const size_t order_count = 1000;
    std::vector<ExecutionReport> received_reports;
    received_reports.reserve(order_count);

    // Producer submits 1000 resting orders
    for (size_t i = 1; i <= order_count; ++i) {
        OrderCommand cmd = OrderCommand::make_add(
            i, 3045, 1, Side::Buy, 80000 + static_cast<Price>(i % 50), 10);
        pipeline.submit_order_wait(cmd);
    }

    // Consumer polls reports
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

    // Drain any remaining
    while (pipeline.poll_execution(rep)) {
        received_reports.push_back(rep);
    }

    ASSERT_EQ(received_reports.size(), order_count);
    ASSERT_EQ(pipeline.orders_submitted(), order_count);
    ASSERT_EQ(pipeline.orders_processed(), order_count);
    ASSERT_EQ(pipeline.executions_emitted(), order_count);
    ASSERT_EQ(pipeline.dropped_ingress(), 0u);
    ASSERT_EQ(pipeline.dropped_egress(), 0u);

    // Verify FIFO ordering
    for (size_t i = 0; i < order_count; ++i) {
        ASSERT_EQ(received_reports[i].order_id, i + 1);
        ASSERT_EQ(received_reports[i].exec_type, ExecutionType::New);
    }
}

TEST_CASE(Pipeline_Backpressure_DropTracking) {
    OrderExecutionPipeline pipeline;
    // Worker thread is deliberately NOT started

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
// 5. Differential Testing: Direct Engine vs. Risk + Gateway Pipeline
// ============================================================================

TEST_CASE(Differential_DirectEngineVsGatewayPipeline) {
    MatchingEngine direct_engine;
    PreTradeRiskEngine risk;
    OrderGateway gateway;
    MatchingEngine gateway_engine;

    std::vector<Trade> direct_trades;
    std::vector<ExecutionReport> gateway_reports;

    // Run deterministic stream of 2,000 orders
    const size_t test_orders = 2000;
    for (size_t i = 1; i <= test_orders; ++i) {
        const Side side = (i % 2 == 0) ? Side::Buy : Side::Sell;
        const Price price = (side == Side::Buy) ? (83000 + static_cast<Price>(i % 30))
                                                : (83020 + static_cast<Price>(i % 30));
        const Quantity qty = 10 + (i % 15);

        // Path A: Direct Matching Engine
        direct_engine.submit_limit_order(i, side, price, qty, direct_trades);

        // Path B: Order Command -> Risk -> Gateway -> Matching Engine
        OrderCommand cmd = OrderCommand::make_add(i, 3045, 1, side, price, qty);
        gateway.process_command(cmd, risk, gateway_engine, gateway_reports);
    }

    // Verify 100% bit-exact equivalence between direct engine and gateway engine
    ASSERT_EQ(direct_engine.total_trades_generated(), gateway_engine.total_trades_generated());
    ASSERT_EQ(direct_engine.book().best_bid().value_or(0), gateway_engine.book().best_bid().value_or(0));
    ASSERT_EQ(direct_engine.book().best_ask().value_or(0), gateway_engine.book().best_ask().value_or(0));
    ASSERT_EQ(direct_engine.book().total_orders(), gateway_engine.book().total_orders());
}
