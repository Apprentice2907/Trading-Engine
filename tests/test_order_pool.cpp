#include "test_framework.hpp"
#include "hft/order_pool.hpp"

using namespace hft;

TEST_CASE(OrderPool_AllocationAndRecycling) {
    OrderPool pool(4);
    ASSERT_EQ(pool.capacity(), 4);
    ASSERT_EQ(pool.size(), 0);
    ASSERT_TRUE(pool.empty());

    Order o1{1, 10000, 10, 10, Side::Buy, OrderType::Limit, 1};
    Order o2{2, 10010, 20, 20, Side::Sell, OrderType::Limit, 2};
    Order o3{3, 10020, 30, 30, Side::Buy, OrderType::Limit, 3};

    OrderIndex idx1 = pool.allocate(o1);
    OrderIndex idx2 = pool.allocate(o2);
    OrderIndex idx3 = pool.allocate(o3);

    ASSERT_EQ(pool.size(), 3);
    ASSERT_NE(idx1, idx2);
    ASSERT_NE(idx2, idx3);

    ASSERT_EQ(pool.order(idx1).id, 1);
    ASSERT_EQ(pool.order(idx2).id, 2);
    ASSERT_EQ(pool.order(idx3).id, 3);

    // Deallocate middle order
    pool.deallocate(idx2);
    ASSERT_EQ(pool.size(), 2);

    // Next allocate should recycle idx2 (O(1) LIFO free list)
    Order o4{4, 10030, 40, 40, Side::Buy, OrderType::Limit, 4};
    OrderIndex idx4 = pool.allocate(o4);
    ASSERT_EQ(idx4, idx2);
    ASSERT_EQ(pool.order(idx4).id, 4);
    ASSERT_EQ(pool.size(), 3);
}

TEST_CASE(OrderPool_AutomaticExpansion) {
    OrderPool pool(2);
    ASSERT_EQ(pool.capacity(), 2);

    OrderIndex i1 = pool.allocate(Order{1, 100, 10, 10, Side::Buy, OrderType::Limit, 1});
    OrderIndex i2 = pool.allocate(Order{2, 200, 20, 20, Side::Buy, OrderType::Limit, 2});
    ASSERT_EQ(pool.size(), 2);

    // 3rd allocate triggers expansion
    OrderIndex i3 = pool.allocate(Order{3, 300, 30, 30, Side::Buy, OrderType::Limit, 3});
    ASSERT_EQ(pool.size(), 3);
    ASSERT_GE(pool.capacity(), 4);

    // Previous orders must still be intact
    ASSERT_EQ(pool.order(i1).id, 1);
    ASSERT_EQ(pool.order(i2).id, 2);
    ASSERT_EQ(pool.order(i3).id, 3);
}

TEST_CASE(OrderPool_ClearAndReuse) {
    OrderPool pool(8);
    for (uint64_t i = 1; i <= 5; ++i) {
        pool.allocate(Order{i, 1000, 10, 10, Side::Buy, OrderType::Limit, i});
    }
    ASSERT_EQ(pool.size(), 5);

    pool.clear();
    ASSERT_EQ(pool.size(), 0);
    ASSERT_TRUE(pool.empty());

    // Should be able to allocate again without growth
    OrderIndex ni = pool.allocate(Order{100, 5000, 50, 50, Side::Sell, OrderType::Limit, 10});
    ASSERT_EQ(pool.size(), 1);
    ASSERT_EQ(pool.order(ni).id, 100);
}
