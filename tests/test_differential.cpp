#include "test_framework.hpp"
#include "hft/matching_engine.hpp"
#include "hft/baseline/baseline_matching_engine.hpp"

using namespace hft;

static void run_differential_test(uint64_t seed, int operations_count) {
    baseline::MatchingEngine baseline_engine;
    MatchingEngine optimized_engine;

    optimized_engine.reserve(static_cast<size_t>(operations_count));

    uint64_t lcg_state = seed;
    auto lcg_next = [&lcg_state](uint64_t min, uint64_t max) -> uint64_t {
        lcg_state = lcg_state * 6364136223846793005ULL + 1ULL;
        uint64_t x = lcg_state >> 32;
        return min + (x % (max - min + 1));
    };

    OrderId next_id = 1;
    std::vector<OrderId> active_ids;

    std::vector<Trade> baseline_step_trades;
    std::vector<Trade> optimized_step_trades;

    std::vector<Trade> baseline_all_trades;
    std::vector<Trade> optimized_all_trades;

    for (int step = 0; step < operations_count; ++step) {
        baseline_step_trades.clear();
        optimized_step_trades.clear();

        const uint64_t op = lcg_next(1, 100);

        if (op <= 60 || active_ids.empty()) {
            // 60% Add Limit Order
            OrderId id = next_id++;
            Side side = (lcg_next(0, 1) == 0) ? Side::Buy : Side::Sell;
            Price price = static_cast<Price>(lcg_next(9950, 10050));
            Quantity qty = static_cast<Quantity>(lcg_next(5, 100));

            auto b_res = baseline_engine.submit_limit_order(id, side, price, qty, baseline_step_trades);
            auto o_res = optimized_engine.submit_limit_order(id, side, price, qty, optimized_step_trades);

            ASSERT_EQ(b_res, o_res);

            if (b_res == OrderResult::Accepted) {
                if (baseline_engine.book().has_order(id)) {
                    active_ids.push_back(id);
                }
            }
        } else if (op <= 80) {
            // 20% Cancel random order
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();

            auto b_res = baseline_engine.cancel_order(id);
            auto o_res = optimized_engine.cancel_order(id);
            ASSERT_EQ(b_res, o_res);
        } else {
            // 20% Modify random order
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            Price new_price = static_cast<Price>(lcg_next(9960, 10040));
            Quantity new_qty = static_cast<Quantity>(lcg_next(10, 80));

            auto b_res = baseline_engine.modify_order(id, new_price, new_qty, baseline_step_trades);
            auto o_res = optimized_engine.modify_order(id, new_price, new_qty, optimized_step_trades);
            ASSERT_EQ(b_res, o_res);

            if (!baseline_engine.book().has_order(id)) {
                active_ids[idx] = active_ids.back();
                active_ids.pop_back();
            }
        }

        // Verify trades generated in this step match exactly
        ASSERT_EQ(baseline_step_trades.size(), optimized_step_trades.size());
        for (size_t t = 0; t < baseline_step_trades.size(); ++t) {
            ASSERT_TRUE(baseline_step_trades[t] == optimized_step_trades[t]);
            baseline_all_trades.push_back(baseline_step_trades[t]);
            optimized_all_trades.push_back(optimized_step_trades[t]);
        }
    }

    // Comprehensive Final State Equivalence Verification
    ASSERT_EQ(baseline_all_trades.size(), optimized_all_trades.size());
    ASSERT_EQ(baseline_engine.total_trades_generated(), optimized_engine.total_trades_generated());

    const auto& bb = baseline_engine.book();
    const auto& ob = optimized_engine.book();

    ASSERT_EQ(bb.best_bid().has_value(), ob.best_bid().has_value());
    if (bb.best_bid().has_value()) {
        ASSERT_EQ(*bb.best_bid(), *ob.best_bid());
    }

    ASSERT_EQ(bb.best_ask().has_value(), ob.best_ask().has_value());
    if (bb.best_ask().has_value()) {
        ASSERT_EQ(*bb.best_ask(), *ob.best_ask());
    }

    ASSERT_EQ(bb.best_bid_qty(), ob.best_bid_qty());
    ASSERT_EQ(bb.best_ask_qty(), ob.best_ask_qty());
    ASSERT_EQ(bb.bid_depth(), ob.bid_depth());
    ASSERT_EQ(bb.ask_depth(), ob.ask_depth());
    ASSERT_EQ(bb.total_orders(), ob.total_orders());
    ASSERT_EQ(bb.total_bid_qty(), ob.total_bid_qty());
    ASSERT_EQ(bb.total_ask_qty(), ob.total_ask_qty());

    // Compare level-by-level snapshots
    auto b_bids = bb.get_bid_levels();
    auto o_bids = ob.get_bid_levels();
    ASSERT_EQ(b_bids.size(), o_bids.size());
    for (size_t i = 0; i < b_bids.size(); ++i) {
        ASSERT_EQ(b_bids[i].price, o_bids[i].price);
        ASSERT_EQ(b_bids[i].total_quantity, o_bids[i].total_quantity);
        ASSERT_EQ(b_bids[i].order_count, o_bids[i].order_count);
    }

    auto b_asks = bb.get_ask_levels();
    auto o_asks = ob.get_ask_levels();
    ASSERT_EQ(b_asks.size(), o_asks.size());
    for (size_t i = 0; i < b_asks.size(); ++i) {
        ASSERT_EQ(b_asks[i].price, o_asks[i].price);
        ASSERT_EQ(b_asks[i].total_quantity, o_asks[i].total_quantity);
        ASSERT_EQ(b_asks[i].order_count, o_asks[i].order_count);
    }

    // Invariants must hold on both
    std::string b_err, o_err;
    ASSERT_TRUE(baseline_engine.verify_invariants(&b_err));
    ASSERT_TRUE(optimized_engine.verify_invariants(&o_err));
}

TEST_CASE(Differential_MixedWorkload_Seed1) {
    run_differential_test(0x12345678ULL, 1000);
}

TEST_CASE(Differential_MixedWorkload_Seed2) {
    run_differential_test(0xCAFEBABEULL, 1000);
}

TEST_CASE(Differential_MixedWorkload_Seed3) {
    run_differential_test(0xDEADBEEFULL, 1000);
}
