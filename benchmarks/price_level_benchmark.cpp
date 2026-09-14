#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/order_book.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <string>
#include <cstdlib>
#include <new>

// ============================================================================
// 1. Allocation Tracker
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

static thread_local bool g_track_allocations = false;
static thread_local AllocStats g_alloc_stats;

void* operator new(size_t size) {
    if (g_track_allocations) {
        ++g_alloc_stats.alloc_count;
        g_alloc_stats.bytes_allocated += size;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    if (g_track_allocations && p) {
        ++g_alloc_stats.dealloc_count;
    }
    std::free(p);
}

void operator delete(void* p, size_t) noexcept {
    if (g_track_allocations && p) {
        ++g_alloc_stats.dealloc_count;
    }
    std::free(p);
}

// ============================================================================
// 2. Linear Congruential PRNG
// ============================================================================

class FastRng {
public:
    explicit FastRng(uint64_t seed = 0x12345678ULL) : state_(seed) {}
    uint64_t next_u64() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1ULL;
        return state_ >> 32;
    }
    uint64_t next_range(uint64_t min_v, uint64_t max_v) noexcept {
        return min_v + (next_u64() % (max_v - min_v + 1));
    }
    void reset(uint64_t seed = 0x12345678ULL) noexcept { state_ = seed; }
private:
    uint64_t state_;
};

// ============================================================================
// 3. Step 2: Allocation Source Investigation
// ============================================================================

