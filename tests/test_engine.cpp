#include "test_framework.hpp"
#include "hft/order_book.hpp"
#include "hft/matching_engine.hpp"
#include "hft/flat_order_book.hpp"

#include <vector>
#include <random>

using namespace hft;

// ============================================================================
// 1. OrderBook Tests
// ============================================================================

TEST_CASE(OrderBook_BasicAddBuyAndSell) {
    OrderBook book;
    Order buy1{1, 10000, 100, 100, Side::Buy, OrderType::Limit, 1};
    Order sell1{2, 10050, 50, 50, Side::Sell, OrderType::Limit, 2};

    ASSERT_EQ(book.add_order(buy1), OrderResult::Accepted);
    ASSERT_EQ(book.add_order(sell1), OrderResult::Accepted);

    ASSERT_EQ(book.best_bid().value(), 10000);
    ASSERT_EQ(book.best_ask().value(), 10050);
    ASSERT_EQ(book.total_orders(), 2u);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(OrderBook_PricePriority) {
    OrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order b2{2, 10020, 10, 10, Side::Buy, OrderType::Limit, 2};
    Order b3{3, 10010, 10, 10, Side::Buy, OrderType::Limit, 3};

    book.add_order(b1);
    book.add_order(b2);
    book.add_order(b3);

    ASSERT_EQ(book.best_bid().value(), 10020);

    Order s1{4, 10080, 10, 10, Side::Sell, OrderType::Limit, 4};
    Order s2{5, 10050, 10, 10, Side::Sell, OrderType::Limit, 5};
    Order s3{6, 10070, 10, 10, Side::Sell, OrderType::Limit, 6};

    book.add_order(s1);
    book.add_order(s2);
    book.add_order(s3);

    ASSERT_EQ(book.best_ask().value(), 10050);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(OrderBook_TimePriorityFIFO) {
    OrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order b2{2, 10000, 20, 20, Side::Buy, OrderType::Limit, 2};
    Order b3{3, 10000, 30, 30, Side::Buy, OrderType::Limit, 3};

    book.add_order(b1);
    book.add_order(b2);
    book.add_order(b3);

    auto bids = book.get_bid_levels();
    ASSERT_EQ(bids.size(), 1u);
    ASSERT_EQ(bids[0].order_count, 3u);
    ASSERT_EQ(bids[0].total_quantity, 60u);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(OrderBook_Cancel) {
    OrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order b2{2, 10000, 20, 20, Side::Buy, OrderType::Limit, 2};

    book.add_order(b1);
    book.add_order(b2);

    ASSERT_EQ(book.cancel_order(1), OrderResult::Accepted);
    ASSERT_FALSE(book.has_order(1));
    ASSERT_TRUE(book.has_order(2));
    ASSERT_EQ(book.total_orders(), 1u);
    ASSERT_EQ(book.total_bid_qty(), 20u);

    ASSERT_EQ(book.cancel_order(999), OrderResult::RejectedOrderNotFound);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(OrderBook_Modify) {
    OrderBook book;
    Order b1{1, 10000, 50, 50, Side::Buy, OrderType::Limit, 1};
    book.add_order(b1);

    // Quantity reduction retains queue position
    ASSERT_EQ(book.modify_order(1, 10000, 30), OrderResult::Accepted);
    auto opt = book.get_order(1);
    ASSERT_TRUE(opt.has_value());
    ASSERT_EQ(opt->remaining_qty, 30u);
    ASSERT_EQ(book.total_bid_qty(), 30u);

    // Price change
    ASSERT_EQ(book.modify_order(1, 10010, 30), OrderResult::Accepted);
    ASSERT_EQ(book.best_bid().value(), 10010);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(OrderBook_InvariantsHoldOnCrossedCheck) {
    OrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order s1{2, 10050, 10, 10, Side::Sell, OrderType::Limit, 2};
    book.add_order(b1);
    book.add_order(s1);

    std::string err;
    ASSERT_TRUE(book.verify_invariants(&err));
}

// ============================================================================
// 2. MatchingEngine Tests
// ============================================================================

TEST_CASE(MatchingEngine_ExactFillAndRemoval) {
    MatchingEngine engine;
    std::vector<Trade> trades;

    engine.submit_limit_order(1, Side::Buy, 10000, 50, trades);
    ASSERT_EQ(trades.size(), 0u);
    ASSERT_EQ(engine.book().total_orders(), 1u);

    engine.submit_limit_order(2, Side::Sell, 10000, 50, trades);
    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].quantity, 50u);
    ASSERT_EQ(trades[0].price, 10000);
    ASSERT_EQ(engine.book().total_orders(), 0u);
    ASSERT_TRUE(engine.verify_invariants());
}

TEST_CASE(MatchingEngine_PartialFillAndResidualResting) {
    MatchingEngine engine;
    std::vector<Trade> trades;

    engine.submit_limit_order(1, Side::Buy, 10000, 50, trades);
    trades.clear();

    engine.submit_limit_order(2, Side::Sell, 10000, 30, trades);
    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].quantity, 30u);
    ASSERT_EQ(engine.book().total_orders(), 1u);
    ASSERT_EQ(engine.book().total_bid_qty(), 20u);
    ASSERT_TRUE(engine.verify_invariants());
}

TEST_CASE(MatchingEngine_MultiLevelSweep) {
    MatchingEngine engine;
    std::vector<Trade> trades;

    engine.submit_limit_order(1, Side::Sell, 10010, 20, trades);
    engine.submit_limit_order(2, Side::Sell, 10020, 30, trades);
    engine.submit_limit_order(3, Side::Sell, 10030, 50, trades);
    trades.clear();

    engine.submit_limit_order(4, Side::Buy, 10025, 60, trades);
    ASSERT_EQ(trades.size(), 2u);
    ASSERT_EQ(trades[0].price, 10010);
    ASSERT_EQ(trades[0].quantity, 20u);
    ASSERT_EQ(trades[1].price, 10020);
    ASSERT_EQ(trades[1].quantity, 30u);

    // Remaining 10 rests at 10025
    ASSERT_EQ(engine.book().best_bid().value(), 10025);
    ASSERT_EQ(engine.book().best_bid_qty(), 10u);
    ASSERT_EQ(engine.book().best_ask().value(), 10030);
    ASSERT_TRUE(engine.verify_invariants());
}

TEST_CASE(MatchingEngine_ExecutionPriceIsRestingPrice) {
    MatchingEngine engine;
    std::vector<Trade> trades;

    engine.submit_limit_order(1, Side::Sell, 10000, 50, trades);
    trades.clear();

    // Aggressive buy at 10050 crosses resting sell at 10000; execution price must be 10000
    engine.submit_limit_order(2, Side::Buy, 10050, 50, trades);
    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].price, 10000);
    ASSERT_TRUE(engine.verify_invariants());
}

