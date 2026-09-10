#include "test_framework.hpp"
#include "hft/matching_engine.hpp"

using namespace hft;

struct ScenarioResult {
    std::vector<Trade> trades;
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    Quantity best_bid_qty{0};
    Quantity best_ask_qty{0};
    size_t total_orders{0};
    Quantity total_bid_qty{0};
    Quantity total_ask_qty{0};
    bool invariants_ok{false};
    std::string invariant_err;
};


static ScenarioResult run_simulation_script() {
    MatchingEngine engine;
    std::vector<Trade> all_trades;
    std::vector<Trade> step_trades;

    // Fixed deterministic pseudo-random sequence (Linear Congruential Generator)
    // No reliance on system clocks or external state
    uint64_t lcg_state = 123456789ULL;
    auto lcg_next = [&lcg_state](uint64_t min, uint64_t max) -> uint64_t {
        lcg_state = lcg_state * 6364136223846793005ULL + 1ULL;
        uint64_t x = lcg_state >> 32;
        return min + (x % (max - min + 1));
    };


    OrderId next_id = 1;
    std::vector<OrderId> active_ids;

    // Run 500 deterministic operations
    for (int step = 0; step < 500; ++step) {
        step_trades.clear();
        const uint64_t op = lcg_next(1, 100);

        if (op <= 60 || active_ids.empty()) {
            // 60% Add Limit Order
            OrderId id = next_id++;
            Side side = (lcg_next(0, 1) == 0) ? Side::Buy : Side::Sell;
            Price price = static_cast<Price>(lcg_next(9900, 10100));
            Quantity qty = static_cast<Quantity>(lcg_next(1, 100));

            auto res = engine.submit_limit_order(id, side, price, qty, step_trades);
            if (res == OrderResult::Accepted) {
                if (engine.book().has_order(id)) {
                    active_ids.push_back(id);
                }
            }
        } else if (op <= 80) {
            // 20% Cancel random order
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();

            engine.cancel_order(id);
        } else {
            // 20% Modify random order
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            Price new_price = static_cast<Price>(lcg_next(9950, 10050));
            Quantity new_qty = static_cast<Quantity>(lcg_next(5, 80));

            engine.modify_order(id, new_price, new_qty, step_trades);
            if (!engine.book().has_order(id)) {
                active_ids[idx] = active_ids.back();
                active_ids.pop_back();
            }
        }

        // Collect trades
        for (const auto& t : step_trades) {
            all_trades.push_back(t);
        }
    }

    ScenarioResult result;
    result.trades = all_trades;
    result.best_bid = engine.book().best_bid();
    result.best_ask = engine.book().best_ask();
    result.best_bid_qty = engine.book().best_bid_qty();
    result.best_ask_qty = engine.book().best_ask_qty();
    result.total_orders = engine.book().total_orders();
    result.total_bid_qty = engine.book().total_bid_qty();
    result.total_ask_qty = engine.book().total_ask_qty();
    result.invariants_ok = engine.verify_invariants(&result.invariant_err);

    return result;
}

TEST_CASE(MatchingEngine_DeterministicReplay) {
    // Run the scenario twice independently from a clean state
    ScenarioResult run1 = run_simulation_script();
    ScenarioResult run2 = run_simulation_script();

    // Verify invariants held throughout and at the end of both runs
    ASSERT_TRUE(run1.invariants_ok);
    ASSERT_TRUE(run2.invariants_ok);

    // Verify trade executions are non-empty and identical
    ASSERT_GT(run1.trades.size(), 0);
    ASSERT_EQ(run1.trades.size(), run2.trades.size());

    for (size_t i = 0; i < run1.trades.size(); ++i) {
        ASSERT_TRUE(run1.trades[i] == run2.trades[i]);
    }

    // Verify order book final state is bit-identical
    ASSERT_EQ(run1.best_bid.has_value(), run2.best_bid.has_value());
    if (run1.best_bid.has_value()) {
        ASSERT_EQ(*run1.best_bid, *run2.best_bid);
    }

    ASSERT_EQ(run1.best_ask.has_value(), run2.best_ask.has_value());
    if (run1.best_ask.has_value()) {
        ASSERT_EQ(*run1.best_ask, *run2.best_ask);
    }

    ASSERT_EQ(run1.best_bid_qty, run2.best_bid_qty);
    ASSERT_EQ(run1.best_ask_qty, run2.best_ask_qty);
    ASSERT_EQ(run1.total_orders, run2.total_orders);
    ASSERT_EQ(run1.total_bid_qty, run2.total_bid_qty);
    ASSERT_EQ(run1.total_ask_qty, run2.total_ask_qty);
}