void run_allocation_investigation() {
    std::cout << "===================================================================================\n";
    std::cout << " STEP 2: ALLOCATION SOURCE BREAKDOWN EXPERIMENT\n";
    std::cout << " Investigating where the remaining ~1 allocation/op originates\n";
    std::cout << "===================================================================================\n";

    constexpr size_t N = 10000;

    // Test A: OrderPool alone
    {
        hft::OrderPool pool(N);
        g_alloc_stats.reset();
        g_track_allocations = true;
        std::vector<hft::OrderIndex> indices;
        indices.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            hft::Order o{static_cast<hft::OrderId>(i + 1), 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, 1};
            indices.push_back(pool.allocate(o));
        }
        for (auto idx : indices) {
            pool.deallocate(idx);
        }
        g_track_allocations = false;
        std::cout << "  1. OrderPool (" << N << " alloc+dealloc in pre-reserved pool):\n"
                  << "     Allocs: " << g_alloc_stats.alloc_count
                  << ", Deallocs: " << g_alloc_stats.dealloc_count
                  << " -> " << (static_cast<double>(g_alloc_stats.alloc_count) / N) << " allocs/op\n";
    }

    // Test B: std::unordered_map alone (with reserve)
    {
        struct OrderLoc { hft::Side side; hft::Price price; hft::OrderIndex idx; };
        std::unordered_map<hft::OrderId, OrderLoc> lookup;
        lookup.reserve(N);

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < N; ++i) {
            lookup[static_cast<hft::OrderId>(i + 1)] = OrderLoc{hft::Side::Buy, 10000, static_cast<hft::OrderIndex>(i)};
        }
        for (size_t i = 0; i < N; ++i) {
            lookup.erase(static_cast<hft::OrderId>(i + 1));
        }
        g_track_allocations = false;
        std::cout << "  2. std::unordered_map<OrderId, OrderLocation> (with reserve(" << N << ")):\n"
                  << "     Allocs: " << g_alloc_stats.alloc_count
                  << ", Deallocs: " << g_alloc_stats.dealloc_count
                  << " -> " << (static_cast<double>(g_alloc_stats.alloc_count) / N) << " allocs/op\n"
                  << "     NOTE: In MSVC STL, std::unordered_map allocates a _List_node on heap for every unique key!\n";
    }

    // Test C: std::map price levels alone
    {
        std::map<hft::Price, hft::PriceLevel, std::greater<hft::Price>> bid_map;

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < 1000; ++i) {
            bid_map[static_cast<hft::Price>(10000 - i)] = hft::PriceLevel{static_cast<hft::Price>(10000 - i), 100, 0, 0, 1};
        }
        for (size_t i = 0; i < 1000; ++i) {
            bid_map.erase(static_cast<hft::Price>(10000 - i));
        }
        g_track_allocations = false;
        std::cout << "  3. std::map<Price, PriceLevel> (1,000 distinct price levels insert + erase):\n"
                  << "     Allocs: " << g_alloc_stats.alloc_count
                  << ", Deallocs: " << g_alloc_stats.dealloc_count
                  << " -> " << (static_cast<double>(g_alloc_stats.alloc_count) / 1000) << " allocs/level\n"
                  << "     NOTE: Each new price level allocates an Rb-tree node (_Tree_node) on the heap.\n";
    }

    // Test D: OrderBook Add order at existing price vs new price
    {
        hft::OrderBook book(N);
        std::vector<hft::Trade> trades;
        uint64_t seq = 0;

        // Warm up with 1 order to create price level 10000
        book.process_order(hft::Order{1, 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);

        // Measure: Add 5,000 orders at EXISTING price 10000
        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < 5000; ++i) {
            book.process_order(hft::Order{static_cast<hft::OrderId>(i + 2), 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
        }
        g_track_allocations = false;
        std::cout << "  4. OrderBook add 5,000 orders at EXISTING price level:\n"
                  << "     Allocs: " << g_alloc_stats.alloc_count
                  << " -> " << (static_cast<double>(g_alloc_stats.alloc_count) / 5000) << " allocs/op (All from std::unordered_map lookup)\n";

        // Measure: Add 1,000 orders at NEW distinct price levels (9999 down to 9000)
        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < 1000; ++i) {
            book.process_order(hft::Order{static_cast<hft::OrderId>(i + 6000), static_cast<hft::Price>(9999 - i), 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
        }
        g_track_allocations = false;
        std::cout << "  5. OrderBook add 1,000 orders at NEW price levels:\n"
                  << "     Allocs: " << g_alloc_stats.alloc_count
                  << " -> " << (static_cast<double>(g_alloc_stats.alloc_count) / 1000) << " allocs/op (1 map node + 1 unordered_map node)\n";
    }
    std::cout << "===================================================================================\n\n";
}

// ============================================================================
// 4. Step 3 & 4: Price-Level Microbenchmark (std::map vs Flat Contiguous Vector)
// ============================================================================

// Flat contiguous sorted price-level vector representation
struct FlatBids {
    std::vector<hft::PriceLevel> levels;

    // Bids sorted descending: 100, 99, 98...
    // Compare functor for descending: higher price comes first
    static bool desc_cmp(const hft::PriceLevel& a, hft::Price b) noexcept {
        return a.price > b;
    }

    auto find(hft::Price p) noexcept {
        auto it = std::lower_bound(levels.begin(), levels.end(), p, desc_cmp);
        if (it != levels.end() && it->price == p) {
            return it;
        }
        return levels.end();
    }

    auto insert_or_get(hft::Price p) {
        auto it = std::lower_bound(levels.begin(), levels.end(), p, desc_cmp);
        if (it != levels.end() && it->price == p) {
            return it;
        }
        return levels.insert(it, hft::PriceLevel{p, 0, hft::INVALID_INDEX, hft::INVALID_INDEX, 0});
    }

    void erase_at(size_t idx) {
        if (idx < levels.size()) {
            levels.erase(levels.begin() + idx);
        }
    }

    hft::Price best_price() const noexcept {
        return levels.empty() ? 0 : levels.front().price;
    }
};

void run_price_level_microbenchmark() {
    std::cout << "===================================================================================\n";
    std::cout << " STEP 4: PRICE-LEVEL MICROBENCHMARK (std::map vs Contiguous Sorted Vector)\n";
    std::cout << " Testing: 10, 100, 1,000, 10,000, and 100,000 Price Levels\n";
    std::cout << "===================================================================================\n";

    const std::vector<size_t> level_counts = {10, 100, 1000, 10000, 100000};
    using Clock = std::chrono::steady_clock;

    std::cout << std::left
              << std::setw(10) << "Levels"
              << std::setw(24) << "Op"
              << std::setw(18) << "std::map (ns/op)"
              << std::setw(18) << "FlatVector (ns/op)"
              << "Speedup / Ratio\n";
    std::cout << "-----------------------------------------------------------------------------------\n";

    for (size_t num_levels : level_counts) {
        // Setup deterministic prices
        std::vector<hft::Price> prices;
        prices.reserve(num_levels);
        for (size_t i = 0; i < num_levels; ++i) {
            prices.push_back(static_cast<hft::Price>(200000 - i * 2));
        }

        // 1. Setup instances
        std::map<hft::Price, hft::PriceLevel, std::greater<hft::Price>> map_bids;
        FlatBids flat_bids;
        flat_bids.levels.reserve(num_levels + 10);

        for (auto p : prices) {
            map_bids[p] = hft::PriceLevel{p, 100, 0, 0, 1};
            flat_bids.levels.push_back(hft::PriceLevel{p, 100, 0, 0, 1});
        }

        // Test A: Best Price Access (1,000,000 accesses)
        {
            constexpr size_t iterations = 1000000;
            volatile hft::Price sink = 0;

            auto t0 = Clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                sink = map_bids.begin()->first;
            }
            auto t1 = Clock::now();
            double map_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iterations;

            auto t2 = Clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                sink = flat_bids.best_price();
            }
            auto t3 = Clock::now();
            double flat_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / iterations;

            std::cout << std::left
                      << std::setw(10) << num_levels
                      << std::setw(24) << "Best Price Access"
                      << std::setw(18) << std::fixed << std::setprecision(2) << map_ns
                      << std::setw(18) << flat_ns
                      << std::setprecision(2) << (map_ns / flat_ns) << "x\n";
        }

        // Test B: Lookup Existing Price (100,000 random lookups)
        {
            const size_t lookups = (num_levels <= 10000) ? 500000 : 100000;
            FastRng rng(42);
            std::vector<hft::Price> query_prices;
            query_prices.reserve(lookups);
            for (size_t i = 0; i < lookups; ++i) {
                size_t idx = rng.next_range(0, num_levels - 1);
                query_prices.push_back(prices[idx]);
            }

            volatile uint64_t sink = 0;
            auto t0 = Clock::now();
            for (size_t i = 0; i < lookups; ++i) {
                auto it = map_bids.find(query_prices[i]);
                sink += it->second.total_quantity;
            }
            auto t1 = Clock::now();
            double map_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / lookups;

            auto t2 = Clock::now();
            for (size_t i = 0; i < lookups; ++i) {
                auto it = flat_bids.find(query_prices[i]);
                sink += it->total_quantity;
            }
            auto t3 = Clock::now();
            double flat_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / lookups;

            std::cout << std::left
                      << std::setw(10) << num_levels
                      << std::setw(24) << "Lookup Existing Price"
                      << std::setw(18) << std::fixed << std::setprecision(2) << map_ns
                      << std::setw(18) << flat_ns
                      << std::setprecision(2) << (map_ns / flat_ns) << "x\n";
        }

        // Test C: Sequential Traversal of Top 10 Price Levels (Sweeping order execution)
        {
            if (num_levels >= 10) {
                constexpr size_t sweeps = 200000;
                volatile uint64_t sink = 0;

                auto t0 = Clock::now();
                for (size_t i = 0; i < sweeps; ++i) {
                    auto it = map_bids.begin();
                    for (size_t k = 0; k < 10; ++k) {
                        sink += it->second.total_quantity;
                        ++it;
                    }
                }
                auto t1 = Clock::now();
                double map_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / sweeps;

                auto t2 = Clock::now();
                for (size_t i = 0; i < sweeps; ++i) {
                    for (size_t k = 0; k < 10; ++k) {
                        sink += flat_bids.levels[k].total_quantity;
                    }
                }
                auto t3 = Clock::now();
                double flat_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / sweeps;

                std::cout << std::left
                          << std::setw(10) << num_levels
                          << std::setw(24) << "Top 10 Level Sweep"
                          << std::setw(18) << std::fixed << std::setprecision(2) << map_ns
                          << std::setw(18) << flat_ns
                          << std::setprecision(2) << (map_ns / flat_ns) << "x\n";
            }
        }

        // Test D: Insert and Erase Near Top of Book (10,000 ops)
        // In trading, orders frequently insert and cancel within top 5-20 levels
        {
            const size_t insert_ops = (num_levels <= 10000) ? 50000 : 5000;
            hft::Price test_price = prices[1] + 1; // Between prices[0] and prices[1]

            auto t0 = Clock::now();
            for (size_t i = 0; i < insert_ops; ++i) {
                map_bids[test_price] = hft::PriceLevel{test_price, 50, 0, 0, 1};
                map_bids.erase(test_price);
            }
            auto t1 = Clock::now();
            double map_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / insert_ops;

            auto t2 = Clock::now();
            for (size_t i = 0; i < insert_ops; ++i) {
                flat_bids.insert_or_get(test_price);
                flat_bids.erase_at(1);
            }
            auto t3 = Clock::now();
            double flat_ns = std::chrono::duration<double, std::nano>(t3 - t2).count() / insert_ops;

            std::cout << std::left
                      << std::setw(10) << num_levels
                      << std::setw(24) << "Top-Level Insert+Erase"
                      << std::setw(18) << std::fixed << std::setprecision(2) << map_ns
                      << std::setw(18) << flat_ns
                      << std::setprecision(2) << (map_ns / flat_ns) << "x\n";
        }
        std::cout << "-----------------------------------------------------------------------------------\n";
    }
    std::cout << "===================================================================================\n\n";
}

