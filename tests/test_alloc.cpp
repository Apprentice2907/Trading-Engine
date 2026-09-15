// test_alloc.cpp — Zero-allocation regression tests for HFT hot paths.
//
// These tests verify that the hot paths in the matching engine, pre-trade risk
// engine, SPSC queue, and order gateway perform zero dynamic heap allocations
// after the warmup phase. Any regression that introduces heap allocation in a
// hot path will cause these tests to fail.
//
// Methodology:
//   1. Warm up the component (prime allocator freelists, fill lookup tables).
//   2. Arm the allocation tracker.
//   3. Run the hot-path operations.
//   4. Disarm and assert alloc_count == 0.
//
// The operator new/delete overrides here are LOCAL to this translation unit's
// linkage because test_alloc.cpp is compiled into the hft_tests binary (not
// into hft_core). No other TU in hft_tests defines these operators, so there
// is no ODR conflict.

#include "test_framework.hpp"
#include "hft/matching_engine.hpp"
#include "hft/flat_order_book.hpp"
#include "hft/trading_pipeline.hpp"
#include "hft/market_data.hpp"
#include "hft/spsc_queue.hpp"
#include "hft/order.hpp"

#include <cstdlib>
#include <new>
#include <cstdint>

// ============================================================================
// Allocation tracker (TU-local; no ODR conflict because hft_core does not
// define operator new/delete)
// ============================================================================

struct AllocStats {
    uint64_t alloc_count{0};
    uint64_t dealloc_count{0};
    uint64_t bytes_allocated{0};

    void reset() noexcept {
        alloc_count = 0;
        dealloc_count = 0;
        bytes_allocated = 0;
    }
};

static thread_local bool       g_track_alloc = false;
static thread_local AllocStats g_alloc_stats;

