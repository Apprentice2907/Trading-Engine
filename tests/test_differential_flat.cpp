#include "test_framework.hpp"
#include "hft/matching_engine.hpp"
#include "hft/flat_matching_engine.hpp"

using namespace hft;

static void run_flat_differential_test(uint64_t seed, int operations_count) {
    MapMatchingEngine map_engine;
    FlatMatchingEngine flat_engine;

    map_engine.reserve(static_cast<size_t>(operations_count));
    flat_engine.reserve(static_cast<size_t>(operations_count), 2048);

    uint64_t lcg_state = seed;
    auto lcg_next = [&lcg_state](uint64_t min, uint64_t max) -> uint64_t {
        lcg_state = lcg_state * 6364136223846793005ULL + 1ULL;
        uint64_t x = lcg_state >> 32;
        return min + (x % (max - min + 1));
    };

    OrderId next_id = 1;
    std::vector<OrderId> active_ids;

    std::vector<Trade> map_step_trades;
    std::vector<Trade> flat_step_trades;

    std::vector<Trade> map_all_trades;
    std::vector<Trade> flat_all_trades;

    for (int step = 0; step < operations_count; ++step) {
        map_step_trades.clear();
        flat_step_trades.clear();

        const uint64_t op = lcg_next(1, 100);

        if (op <= 60 || active_ids.empty()) {
            // 60% Add Limit Order
            OrderId id = next_id++;
            Side side = (lcg_next(0, 1) == 0) ? Side::Buy : Side::Sell;
            Price price = static_cast<Price>(lcg_next(9950, 10050));
            Quantity qty = static_cast<Quantity>(lcg_next(5, 100));

            auto m_res = map_engine.submit_limit_order(id, side, price, qty, map_step_trades);
            auto f_res = flat_engine.submit_limit_order(id, side, price, qty, flat_step_trades);

            ASSERT_EQ(m_res, f_res);

            if (m_res == OrderResult::Accepted) {
                if (map_engine.book().has_order(id)) {
                    active_ids.push_back(id);
                }
            }
        } else if (op <= 80) {
            // 20% Cancel random order
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();

            auto m_res = map_engine.cancel_order(id);
            auto f_res = flat_engine.cancel_order(id);
            ASSERT_EQ(m_res, f_res);
        } else {
            // 20% Modify random order
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            Price new_price = static_cast<Price>(lcg_next(9960, 10040));
            Quantity new_qty = static_cast<Quantity>(lcg_next(10, 80));

            auto m_res = map_engine.modify_order(id, new_price, new_qty, map_step_trades);
            auto f_res = flat_engine.modify_order(id, new_price, new_qty, flat_step_trades);
            ASSERT_EQ(m_res, f_res);

            if (!map_engine.book().has_order(id)) {
                active_ids[idx] = active_ids.back();
                active_ids.pop_back();
            }
        }

        // Verify trades generated in this step match bit-for-bit
        ASSERT_EQ(map_step_trades.size(), flat_step_trades.size());
        for (size_t t = 0; t < map_step_trades.size(); ++t) {
            ASSERT_TRUE(map_step_trades[t] == flat_step_trades[t]);
            map_all_trades.push_back(map_step_trades[t]);
            flat_all_trades.push_back(flat_step_trades[t]);
        }
    }

    // Comprehensive Final State Equivalence Verification
    ASSERT_EQ(map_all_trades.size(), flat_all_trades.size());
    ASSERT_EQ(map_engine.total_trades_generated(), flat_engine.total_trades_generated());

    const auto& mb = map_engine.book();
    const auto& fb = flat_engine.book();

    ASSERT_EQ(mb.best_bid().has_value(), fb.best_bid().has_value());
    if (mb.best_bid().has_value()) {
        ASSERT_EQ(*mb.best_bid(), *fb.best_bid());
    }

    ASSERT_EQ(mb.best_ask().has_value(), fb.best_ask().has_value());
    if (mb.best_ask().has_value()) {
        ASSERT_EQ(*mb.best_ask(), *fb.best_ask());
    }

    ASSERT_EQ(mb.best_bid_qty(), fb.best_bid_qty());
    ASSERT_EQ(mb.best_ask_qty(), fb.best_ask_qty());
    ASSERT_EQ(mb.bid_depth(), fb.bid_depth());
    ASSERT_EQ(mb.ask_depth(), fb.ask_depth());
    ASSERT_EQ(mb.total_orders(), fb.total_orders());
    ASSERT_EQ(mb.total_bid_qty(), fb.total_bid_qty());
    ASSERT_EQ(mb.total_ask_qty(), fb.total_ask_qty());

    // Compare level-by-level snapshots
    auto m_bids = mb.get_bid_levels();
    auto f_bids = fb.get_bid_levels();
    ASSERT_EQ(m_bids.size(), f_bids.size());
    for (size_t i = 0; i < m_bids.size(); ++i) {
        ASSERT_EQ(m_bids[i].price, f_bids[i].price);
        ASSERT_EQ(m_bids[i].total_quantity, f_bids[i].total_quantity);
        ASSERT_EQ(m_bids[i].order_count, f_bids[i].order_count);
    }

    auto m_asks = mb.get_ask_levels();
    auto f_asks = fb.get_ask_levels();
    ASSERT_EQ(m_asks.size(), f_asks.size());
    for (size_t i = 0; i < m_asks.size(); ++i) {
        ASSERT_EQ(m_asks[i].price, f_asks[i].price);
        ASSERT_EQ(m_asks[i].total_quantity, f_asks[i].total_quantity);
        ASSERT_EQ(m_asks[i].order_count, f_asks[i].order_count);
    }

    // Invariants must hold on both
    std::string m_err, f_err;
    ASSERT_TRUE(map_engine.verify_invariants(&m_err));
    ASSERT_TRUE(flat_engine.verify_invariants(&f_err));
}

TEST_CASE(DifferentialFlat_MixedWorkload_Seed1) {
    run_flat_differential_test(0x12345678ULL, 1000);
}

TEST_CASE(DifferentialFlat_MixedWorkload_Seed2) {
    run_flat_differential_test(0xCAFEBABEULL, 1000);
}

TEST_CASE(DifferentialFlat_MixedWorkload_Seed3) {
    run_flat_differential_test(0xDEADBEEFULL, 1000);
}