// ============================================================================
// 5. Step 3: Granular Price-Level Access Profiling
// ============================================================================

void run_granular_access_profiling() {
    std::cout << "===================================================================================\n";
    std::cout << " STEP 3: GRANULAR PRICE-LEVEL ACCESS PROFILING (Current std::map OrderBook)\n";
    std::cout << " Measuring isolated operations: Existing vs New Price, Cancel, Sweep\n";
    std::cout << "===================================================================================\n";

    using Clock = std::chrono::steady_clock;
    constexpr size_t iters = 100000;

    // A. Add Order at Existing Price
    {
        hft::OrderBook book(iters + 100);
        std::vector<hft::Trade> trades;
        uint64_t seq = 0;
        // Warm up price level
        book.process_order(hft::Order{1, 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);

        auto t0 = Clock::now();
        for (size_t i = 0; i < iters; ++i) {
            book.process_order(hft::Order{static_cast<hft::OrderId>(i + 2), 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
        std::cout << "  A. Add order at EXISTING price:       " << std::fixed << std::setprecision(1) << ns << " ns/op\n";
    }

    // B. Add Order at NEW Price
    {
        hft::OrderBook book(iters + 100);
        std::vector<hft::Trade> trades;
        uint64_t seq = 0;

        auto t0 = Clock::now();
        for (size_t i = 0; i < iters; ++i) {
            book.process_order(hft::Order{static_cast<hft::OrderId>(i + 1), static_cast<hft::Price>(500000 - i), 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
        std::cout << "  B. Add order at NEW price:            " << std::fixed << std::setprecision(1) << ns << " ns/op\n";
    }

    // C. Cancel Order from Existing Price (Level remains populated)
    {
        hft::OrderBook book(iters + 100);
        std::vector<hft::Trade> trades;
        uint64_t seq = 0;
        for (size_t i = 0; i < iters + 10; ++i) {
            book.process_order(hft::Order{static_cast<hft::OrderId>(i + 1), 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
        }

        auto t0 = Clock::now();
        for (size_t i = 0; i < iters; ++i) {
            book.cancel(static_cast<hft::OrderId>(i + 1));
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
        std::cout << "  C. Cancel order (existing level retained): " << std::fixed << std::setprecision(1) << ns << " ns/op\n";
    }

    // D. Remove an Empty Price Level (Last order cancelled erases level from map)
    {
        hft::OrderBook book(iters + 100);
        std::vector<hft::Trade> trades;
        uint64_t seq = 0;
        for (size_t i = 0; i < iters; ++i) {
            book.process_order(hft::Order{static_cast<hft::OrderId>(i + 1), static_cast<hft::Price>(500000 - i), 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
        }

        auto t0 = Clock::now();
        for (size_t i = 0; i < iters; ++i) {
            book.cancel(static_cast<hft::OrderId>(i + 1));
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
        std::cout << "  D. Remove empty price level (erased):     " << std::fixed << std::setprecision(1) << ns << " ns/op\n";
    }

    // E. Match Against Best Price (Single price level, 100,000 matches)
    {
        hft::OrderBook book(iters * 2 + 100);
        std::vector<hft::Trade> trades;
        trades.reserve(16);
        uint64_t seq = 0;

        auto t0 = Clock::now();
        for (size_t i = 0; i < iters; ++i) {
            // Passive ask
            book.process_order(hft::Order{static_cast<hft::OrderId>(i * 2 + 1), 10000, 10, 10, hft::Side::Sell, hft::OrderType::Limit, ++seq}, trades, seq);
            trades.clear();
            // Aggressive buy matching it
            book.process_order(hft::Order{static_cast<hft::OrderId>(i * 2 + 2), 10000, 10, 10, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
            trades.clear();
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (iters * 2);
        std::cout << "  E. Match against best price (pair):       " << std::fixed << std::setprecision(1) << ns << " ns/op\n";
    }

    // F. Walk Through Multiple Price Levels During a Sweep (Sweeps 5 price levels)
    {
        constexpr size_t sweep_cycles = 10000;
        hft::OrderBook book(sweep_cycles * 10);
        std::vector<hft::Trade> trades;
        trades.reserve(16);
        uint64_t seq = 0;

        auto t0 = Clock::now();
        for (size_t c = 0; c < sweep_cycles; ++c) {
            // Setup 5 ask levels
            for (size_t l = 0; l < 5; ++l) {
                hft::OrderId id = static_cast<hft::OrderId>(c * 10 + l + 1);
                book.process_order(hft::Order{id, static_cast<hft::Price>(10000 + l), 10, 10, hft::Side::Sell, hft::OrderType::Limit, ++seq}, trades, seq);
                trades.clear();
            }
            // Aggressive buy sweeping all 5 levels
            hft::OrderId sweep_id = static_cast<hft::OrderId>(c * 10 + 6);
            book.process_order(hft::Order{sweep_id, 10010, 50, 50, hft::Side::Buy, hft::OrderType::Limit, ++seq}, trades, seq);
            trades.clear();
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (sweep_cycles * 6);
        std::cout << "  F. 5-Level Sweep execution:               " << std::fixed << std::setprecision(1) << ns << " ns/op\n";
    }
    std::cout << "===================================================================================\n\n";
}

int main() {
    run_allocation_investigation();
    run_granular_access_profiling();
    run_price_level_microbenchmark();
    return 0;
}