void* operator new(size_t size) {
    if (g_track_alloc) {
        ++g_alloc_stats.alloc_count;
        g_alloc_stats.bytes_allocated += size;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    if (g_track_alloc && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

void operator delete(void* p, size_t) noexcept {
    if (g_track_alloc && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

void* operator new[](size_t size) {
    if (g_track_alloc) {
        ++g_alloc_stats.alloc_count;
        g_alloc_stats.bytes_allocated += size;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p) noexcept {
    if (g_track_alloc && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
    if (g_track_alloc && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

// ============================================================================
// Helper: RAII guard to arm/disarm allocation tracking
// ============================================================================

struct AllocGuard {
    explicit AllocGuard() {
        g_alloc_stats.reset();
        g_track_alloc = true;
    }
    ~AllocGuard() {
        g_track_alloc = false;
    }
    [[nodiscard]] uint64_t alloc_count() const noexcept {
        return g_alloc_stats.alloc_count;
    }
};

// ============================================================================
// Tests
// ============================================================================

// MatchingEngine: aggressive orders that execute and match against resting orders
// in the book must perform zero heap allocations on the hot path.
TEST_CASE(ZeroAlloc_MatchingEngine_AggressiveMatch) {
    constexpr size_t SEED_COUNT = 10000;
    constexpr size_t WARMUP     = 200;
    constexpr size_t HOT        = 5000;

    hft::MatchingEngine engine;
    engine.reserve(SEED_COUNT + WARMUP + HOT + 100);
    std::vector<hft::Trade> trades;
    trades.reserve(64);

    // Seed resting sell orders
    for (size_t i = 1; i <= SEED_COUNT; ++i) {
        engine.submit_limit_order(i, hft::Side::Sell, 10000, 10, trades);
        trades.clear();
    }

    // Warmup: aggressive buys that fill completely
    for (size_t i = 1; i <= WARMUP; ++i) {
        engine.submit_limit_order(SEED_COUNT + i, hft::Side::Buy, 10000, 10, trades);
        trades.clear();
    }

    // Hot path: arm tracker, execute crossing orders, assert zero allocations
    {
        AllocGuard guard;
        for (size_t i = 1; i <= HOT; ++i) {
            engine.submit_limit_order(SEED_COUNT + WARMUP + i, hft::Side::Buy, 10000, 10, trades);
            trades.clear();
        }
        ASSERT_EQ(guard.alloc_count(), uint64_t{0});
    }
}

// MatchingEngine: cancel path must not allocate
TEST_CASE(ZeroAlloc_MatchingEngine_CancelPath) {
    constexpr size_t N = 50000;

    hft::MatchingEngine engine;
    engine.reserve(N + 100);
    std::vector<hft::Trade> trades;
    trades.reserve(32);

    // Pre-seed N resting orders (not timed)
    for (size_t i = 1; i <= N; ++i) {
        engine.submit_limit_order(i, hft::Side::Buy, 9000, 10, trades);
        trades.clear();
    }

    {
        AllocGuard guard;
        for (size_t i = 1; i <= N; ++i) {
            engine.cancel_order(i);
        }
        ASSERT_EQ(guard.alloc_count(), uint64_t{0});
    }
}

// FlatMatchingEngine: aggressive orders that execute and match against resting orders
// must perform zero heap allocations.
TEST_CASE(ZeroAlloc_FlatMatchingEngine_AggressiveMatch) {
    constexpr size_t SEED_COUNT = 10000;
    constexpr size_t WARMUP     = 200;
    constexpr size_t HOT        = 5000;

    hft::FlatMatchingEngine engine;
    engine.reserve(SEED_COUNT + WARMUP + HOT + 100);
    std::vector<hft::Trade> trades;
    trades.reserve(64);

    // Seed resting sell orders
    for (size_t i = 1; i <= SEED_COUNT; ++i) {
        engine.submit_limit_order(i, hft::Side::Sell, 10000, 10, trades);
        trades.clear();
    }

    // Warmup
    for (size_t i = 1; i <= WARMUP; ++i) {
        engine.submit_limit_order(SEED_COUNT + i, hft::Side::Buy, 10000, 10, trades);
        trades.clear();
    }

    {
        AllocGuard guard;
        for (size_t i = 1; i <= HOT; ++i) {
            engine.submit_limit_order(SEED_COUNT + WARMUP + i, hft::Side::Buy, 10000, 10, trades);
            trades.clear();
        }
        ASSERT_EQ(guard.alloc_count(), uint64_t{0});
    }
}

// FlatMatchingEngine: cancel path must not allocate
TEST_CASE(ZeroAlloc_FlatMatchingEngine_CancelPath) {
    constexpr size_t N = 50000;

    hft::FlatMatchingEngine engine;
    engine.reserve(N + 100);
    std::vector<hft::Trade> trades;
    trades.reserve(32);

    for (size_t i = 1; i <= N; ++i) {
        engine.submit_limit_order(i, hft::Side::Buy, 9000, 10, trades);
        trades.clear();
    }

    {
        AllocGuard guard;
        for (size_t i = 1; i <= N; ++i) {
            engine.cancel_order(i);
        }
        ASSERT_EQ(guard.alloc_count(), uint64_t{0});
    }
}

// Resting order allocation bounds:
// OrderPool pre-allocates contiguous memory (0 dynamic allocations).
// In standard C++, std::unordered_map allocates exactly 1 node per new key on insert.
// This test verifies that resting order inserts do not exceed 1 allocation per order
// (confirming OrderPool and level vectors do not dynamically reallocate).
TEST_CASE(RestingOrderAllocations_BoundedToOnePerOrder) {
    constexpr size_t N = 10000;

    hft::FlatMatchingEngine engine;
    engine.reserve(N + 100, 1024);
    std::vector<hft::Trade> trades;
    trades.reserve(32);

    AllocGuard guard;
    for (size_t i = 1; i <= N; ++i) {
        engine.submit_limit_order(i, hft::Side::Buy, 9000, 10, trades);
        trades.clear();
    }
    // At most 1 allocation per order for the std::unordered_map node
    ASSERT_LE(guard.alloc_count(), static_cast<uint64_t>(N));
}

// PreTradeRiskEngine: all risk checks must be allocation-free
TEST_CASE(ZeroAlloc_PreTradeRiskEngine) {
    constexpr size_t WARMUP = 200;
    constexpr size_t HOT    = 50000;

    hft::RiskConfig cfg{};
    cfg.max_order_quantity   = 100000;
    cfg.max_order_notional   = 500000000000ULL;
    cfg.max_exposure_quantity = 10000000;
    cfg.allowed_instrument_id = 3045;
    hft::PreTradeRiskEngine risk(cfg);

    hft::OrderCommand cmd = hft::OrderCommand::make_add(1, 3045, 1, hft::Side::Buy, 83000, 10);

    // Warmup
    for (size_t i = 0; i < WARMUP; ++i) {
        risk.check_order(cmd);
    }

    {
        AllocGuard guard;
        for (size_t i = 0; i < HOT; ++i) {
            risk.check_order(cmd);
        }
        ASSERT_EQ(guard.alloc_count(), uint64_t{0});
    }
}

// SpscQueue: push + pop on a pre-allocated queue must produce zero allocations
TEST_CASE(ZeroAlloc_SpscQueue_PushPop) {
    constexpr size_t WARMUP = 200;
    constexpr size_t HOT    = 100000;

    hft::SpscQueue<hft::OrderEvent, 4096, true> queue;
    hft::OrderEvent ev = hft::OrderEvent::make_add(1, hft::Side::Buy, 10000, 10);
    hft::OrderEvent out{};

    // Warmup
    for (size_t i = 0; i < WARMUP; ++i) {
        queue.try_push(ev);
        queue.try_pop(out);
    }

    {
        AllocGuard guard;
        for (size_t i = 0; i < HOT; ++i) {
            queue.try_push(ev);
            queue.try_pop(out);
        }
        ASSERT_EQ(guard.alloc_count(), uint64_t{0});
    }
}

// Layout verification: confirm that the compile-time size/alignment assertions
// match the expected values at test runtime. These are compile-time already, but
// showing them in the test output verifies them at test execution time.
TEST_CASE(LayoutAssertions_HotPathStructs) {
    ASSERT_EQ(sizeof(hft::OrderEvent),       size_t{32});
    ASSERT_EQ(sizeof(hft::OrderCommand),     size_t{64});
    ASSERT_EQ(sizeof(hft::ExecutionReport),  size_t{64});

    // Both are cache-aligned
    ASSERT_EQ(alignof(hft::OrderCommand),    size_t{64});
    ASSERT_EQ(alignof(hft::ExecutionReport), size_t{64});

    // MarketEvent is 2 cache lines, first cache-line aligned
    ASSERT_EQ(sizeof(hft::MarketEvent),      size_t{128});
    ASSERT_EQ(alignof(hft::MarketEvent),     size_t{64});
}
