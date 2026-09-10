#include "test_framework.hpp"
#include "hft/matching_engine.hpp"

using namespace hft;

TEST_CASE(MatchingEngine_ExactFillAndRemoval) {
    MatchingEngine engine;
    std::vector<Trade> trades;
    std::string err;

    // Resting sell limit: 100 shares @ 100.50
    ASSERT_EQ(engine.submit_limit_order(1, Side::Sell, 10050, 100, trades), OrderResult::Accepted);
    ASSERT_EQ(trades.size(), 0);
    ASSERT_EQ(engine.book().ask_depth(), 1);

    // Incoming buy limit: exactly 100 shares @ 100.50
    ASSERT_EQ(engine.submit_limit_order(2, Side::Buy, 10050, 100, trades), OrderResult::Accepted);
    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].trade_id, 1);
    ASSERT_EQ(trades[0].resting_order_id, 1);
    ASSERT_EQ(trades[0].incoming_order_id, 2);
    ASSERT_EQ(trades[0].price, 10050);
    ASSERT_EQ(trades[0].quantity, 100);
    ASSERT_EQ(trades[0].aggressor_side, Side::Buy);

    // Book must now be completely empty
    ASSERT_EQ(engine.book().ask_depth(), 0);
    ASSERT_EQ(engine.book().bid_depth(), 0);
    ASSERT_FALSE(engine.book().has_order(1));
    ASSERT_FALSE(engine.book().has_order(2));

    ASSERT_TRUE(engine.verify_invariants(&err));
}

TEST_CASE(MatchingEngine_PartialFillAndResidualResting) {
    MatchingEngine engine;
    std::vector<Trade> trades;
    std::string err;

    // Resting buy limit: 100 shares @ 100.00
    ASSERT_EQ(engine.submit_limit_order(10, Side::Buy, 10000, 100, trades), OrderResult::Accepted);

    // Incoming sell limit: 40 shares @ 100.00
    ASSERT_EQ(engine.submit_limit_order(20, Side::Sell, 10000, 40, trades), OrderResult::Accepted);
    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].trade_id, 1);
    ASSERT_EQ(trades[0].quantity, 40);
    ASSERT_EQ(trades[0].aggressor_side, Side::Sell);

    // Resting order 10 should still be live with 60 remaining shares
    ASSERT_TRUE(engine.book().has_order(10));
    auto o10 = engine.book().get_order(10);
    ASSERT_TRUE(o10.has_value());
    ASSERT_EQ(o10->remaining_qty, 60);
    ASSERT_EQ(engine.book().best_bid_qty(), 60);

    // Incoming sell was fully filled and should not be in book
    ASSERT_FALSE(engine.book().has_order(20));

    // Incoming sell limit: 90 shares @ 100.00 (consumes 60, 30 rests in book)
    trades.clear();
    ASSERT_EQ(engine.submit_limit_order(30, Side::Sell, 10000, 90, trades), OrderResult::Accepted);
    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].quantity, 60);
    ASSERT_EQ(trades[0].resting_order_id, 10);
    ASSERT_EQ(trades[0].incoming_order_id, 30);

    // Order 10 is now gone
    ASSERT_FALSE(engine.book().has_order(10));

    // Order 30 now rests with 30 shares as ask @ 10000
    ASSERT_TRUE(engine.book().has_order(30));
    auto o30 = engine.book().get_order(30);
    ASSERT_TRUE(o30.has_value());
    ASSERT_EQ(o30->remaining_qty, 30);
    ASSERT_EQ(*engine.book().best_ask(), 10000);
    ASSERT_EQ(engine.book().best_ask_qty(), 30);

    ASSERT_TRUE(engine.verify_invariants(&err));
}

