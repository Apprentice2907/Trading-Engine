#include "test_framework.hpp"
#include "hft/matching_engine.hpp"
#include "hft/event_pipeline.hpp"

using namespace hft;

static void run_pipeline_equivalence_test(uint64_t seed, int operations_count) {
    // 1. Generate deterministic sequence of operations
    uint64_t lcg_state = seed;
    auto lcg_next = [&lcg_state](uint64_t min, uint64_t max) -> uint64_t {
        lcg_state = lcg_state * 6364136223846793005ULL + 1ULL;
        uint64_t x = lcg_state >> 32;
        return min + (x % (max - min + 1));
    };

    OrderId next_id = 1;
    std::vector<OrderId> active_ids;
    std::vector<OrderEvent> events;
    events.reserve(static_cast<size_t>(operations_count));

    for (int step = 0; step < operations_count; ++step) {
        const uint64_t op = lcg_next(1, 100);

        if (op <= 60 || active_ids.empty()) {
            // Add
            OrderId id = next_id++;
            Side side = (lcg_next(0, 1) == 0) ? Side::Buy : Side::Sell;
            Price price = static_cast<Price>(lcg_next(9950, 10050));
            Quantity qty = static_cast<Quantity>(lcg_next(5, 100));
            events.push_back(OrderEvent::make_add(id, side, price, qty));
            active_ids.push_back(id);
        } else if (op <= 80) {
            // Cancel
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            active_ids[idx] = active_ids.back();
            active_ids.pop_back();
            events.push_back(OrderEvent::make_cancel(id));
        } else {
            // Modify
            size_t idx = static_cast<size_t>(lcg_next(0, active_ids.size() - 1));
            OrderId id = active_ids[idx];
            Price new_price = static_cast<Price>(lcg_next(9960, 10040));
            Quantity new_qty = static_cast<Quantity>(lcg_next(10, 80));
            events.push_back(OrderEvent::make_modify(id, new_price, new_qty));
        }
    }

    // 2. Direct Single-Threaded Execution
    MatchingEngine direct_engine;
    direct_engine.reserve(static_cast<size_t>(operations_count));
    std::vector<OrderResult> direct_results;
    direct_results.reserve(static_cast<size_t>(operations_count));
    std::vector<Trade> direct_trades;
    std::vector<Trade> step_trades;
    step_trades.reserve(16);

    for (const auto& ev : events) {
        step_trades.clear();
        OrderResult res = OrderResult::RejectedUnchanged;
        switch (ev.type) {
            case EventType::Add:
                res = direct_engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, step_trades);
                break;
            case EventType::Cancel:
                res = direct_engine.cancel_order(ev.id);
                break;
            case EventType::Modify:
                res = direct_engine.modify_order(ev.id, ev.price, ev.qty, step_trades);
                break;
        }
        direct_results.push_back(res);
        for (const auto& t : step_trades) {
            direct_trades.push_back(t);
        }
    }

    // 3. Threaded SPSC Pipeline Execution (Producer -> SPSC -> Consumer -> MatchingEngine)
    MatchingEnginePipeline pipeline(MatchingEnginePipeline::DEFAULT_QUEUE_CAPACITY,
                                   static_cast<size_t>(operations_count));
    pipeline.start();

    for (const auto& ev : events) {
        pipeline.enqueue_event_wait(ev);
    }

    pipeline.stop_and_join();

    // 4. Assert Exact Bit-Equality
    const auto& pipe_results = pipeline.results();
    const auto& pipe_trades = pipeline.trades();
    const auto& pipe_engine = pipeline.engine();

    ASSERT_EQ(direct_results.size(), pipe_results.size());
    for (size_t i = 0; i < direct_results.size(); ++i) {
        ASSERT_EQ(direct_results[i], pipe_results[i]);
    }

    ASSERT_EQ(direct_trades.size(), pipe_trades.size());
    for (size_t i = 0; i < direct_trades.size(); ++i) {
        ASSERT_TRUE(direct_trades[i] == pipe_trades[i]);
    }

    // Final book state equivalence
    const auto& db = direct_engine.book();
    const auto& pb = pipe_engine.book();

    ASSERT_EQ(db.best_bid().has_value(), pb.best_bid().has_value());
    if (db.best_bid().has_value()) {
        ASSERT_EQ(*db.best_bid(), *pb.best_bid());
    }

    ASSERT_EQ(db.best_ask().has_value(), pb.best_ask().has_value());
    if (db.best_ask().has_value()) {
        ASSERT_EQ(*db.best_ask(), *pb.best_ask());
    }

    ASSERT_EQ(db.best_bid_qty(), pb.best_bid_qty());
    ASSERT_EQ(db.best_ask_qty(), pb.best_ask_qty());
    ASSERT_EQ(db.bid_depth(), pb.bid_depth());
    ASSERT_EQ(db.ask_depth(), pb.ask_depth());
    ASSERT_EQ(db.total_orders(), pb.total_orders());
    ASSERT_EQ(db.total_bid_qty(), pb.total_bid_qty());
    ASSERT_EQ(db.total_ask_qty(), pb.total_ask_qty());

    // Compare level-by-level
    auto d_bids = db.get_bid_levels();
    auto p_bids = pb.get_bid_levels();
    ASSERT_EQ(d_bids.size(), p_bids.size());
    for (size_t i = 0; i < d_bids.size(); ++i) {
        ASSERT_EQ(d_bids[i].price, p_bids[i].price);
        ASSERT_EQ(d_bids[i].total_quantity, p_bids[i].total_quantity);
        ASSERT_EQ(d_bids[i].order_count, p_bids[i].order_count);
    }

    auto d_asks = db.get_ask_levels();
    auto p_asks = pb.get_ask_levels();
    ASSERT_EQ(d_asks.size(), p_asks.size());
    for (size_t i = 0; i < d_asks.size(); ++i) {
        ASSERT_EQ(d_asks[i].price, p_asks[i].price);
        ASSERT_EQ(d_asks[i].total_quantity, p_asks[i].total_quantity);
        ASSERT_EQ(d_asks[i].order_count, p_asks[i].order_count);
    }

    std::string d_err, p_err;
    ASSERT_TRUE(direct_engine.verify_invariants(&d_err));
    ASSERT_TRUE(pipe_engine.verify_invariants(&p_err));
}

TEST_CASE(Pipeline_DeterministicEquivalence_Seed1) {
    run_pipeline_equivalence_test(0x12345678ULL, 1000);
}

TEST_CASE(Pipeline_DeterministicEquivalence_Seed2) {
    run_pipeline_equivalence_test(0xCAFEBABEULL, 1000);
}

TEST_CASE(Pipeline_DeterministicEquivalence_Seed3) {
    run_pipeline_equivalence_test(0xDEADBEEFULL, 1000);
}
