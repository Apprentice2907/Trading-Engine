#include "test_framework.hpp"
#include "hft/flat_order_book.hpp"
#include "hft/flat_matching_engine.hpp"

using namespace hft;

TEST_CASE(FlatOrderBook_BasicAddBuyAndSell) {
    FlatOrderBook book;
    std::vector<Trade> trades;
    uint64_t seq = 0;

    Order buy1{1, 100, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order sell1{2, 105, 15, 15, Side::Sell, OrderType::Limit, 2};

    ASSERT_EQ(book.process_order(buy1, trades, seq), OrderResult::Accepted);
    ASSERT_EQ(book.process_order(sell1, trades, seq), OrderResult::Accepted);
    ASSERT_EQ(trades.size(), 0ULL);

    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_EQ(*book.best_bid(), 100);
    ASSERT_EQ(book.best_bid_qty(), 10);

    ASSERT_TRUE(book.best_ask().has_value());
    ASSERT_EQ(*book.best_ask(), 105);
    ASSERT_EQ(book.best_ask_qty(), 15);

    ASSERT_EQ(book.bid_depth(), 1ULL);
    ASSERT_EQ(book.ask_depth(), 1ULL);
    ASSERT_EQ(book.total_orders(), 2ULL);

    std::string err;
    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(FlatOrderBook_PricePriority) {
    FlatOrderBook book;
    std::vector<Trade> trades;
    uint64_t seq = 0;

    book.process_order(Order{1, 100, 10, 10, Side::Buy, OrderType::Limit, 1}, trades, seq);
    book.process_order(Order{2, 102, 10, 10, Side::Buy, OrderType::Limit, 2}, trades, seq);
    book.process_order(Order{3, 101, 10, 10, Side::Buy, OrderType::Limit, 3}, trades, seq);

    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_EQ(*book.best_bid(), 102);

    auto levels = book.get_bid_levels();
    ASSERT_EQ(levels.size(), 3ULL);
    ASSERT_EQ(levels[0].price, 102);
    ASSERT_EQ(levels[1].price, 101);
    ASSERT_EQ(levels[2].price, 100);

    std::string err;
    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(FlatOrderBook_TimePriorityFIFO) {
    FlatOrderBook book;
    std::vector<Trade> trades;
    uint64_t seq = 0;

    book.process_order(Order{1, 100, 10, 10, Side::Buy, OrderType::Limit, 1}, trades, seq);
    book.process_order(Order{2, 100, 20, 20, Side::Buy, OrderType::Limit, 2}, trades, seq);

    ASSERT_EQ(book.best_bid_qty(), 30);

    // Cross with 15 units -> should fill order 1 completely and 5 of order 2
    Order sell{3, 100, 15, 15, Side::Sell, OrderType::Limit, 3};
    book.process_order(sell, trades, seq);

    ASSERT_EQ(trades.size(), 2ULL);
    ASSERT_EQ(trades[0].resting_order_id, 1ULL);
    ASSERT_EQ(trades[0].quantity, 10);
    ASSERT_EQ(trades[1].resting_order_id, 2ULL);
    ASSERT_EQ(trades[1].quantity, 5);

    ASSERT_FALSE(book.has_order(1));
    ASSERT_TRUE(book.has_order(2));
    ASSERT_EQ(book.best_bid_qty(), 15);

    std::string err;
    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(FlatOrderBook_Cancel) {
    FlatOrderBook book;
    std::vector<Trade> trades;
    uint64_t seq = 0;

    book.process_order(Order{1, 100, 10, 10, Side::Buy, OrderType::Limit, 1}, trades, seq);
    book.process_order(Order{2, 100, 15, 15, Side::Buy, OrderType::Limit, 2}, trades, seq);
    book.process_order(Order{3, 101, 20, 20, Side::Buy, OrderType::Limit, 3}, trades, seq);

    ASSERT_EQ(book.total_orders(), 3ULL);
    ASSERT_EQ(book.bid_depth(), 2ULL);

    // Cancel order 3 (only order at price 101) -> level should be deleted
    ASSERT_EQ(book.cancel(3), OrderResult::Accepted);
    ASSERT_EQ(book.bid_depth(), 1ULL);
    ASSERT_EQ(*book.best_bid(), 100);

    // Cancel order 1 -> order 2 remains at 100
    ASSERT_EQ(book.cancel(1), OrderResult::Accepted);
    ASSERT_EQ(book.bid_depth(), 1ULL);
    ASSERT_EQ(book.best_bid_qty(), 15);

    // Cancel order 2 -> book becomes empty
    ASSERT_EQ(book.cancel(2), OrderResult::Accepted);
    ASSERT_EQ(book.bid_depth(), 0ULL);
    ASSERT_FALSE(book.best_bid().has_value());

    std::string err;
    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(FlatOrderBook_Modify) {
    FlatOrderBook book;
    std::vector<Trade> trades;
    uint64_t seq = 0;

    book.process_order(Order{1, 100, 20, 20, Side::Buy, OrderType::Limit, 1}, trades, seq);
    book.process_order(Order{2, 100, 10, 10, Side::Buy, OrderType::Limit, 2}, trades, seq);

    // Reduce qty: priority retained
    ASSERT_EQ(book.modify(1, 100, 15, trades, seq), OrderResult::Accepted);
    ASSERT_EQ(book.best_bid_qty(), 25);

    // Increase qty: loses priority to order 2
    ASSERT_EQ(book.modify(1, 100, 25, trades, seq), OrderResult::Accepted);
    ASSERT_EQ(book.best_bid_qty(), 35);

    // Sell 15 -> should fill order 2 first (10) and then order 1 (5)
    book.process_order(Order{3, 100, 15, 15, Side::Sell, OrderType::Limit, 3}, trades, seq);
    ASSERT_EQ(trades.size(), 2ULL);
    ASSERT_EQ(trades[0].resting_order_id, 2ULL);
    ASSERT_EQ(trades[0].quantity, 10);
    ASSERT_EQ(trades[1].resting_order_id, 1ULL);
    ASSERT_EQ(trades[1].quantity, 5);

    std::string err;
    ASSERT_TRUE(book.verify_invariants(&err));
}