TEST_CASE(MatchingEngine_MultiLevelSweep) {
    MatchingEngine engine;
    std::vector<Trade> trades;
    std::string err;

    // Resting asks:
    // 100.10: 20 shares (order 1)
    // 100.20: 30 shares (order 2)
    // 100.30: 50 shares (order 3)
    engine.submit_limit_order(1, Side::Sell, 10010, 20, trades);
    engine.submit_limit_order(2, Side::Sell, 10020, 30, trades);
    engine.submit_limit_order(3, Side::Sell, 10030, 50, trades);
    ASSERT_EQ(engine.book().ask_depth(), 3);

    // Aggressive buy order: 65 shares @ 100.25
    // Should consume all of order 1 (20 @ 100.10)
    // Should consume all of order 2 (30 @ 100.20)
    // Cannot execute against order 3 (100.30 > 100.25)
    // Remaining 15 shares should rest as bid @ 100.25
    trades.clear();
    ASSERT_EQ(engine.submit_limit_order(100, Side::Buy, 10025, 65, trades), OrderResult::Accepted);

    ASSERT_EQ(trades.size(), 2);
    ASSERT_EQ(trades[0].resting_order_id, 1);
    ASSERT_EQ(trades[0].price, 10010);
    ASSERT_EQ(trades[0].quantity, 20);

    ASSERT_EQ(trades[1].resting_order_id, 2);
    ASSERT_EQ(trades[1].price, 10020);
    ASSERT_EQ(trades[1].quantity, 30);

    // Residual order 100 has 15 shares resting at 100.25
    ASSERT_TRUE(engine.book().has_order(100));
    ASSERT_EQ(*engine.book().best_bid(), 10025);
    ASSERT_EQ(engine.book().best_bid_qty(), 15);

    // Best ask is now order 3 at 100.30
    ASSERT_TRUE(engine.book().has_order(3));
    ASSERT_EQ(*engine.book().best_ask(), 10030);
    ASSERT_EQ(engine.book().best_ask_qty(), 50);

    ASSERT_TRUE(engine.verify_invariants(&err));
}

TEST_CASE(MatchingEngine_ExecutionPriceIsRestingPrice) {
    MatchingEngine engine;
    std::vector<Trade> trades;
    std::string err;

    // Resting sell order at 100.00
    engine.submit_limit_order(1, Side::Sell, 10000, 50, trades);

    // Aggressive buy order at 100.50 (willing to pay up to 100.50)
    // Trade execution MUST be at resting price (100.00), not aggressor price (100.50)
    trades.clear();
    engine.submit_limit_order(2, Side::Buy, 10050, 50, trades);

    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].price, 10000);
    ASSERT_EQ(trades[0].quantity, 50);

    ASSERT_TRUE(engine.verify_invariants(&err));
}

TEST_CASE(MatchingEngine_EdgeCases) {
    MatchingEngine engine;
    std::vector<Trade> trades;
    std::string err;

    // Zero quantity rejected
    ASSERT_EQ(engine.submit_limit_order(1, Side::Buy, 10000, 0, trades),
              OrderResult::RejectedInvalidQuantity);

    // Zero or negative price rejected
    ASSERT_EQ(engine.submit_limit_order(2, Side::Buy, 0, 10, trades),
              OrderResult::RejectedInvalidPrice);
    ASSERT_EQ(engine.submit_limit_order(3, Side::Buy, -500, 10, trades),
              OrderResult::RejectedInvalidPrice);

    // Zero ID rejected
    ASSERT_EQ(engine.submit_limit_order(0, Side::Buy, 10000, 10, trades),
              OrderResult::RejectedDuplicateId);

    // Valid order
    ASSERT_EQ(engine.submit_limit_order(10, Side::Buy, 10000, 100, trades),
              OrderResult::Accepted);

    // Duplicate order ID rejected
    ASSERT_EQ(engine.submit_limit_order(10, Side::Buy, 10000, 50, trades),
              OrderResult::RejectedDuplicateId);

    // Cancel nonexistent order
    ASSERT_EQ(engine.cancel_order(999), OrderResult::RejectedOrderNotFound);

    // Modify nonexistent order
    ASSERT_EQ(engine.modify_order(999, 10000, 50, trades), OrderResult::RejectedOrderNotFound);

    // Modify with invalid price or quantity
    ASSERT_EQ(engine.modify_order(10, -100, 50, trades), OrderResult::RejectedInvalidPrice);
    ASSERT_EQ(engine.modify_order(10, 10000, 0, trades), OrderResult::RejectedInvalidQuantity);

    ASSERT_TRUE(engine.verify_invariants(&err));
}