TEST_CASE(MatchingEngine_EdgeCases) {
    MatchingEngine engine;
    std::vector<Trade> trades;

    ASSERT_EQ(engine.submit_limit_order(0, Side::Buy, 10000, 10, trades), OrderResult::RejectedDuplicateId);
    ASSERT_EQ(engine.submit_limit_order(1, Side::Buy, 0, 10, trades), OrderResult::RejectedInvalidPrice);
    ASSERT_EQ(engine.submit_limit_order(1, Side::Buy, 10000, 0, trades), OrderResult::RejectedInvalidQuantity);

    engine.submit_limit_order(10, Side::Buy, 10000, 10, trades);
    ASSERT_EQ(engine.submit_limit_order(10, Side::Buy, 10000, 10, trades), OrderResult::RejectedDuplicateId);
    ASSERT_TRUE(engine.verify_invariants());
}

TEST_CASE(MatchingEngine_DeterministicReplay) {
    auto run_seq = [](MatchingEngine& eng) {
        std::vector<Trade> t;
        eng.submit_limit_order(1, Side::Buy, 10000, 100, t);
        eng.submit_limit_order(2, Side::Buy, 9990, 50, t);
        eng.submit_limit_order(3, Side::Sell, 10010, 80, t);
        eng.submit_limit_order(4, Side::Sell, 10000, 60, t);
        eng.cancel_order(2);
        eng.modify_order(1, 10000, 20, t);
        return eng.total_trades_generated();
    };

    MatchingEngine e1, e2;
    uint64_t t1 = run_seq(e1);
    uint64_t t2 = run_seq(e2);
    ASSERT_EQ(t1, t2);
    ASSERT_EQ(e1.book().total_orders(), e2.book().total_orders());
}

// ============================================================================
// 3. OrderPool Tests
// ============================================================================

