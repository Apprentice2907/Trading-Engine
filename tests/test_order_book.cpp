#include "test_framework.hpp"
#include "hft/order_book.hpp"

using namespace hft;

TEST_CASE(OrderBook_BasicAddBuyAndSell) {
    OrderBook book;
    std::string err;

    Order buy1{1, 10000, 50, 50, Side::Buy, OrderType::Limit, 1};
    ASSERT_EQ(book.add_resting_order(buy1), OrderResult::Accepted);
    ASSERT_TRUE(book.has_order(1));
    ASSERT_EQ(book.bid_depth(), 1);
    ASSERT_EQ(*book.best_bid(), 10000);
    ASSERT_EQ(book.best_bid_qty(), 50);

    Order sell1{2, 10050, 40, 40, Side::Sell, OrderType::Limit, 2};
    ASSERT_EQ(book.add_resting_order(sell1), OrderResult::Accepted);
    ASSERT_TRUE(book.has_order(2));
    ASSERT_EQ(book.ask_depth(), 1);
    ASSERT_EQ(*book.best_ask(), 10050);
    ASSERT_EQ(book.best_ask_qty(), 40);

    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(OrderBook_PricePriority) {
    OrderBook book;
    std::string err;

    // Add bids in arbitrary order: 10000, 10020, 9980
    book.add_resting_order(Order{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1});
    book.add_resting_order(Order{2, 10020, 20, 20, Side::Buy, OrderType::Limit, 2});
    book.add_resting_order(Order{3, 9980,  30, 30, Side::Buy, OrderType::Limit, 3});

    // Best bid must be the highest price (10020)
    ASSERT_EQ(*book.best_bid(), 10020);
    ASSERT_EQ(book.best_bid_qty(), 20);

    auto bid_levels = book.get_bid_levels();
    ASSERT_EQ(bid_levels.size(), 3);
    ASSERT_EQ(bid_levels[0].price, 10020);
    ASSERT_EQ(bid_levels[1].price, 10000);
    ASSERT_EQ(bid_levels[2].price, 9980);

    // Add asks: 10080, 10050, 10100
    book.add_resting_order(Order{4, 10080, 15, 15, Side::Sell, OrderType::Limit, 4});
    book.add_resting_order(Order{5, 10050, 25, 25, Side::Sell, OrderType::Limit, 5});
    book.add_resting_order(Order{6, 10100, 35, 35, Side::Sell, OrderType::Limit, 6});

    // Best ask must be the lowest price (10050)
    ASSERT_EQ(*book.best_ask(), 10050);
    ASSERT_EQ(book.best_ask_qty(), 25);

    auto ask_levels = book.get_ask_levels();
    ASSERT_EQ(ask_levels.size(), 3);
    ASSERT_EQ(ask_levels[0].price, 10050);
    ASSERT_EQ(ask_levels[1].price, 10080);
    ASSERT_EQ(ask_levels[2].price, 10100);

    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(OrderBook_TimePriorityFIFO) {
    OrderBook book;
    std::string err;

    // Add multiple orders at the exact same price: 10000
    book.add_resting_order(Order{101, 10000, 10, 10, Side::Buy, OrderType::Limit, 10});
    book.add_resting_order(Order{102, 10000, 20, 20, Side::Buy, OrderType::Limit, 20});
    book.add_resting_order(Order{103, 10000, 30, 30, Side::Buy, OrderType::Limit, 30});

    ASSERT_EQ(book.bid_depth(), 1);
    ASSERT_EQ(book.best_bid_qty(), 60);

    // Match against incoming sell order of 15
    Order sell{201, 10000, 15, 15, Side::Sell, OrderType::Limit, 40};
    std::vector<Trade> trades;
    uint64_t trade_seq = 0;

    size_t count = book.match(sell, trades, trade_seq);
    ASSERT_EQ(count, 2);
    ASSERT_EQ(trades.size(), 2);

    // Trade 1 must consume Order 101 first (10 qty)
    ASSERT_EQ(trades[0].resting_order_id, 101);
    ASSERT_EQ(trades[0].quantity, 10);
    ASSERT_EQ(trades[0].price, 10000);

    // Trade 2 must consume Order 102 next (remaining 5 qty)
    ASSERT_EQ(trades[1].resting_order_id, 102);
    ASSERT_EQ(trades[1].quantity, 5);
    ASSERT_EQ(trades[1].price, 10000);

    // Order 101 should be completely filled and removed
    ASSERT_FALSE(book.has_order(101));

    // Order 102 should still exist with remaining qty 15
    ASSERT_TRUE(book.has_order(102));
    auto o102 = book.get_order(102);
    ASSERT_TRUE(o102.has_value());
    ASSERT_EQ(o102->remaining_qty, 15);

    // Order 103 should still exist with untouched qty 30
    ASSERT_TRUE(book.has_order(103));
    auto o103 = book.get_order(103);
    ASSERT_TRUE(o103.has_value());
    ASSERT_EQ(o103->remaining_qty, 30);

    ASSERT_EQ(book.best_bid_qty(), 45);
    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(OrderBook_Cancel) {
    OrderBook book;
    std::string err;

    book.add_resting_order(Order{1, 10000, 50, 50, Side::Buy, OrderType::Limit, 1});
    book.add_resting_order(Order{2, 10000, 30, 30, Side::Buy, OrderType::Limit, 2});
    ASSERT_EQ(book.best_bid_qty(), 80);

    // Cancel existing order 1
    ASSERT_EQ(book.cancel(1), OrderResult::Accepted);
    ASSERT_FALSE(book.has_order(1));
    ASSERT_EQ(book.best_bid_qty(), 30);

    // Cancel nonexistent order
    ASSERT_EQ(book.cancel(999), OrderResult::RejectedOrderNotFound);

    // Cancel order 2 -> price level becomes empty and must be pruned
    ASSERT_EQ(book.cancel(2), OrderResult::Accepted);
    ASSERT_FALSE(book.has_order(2));
    ASSERT_EQ(book.bid_depth(), 0);
    ASSERT_FALSE(book.best_bid().has_value());

    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(OrderBook_Modify) {
    OrderBook book;
    std::string err;
    std::vector<Trade> trades;
    uint64_t trade_seq = 0;

    book.add_resting_order(Order{1, 10000, 50, 50, Side::Buy, OrderType::Limit, 1});
    book.add_resting_order(Order{2, 10000, 50, 50, Side::Buy, OrderType::Limit, 2});

    // 1. Quantity reduction in-place (retains priority)
    ASSERT_EQ(book.modify(1, 10000, 30, trades, trade_seq), OrderResult::Accepted);
    auto o1 = book.get_order(1);
    ASSERT_TRUE(o1.has_value());
    ASSERT_EQ(o1->remaining_qty, 30);
    ASSERT_EQ(book.best_bid_qty(), 80);

    // 2. Quantity increase (loses priority, moved behind order 2)
    ASSERT_EQ(book.modify(1, 10000, 60, trades, trade_seq), OrderResult::Accepted);
    ASSERT_EQ(book.best_bid_qty(), 110);

    // A sell order of 50 should now consume order 2 first because order 1 lost priority
    Order sell{3, 10000, 50, 50, Side::Sell, OrderType::Limit, 3};
    book.match(sell, trades, trade_seq);
    ASSERT_EQ(trades.size(), 1);
    ASSERT_EQ(trades[0].resting_order_id, 2);
    ASSERT_EQ(trades[0].quantity, 50);

    // 3. Price change
    ASSERT_EQ(book.modify(1, 10010, 40, trades, trade_seq), OrderResult::Accepted);
    ASSERT_EQ(*book.best_bid(), 10010);
    ASSERT_EQ(book.best_bid_qty(), 40);

    ASSERT_TRUE(book.verify_invariants(&err));
}

TEST_CASE(OrderBook_InvariantsHoldOnCrossedCheck) {
    OrderBook book;
    std::string err;

    book.add_resting_order(Order{1, 10000, 50, 50, Side::Buy, OrderType::Limit, 1});
    book.add_resting_order(Order{2, 10050, 50, 50, Side::Sell, OrderType::Limit, 2});
    ASSERT_TRUE(book.verify_invariants(&err));

    // Non-existent order lookup check
    ASSERT_FALSE(book.has_order(999));
    ASSERT_FALSE(book.get_order(999).has_value());
}