TEST_CASE(OrderPool_AllocationAndRecycling) {
    OrderPool pool(16);
    Order o1{1, 100, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order o2{2, 200, 20, 20, Side::Sell, OrderType::Limit, 2};

    OrderIndex idx1 = pool.allocate(o1);
    OrderIndex idx2 = pool.allocate(o2);
    ASSERT_EQ(pool.size(), 2u);

    pool.deallocate(idx1);
    ASSERT_EQ(pool.size(), 1u);

    OrderIndex idx3 = pool.allocate(o1);
    ASSERT_EQ(idx3, idx1); // Recycled same index
    ASSERT_EQ(pool.size(), 2u);
}

TEST_CASE(OrderPool_AutomaticExpansion) {
    OrderPool pool(4);
    for (uint64_t i = 1; i <= 20; ++i) {
        Order o{i, 100, 10, 10, Side::Buy, OrderType::Limit, i};
        pool.allocate(o);
    }
    ASSERT_EQ(pool.size(), 20u);
    ASSERT_TRUE(pool.capacity() >= 20u);
}

TEST_CASE(OrderPool_ClearAndReuse) {
    OrderPool pool(10);
    for (uint64_t i = 1; i <= 5; ++i) {
        Order o{i, 100, 10, 10, Side::Buy, OrderType::Limit, i};
        pool.allocate(o);
    }
    ASSERT_EQ(pool.size(), 5u);
    pool.clear();
    ASSERT_EQ(pool.size(), 0u);

    Order o{100, 100, 10, 10, Side::Buy, OrderType::Limit, 1};
    OrderIndex idx = pool.allocate(o);
    ASSERT_EQ(idx, 0u);
    ASSERT_EQ(pool.size(), 1u);
}

// ============================================================================
// 4. Deterministic Differential Tests (Map Engine Multi-Seed)
// ============================================================================

static void run_engine_consistency_test(uint64_t seed, int op_count) {
    MatchingEngine e1, e2;
    e1.reserve(static_cast<size_t>(op_count));
    e2.reserve(static_cast<size_t>(op_count));

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> op_dist(1, 100);
    std::uniform_int_distribution<int64_t> px_dist(9900, 10100);
    std::uniform_int_distribution<uint64_t> qty_dist(5, 100);

    OrderId next_id = 1;
    std::vector<OrderId> live_ids;
    std::vector<Trade> t1, t2;

    for (int i = 0; i < op_count; ++i) {
        t1.clear();
        t2.clear();
        uint64_t op = op_dist(rng);

        if (op <= 65 || live_ids.empty()) {
            OrderId id = next_id++;
            Side side = (op % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = px_dist(rng);
            Quantity q = qty_dist(rng);

            auto r1 = e1.submit_limit_order(id, side, p, q, t1);
            auto r2 = e2.submit_limit_order(id, side, p, q, t2);
            ASSERT_EQ(r1, r2);
            ASSERT_EQ(t1.size(), t2.size());
            if (r1 == OrderResult::Accepted && e1.book().has_order(id)) {
                live_ids.push_back(id);
            }
        } else {
            size_t idx = static_cast<size_t>(rng() % live_ids.size());
            OrderId id = live_ids[idx];
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();

            auto r1 = e1.cancel_order(id);
            auto r2 = e2.cancel_order(id);
            ASSERT_EQ(r1, r2);
        }
    }

    ASSERT_EQ(e1.total_trades_generated(), e2.total_trades_generated());
    ASSERT_EQ(e1.book().total_orders(), e2.book().total_orders());
    ASSERT_TRUE(e1.verify_invariants());
    ASSERT_TRUE(e2.verify_invariants());
}

TEST_CASE(Differential_MixedWorkload_Seed1) {
    run_engine_consistency_test(0x12345678ULL, 5000);
}

TEST_CASE(Differential_MixedWorkload_Seed2) {
    run_engine_consistency_test(0x87654321ULL, 5000);
}

TEST_CASE(Differential_MixedWorkload_Seed3) {
    run_engine_consistency_test(0xCAFEBABEULL, 5000);
}

// ============================================================================
// 5. FlatOrderBook Tests
// ============================================================================

TEST_CASE(FlatOrderBook_BasicAddBuyAndSell) {
    FlatOrderBook book;
    Order buy1{1, 10000, 100, 100, Side::Buy, OrderType::Limit, 1};
    Order sell1{2, 10050, 50, 50, Side::Sell, OrderType::Limit, 2};

    ASSERT_EQ(book.add_resting_order(buy1), OrderResult::Accepted);
    ASSERT_EQ(book.add_resting_order(sell1), OrderResult::Accepted);

    ASSERT_EQ(book.best_bid().value(), 10000);
    ASSERT_EQ(book.best_ask().value(), 10050);
    ASSERT_EQ(book.total_orders(), 2u);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(FlatOrderBook_PricePriority) {
    FlatOrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order b2{2, 10020, 10, 10, Side::Buy, OrderType::Limit, 2};
    Order b3{3, 10010, 10, 10, Side::Buy, OrderType::Limit, 3};

    book.add_resting_order(b1);
    book.add_resting_order(b2);
    book.add_resting_order(b3);

    ASSERT_EQ(book.best_bid().value(), 10020);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(FlatOrderBook_TimePriorityFIFO) {
    FlatOrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order b2{2, 10000, 20, 20, Side::Buy, OrderType::Limit, 2};

    book.add_resting_order(b1);
    book.add_resting_order(b2);

    auto bids = book.get_bid_levels();
    ASSERT_EQ(bids.size(), 1u);
    ASSERT_EQ(bids[0].order_count, 2u);
    ASSERT_EQ(bids[0].total_quantity, 30u);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(FlatOrderBook_Cancel) {
    FlatOrderBook book;
    Order b1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    book.add_resting_order(b1);

    ASSERT_EQ(book.cancel(1), OrderResult::Accepted);
    ASSERT_FALSE(book.has_order(1));
    ASSERT_EQ(book.cancel(999), OrderResult::RejectedOrderNotFound);
    ASSERT_TRUE(book.verify_invariants());
}

TEST_CASE(FlatOrderBook_Modify) {
    FlatOrderBook book;
    Order b1{1, 10000, 50, 50, Side::Buy, OrderType::Limit, 1};
    book.add_resting_order(b1);

    std::vector<Trade> trades;
    uint64_t seq = 0;
    ASSERT_EQ(book.modify(1, 10000, 30, trades, seq), OrderResult::Accepted);
    auto opt = book.get_order(1);
    ASSERT_TRUE(opt.has_value());
    ASSERT_EQ(opt->remaining_qty, 30u);
    ASSERT_TRUE(book.verify_invariants());
}

// ============================================================================
// 6. Differential Tests (MapOrderBook vs FlatOrderBook)
// ============================================================================

static void run_map_vs_flat_differential(uint64_t seed, int op_count) {
    MapMatchingEngine map_engine;
    FlatMatchingEngine flat_engine;

    map_engine.reserve(static_cast<size_t>(op_count));
    flat_engine.reserve(static_cast<size_t>(op_count));

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> op_dist(1, 100);
    std::uniform_int_distribution<int64_t> px_dist(9950, 10050);
    std::uniform_int_distribution<uint64_t> qty_dist(5, 100);

    OrderId next_id = 1;
    std::vector<OrderId> live_ids;
    std::vector<Trade> map_trades, flat_trades;

    for (int i = 0; i < op_count; ++i) {
        map_trades.clear();
        flat_trades.clear();
        uint64_t op = op_dist(rng);

        if (op <= 65 || live_ids.empty()) {
            OrderId id = next_id++;
            Side side = (op % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = px_dist(rng);
            Quantity q = qty_dist(rng);

            auto m_res = map_engine.submit_limit_order(id, side, p, q, map_trades);
            auto f_res = flat_engine.submit_limit_order(id, side, p, q, flat_trades);
            ASSERT_EQ(m_res, f_res);
            ASSERT_EQ(map_trades.size(), flat_trades.size());
            for (size_t t = 0; t < map_trades.size(); ++t) {
                ASSERT_TRUE(map_trades[t] == flat_trades[t]);
            }
            if (m_res == OrderResult::Accepted && map_engine.book().has_order(id)) {
                live_ids.push_back(id);
            }
        } else {
            size_t idx = static_cast<size_t>(rng() % live_ids.size());
            OrderId id = live_ids[idx];
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();

            auto m_res = map_engine.cancel_order(id);
            auto f_res = flat_engine.cancel_order(id);
            ASSERT_EQ(m_res, f_res);
        }
    }

    ASSERT_EQ(map_engine.total_trades_generated(), flat_engine.total_trades_generated());
    ASSERT_EQ(map_engine.book().total_orders(), flat_engine.book().total_orders());
    ASSERT_TRUE(map_engine.verify_invariants());
    ASSERT_TRUE(flat_engine.verify_invariants());
}

TEST_CASE(DifferentialFlat_MixedWorkload_Seed1) {
    run_map_vs_flat_differential(0x11223344ULL, 5000);
}

TEST_CASE(DifferentialFlat_MixedWorkload_Seed2) {
    run_map_vs_flat_differential(0x55667788ULL, 5000);
}

TEST_CASE(DifferentialFlat_MixedWorkload_Seed3) {
    run_map_vs_flat_differential(0x99AABBCCULL, 5000);
}
