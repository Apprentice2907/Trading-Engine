// benchmark.cpp — Matching engine throughput and latency benchmark.
//
// Methodology:
//   Throughput  : Uninstrumented bulk loop (no clock calls inside hot path).
//                 Reported as: total_ops / wall_clock_seconds.
//   Latency     : Per-operation TSC measurements collected into a raw sample
//                 array. Mean, p50, p95, p99, and max are ALL computed from
//                 the same sorted sample array — preventing the mean < p50
//                 inconsistency that arises when throughput-derived averages
//                 are mixed with separately measured percentiles.
//
// Timer: TSC via __rdtsc() + _mm_lfence() with empirical frequency calibration
//        (50 ms sleep against steady_clock). Falls back to steady_clock on
//        non-x86 platforms.

#include "hft/matching_engine.hpp"
#include "hft/flat_order_book.hpp"
#include "bench_timer.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <new>

// ============================================================================
// 1. Allocation Tracking Harness
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

void* operator new[](size_t size) {
    if (g_track_allocations) {
        ++g_alloc_stats.alloc_count;
        g_alloc_stats.bytes_allocated += size;
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p) noexcept {
    if (g_track_allocations && p) {
        ++g_alloc_stats.dealloc_count;
    }
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
    if (g_track_allocations && p) {
        ++g_alloc_stats.dealloc_count;
    }
    std::free(p);
}

// ============================================================================
// 2. Deterministic Workload Generator
// ============================================================================

enum class OpType : uint8_t {
    SubmitLimit,
    Cancel,
    Modify
};

struct BenchmarkOp {
    OpType type{OpType::SubmitLimit};
    hft::OrderId id{0};
    hft::Side side{hft::Side::Buy};
    hft::Price price{0};
    hft::Quantity qty{0};
};

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(uint64_t seed = 0x12345678ULL)
        : state_(seed), initial_seed_(seed) {}

    uint64_t next_u64() noexcept {
        state_ = state_ * 6364136223846793005ULL + 1ULL;
        return state_ >> 32;
    }

    uint64_t next_range(uint64_t min_val, uint64_t max_val) noexcept {
        return min_val + (next_u64() % (max_val - min_val + 1));
    }

    [[nodiscard]] uint64_t seed() const noexcept { return initial_seed_; }

    void reset(uint64_t seed = 0x12345678ULL) noexcept {
        state_ = seed;
        initial_seed_ = seed;
    }

    // Workload A: Add-Heavy (100% resting limit orders, uncrossed)
    std::vector<BenchmarkOp> generate_add_heavy(size_t count) {
        std::vector<BenchmarkOp> ops;
        ops.reserve(count);
        hft::OrderId next_id = 1;

        for (size_t i = 0; i < count; ++i) {
            hft::OrderId id = next_id++;
            hft::Side side = (next_u64() % 2 == 0) ? hft::Side::Buy : hft::Side::Sell;
            hft::Price price = (side == hft::Side::Buy)
                ? static_cast<hft::Price>(next_range(9000, 9999))
                : static_cast<hft::Price>(next_range(10001, 11000));
            hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 100));

            ops.push_back(BenchmarkOp{OpType::SubmitLimit, id, side, price, qty});
        }
        return ops;
    }

    // Workload B: Match-Heavy (Interleaved resting and aggressive crossing orders)
    std::vector<BenchmarkOp> generate_match_heavy(size_t count) {
        std::vector<BenchmarkOp> ops;
        ops.reserve(count);
        hft::OrderId next_id = 1;

        for (size_t i = 0; i < count; ++i) {
            hft::OrderId id = next_id++;
            if (i % 2 == 0) {
                hft::Price price = static_cast<hft::Price>(next_range(10000, 10020));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 50));
                ops.push_back(BenchmarkOp{OpType::SubmitLimit, id, hft::Side::Sell, price, qty});
            } else {
                hft::Price price = static_cast<hft::Price>(next_range(10020, 10030));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 60));
                ops.push_back(BenchmarkOp{OpType::SubmitLimit, id, hft::Side::Buy, price, qty});
            }
        }
        return ops;
    }

    // Workload C: Cancel-Heavy (50% passive limit orders, 50% cancellations)
    std::vector<BenchmarkOp> generate_cancel_heavy(size_t count) {
        std::vector<BenchmarkOp> ops;
        ops.reserve(count);
        std::vector<hft::OrderId> live_ids;
        live_ids.reserve(count / 2);
        hft::OrderId next_id = 1;

        for (size_t i = 0; i < count; ++i) {
            if (live_ids.empty() || (next_u64() % 100 < 50)) {
                hft::OrderId id = next_id++;
                hft::Side side = (next_u64() % 2 == 0) ? hft::Side::Buy : hft::Side::Sell;
                hft::Price price = (side == hft::Side::Buy)
                    ? static_cast<hft::Price>(next_range(9000, 9990))
                    : static_cast<hft::Price>(next_range(10010, 11000));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 100));

                ops.push_back(BenchmarkOp{OpType::SubmitLimit, id, side, price, qty});
                live_ids.push_back(id);
            } else {
                size_t idx = static_cast<size_t>(next_range(0, live_ids.size() - 1));
                hft::OrderId id = live_ids[idx];
                live_ids[idx] = live_ids.back();
                live_ids.pop_back();

                ops.push_back(BenchmarkOp{OpType::Cancel, id, hft::Side::Buy, 0, 0});
            }
        }
        return ops;
    }

    // Workload D: Mixed (Realistic distribution: 60% Add, 20% Cancel, 20% Modify)
    std::vector<BenchmarkOp> generate_mixed(size_t count) {
        std::vector<BenchmarkOp> ops;
        ops.reserve(count);
        std::vector<hft::OrderId> active_ids;
        active_ids.reserve(count / 2);
        hft::OrderId next_id = 1;

        for (size_t i = 0; i < count; ++i) {
            const uint64_t roll = next_range(1, 100);

            if (roll <= 60 || active_ids.empty()) {
                hft::OrderId id = next_id++;
                hft::Side side = (next_u64() % 2 == 0) ? hft::Side::Buy : hft::Side::Sell;
                hft::Price price = static_cast<hft::Price>(next_range(9950, 10050));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(5, 100));

                ops.push_back(BenchmarkOp{OpType::SubmitLimit, id, side, price, qty});
                active_ids.push_back(id);
            } else if (roll <= 80) {
                size_t idx = static_cast<size_t>(next_range(0, active_ids.size() - 1));
                hft::OrderId id = active_ids[idx];
                active_ids[idx] = active_ids.back();
                active_ids.pop_back();

                ops.push_back(BenchmarkOp{OpType::Cancel, id, hft::Side::Buy, 0, 0});
            } else {
                size_t idx = static_cast<size_t>(next_range(0, active_ids.size() - 1));
                hft::OrderId id = active_ids[idx];
                hft::Price new_price = static_cast<hft::Price>(next_range(9970, 10030));
                hft::Quantity new_qty = static_cast<hft::Quantity>(next_range(10, 80));

                ops.push_back(BenchmarkOp{OpType::Modify, id, hft::Side::Buy, new_price, new_qty});
            }
        }
        return ops;
    }

private:
    uint64_t state_;
    uint64_t initial_seed_;
};

// ============================================================================
// 3. Benchmark Execution
// ============================================================================

struct BenchmarkResult {
    std::string engine_type;
    std::string workload_name;
    size_t  operation_count{0};
    double  throughput_mops{0.0};  // From uninstrumented bulk loop
    double  mean_ns{0.0};          // From per-op TSC sample array
    double  p50_ns{0.0};           // From same per-op TSC sample array
    double  p95_ns{0.0};
    double  p99_ns{0.0};
    double  p999_ns{0.0};
    double  max_ns{0.0};
    uint64_t total_trades{0};
    double  allocs_per_op{0.0};
};

// Execute a single workload operation on the engine.
template <typename EngineT>
inline void run_op(EngineT& engine, const BenchmarkOp& op, std::vector<hft::Trade>& trades) {
    switch (op.type) {
        case OpType::SubmitLimit:
            engine.submit_limit_order(op.id, op.side, op.price, op.qty, trades);
            trades.clear();
            break;
        case OpType::Cancel:
            engine.cancel_order(op.id);
            break;
        case OpType::Modify:
            engine.modify_order(op.id, op.price, op.qty, trades);
            trades.clear();
            break;
    }
}

class BenchmarkRunner {
public:
    template <typename EngineT>
    static BenchmarkResult run(BenchTimer& timer,
                               const std::string& engine_type,
                               const std::string& name,
                               const std::vector<BenchmarkOp>& ops,
                               bool pre_reserve = false) {
        const size_t count = ops.size();
        const size_t warmup_count = std::min(count / 10, size_t{5000});
        const size_t latency_samples = std::min(count, size_t{200000});

        // ------------------------------------------------------------------
        // 1. Allocation check: warmup then track.
        // ------------------------------------------------------------------
        AllocStats alloc_res;
        {
            EngineT engine;
            if constexpr (std::is_same_v<EngineT, hft::MatchingEngine> ||
                          std::is_same_v<EngineT, hft::FlatMatchingEngine>) {
                if (pre_reserve) engine.reserve(count);
            }
            std::vector<hft::Trade> trades;
            trades.reserve(128);

            // Warmup (excluded from alloc tracking)
            for (size_t i = 0; i < warmup_count; ++i) {
                run_op(engine, ops[i % count], trades);
            }

            const size_t alloc_sample_size = std::min(count - warmup_count, size_t{50000});
            g_alloc_stats.reset();
            g_track_allocations = true;
            for (size_t i = warmup_count; i < warmup_count + alloc_sample_size; ++i) {
                run_op(engine, ops[i], trades);
            }
            g_track_allocations = false;
            alloc_res = g_alloc_stats;
        }

        // ------------------------------------------------------------------
        // 2. Throughput: uninstrumented bulk loop, no clock calls inside.
        //    Reported as total_ops / wall_clock_seconds. This is throughput —
        //    NOT the same as per-operation latency.
        // ------------------------------------------------------------------
        double best_elapsed_sec = 1e9;
        uint64_t final_trades = 0;
        const int tput_iters = (count >= 1000000) ? 1 : 3;

        for (int iter = 0; iter < tput_iters; ++iter) {
            EngineT engine;
            if constexpr (std::is_same_v<EngineT, hft::MatchingEngine> ||
                          std::is_same_v<EngineT, hft::FlatMatchingEngine>) {
                if (pre_reserve) engine.reserve(count);
            }
            std::vector<hft::Trade> trades;
            trades.reserve(256);

            auto t_start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < count; ++i) {
                run_op(engine, ops[i], trades);
            }
            auto t_end = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(t_end - t_start).count();
            if (elapsed < best_elapsed_sec) {
                best_elapsed_sec = elapsed;
                final_trades = engine.total_trades_generated();
            }
        }

        // ------------------------------------------------------------------
        // 3. Per-operation latency: TSC-timed loop.
        //    Warmup first to prime instruction cache and branch predictors.
        //    Mean and all percentiles computed from the SAME sample array.
        // ------------------------------------------------------------------
        LatencySampler sampler(latency_samples);

        {
            EngineT engine;
            if constexpr (std::is_same_v<EngineT, hft::MatchingEngine> ||
                          std::is_same_v<EngineT, hft::FlatMatchingEngine>) {
                if (pre_reserve) engine.reserve(count);
            }
            std::vector<hft::Trade> trades;
            trades.reserve(128);

            // Warmup: prime icache and branch predictors before recording samples.
            for (size_t i = 0; i < warmup_count; ++i) {
                run_op(engine, ops[i % count], trades);
            }

            // Instrumented latency loop.
            for (size_t i = 0; i < latency_samples; ++i) {
                auto t0 = timer.start();
                run_op(engine, ops[i], trades);
                sampler.record(timer.stop_ns(t0));
            }
        }

        sampler.finish();

        const double alloc_sample_n = static_cast<double>(std::min(count - warmup_count, size_t{50000}));

        BenchmarkResult res;
        res.engine_type    = engine_type;
        res.workload_name  = name;
        res.operation_count = count;
        res.throughput_mops = (static_cast<double>(count) / best_elapsed_sec) / 1e6;
        res.mean_ns        = sampler.mean_ns();
        res.p50_ns         = sampler.p50_ns();
        res.p95_ns         = sampler.p95_ns();
        res.p99_ns         = sampler.p99_ns();
        res.p999_ns        = sampler.p999_ns();
        res.max_ns         = sampler.max_ns();
        res.total_trades   = final_trades;
        res.allocs_per_op  = (alloc_sample_n > 0)
                                 ? static_cast<double>(alloc_res.alloc_count) / alloc_sample_n
                                 : 0.0;
        return res;
    }
};

void print_comparison_row(const BenchmarkResult& map_res, const BenchmarkResult& flat_res) {
    const double tput_gain = ((flat_res.throughput_mops - map_res.throughput_mops)
                              / map_res.throughput_mops) * 100.0;

    auto fmt_ns = [](double ns) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << ns;
        return oss.str();
    };

    std::cout << std::left
              << std::setw(14) << map_res.workload_name
              << std::setw(9)  << map_res.operation_count
              << std::setw(14) << (fmt_ns(map_res.p50_ns)  + " / " + fmt_ns(flat_res.p50_ns))
              << std::setw(14) << (fmt_ns(map_res.p95_ns)  + " / " + fmt_ns(flat_res.p95_ns))
              << std::setw(14) << (fmt_ns(map_res.p99_ns)  + " / " + fmt_ns(flat_res.p99_ns))
              << std::setw(14) << (fmt_ns(map_res.p999_ns) + " / " + fmt_ns(flat_res.p999_ns))
              << std::setw(14) << (fmt_ns(map_res.mean_ns) + " / " + fmt_ns(flat_res.mean_ns))
              << std::setw(18) << (std::to_string(map_res.allocs_per_op).substr(0,4) +
                                   " -> " + std::to_string(flat_res.allocs_per_op).substr(0,4))
              << std::fixed << std::setprecision(2)
              << map_res.throughput_mops << " -> " << flat_res.throughput_mops
              << " (" << (tput_gain >= 0.0 ? "+" : "")
              << std::setprecision(1) << tput_gain << "%)\n";
}

int main(int argc, char* argv[]) {
    const uint64_t seed = 0x12345678ULL;
    WorkloadGenerator generator(seed);

    // Calibrate TSC timer at startup (3 x 50 ms sleep against steady_clock).
    BenchTimer timer;
    timer.calibrate(3, 50);

    std::cout << "=======================================================================================================================\n";
    std::cout << " LOW-LATENCY C++ EXCHANGE ENGINE — ORDER BOOK BENCHMARK\n";
    std::cout << " Comparing: MatchingEngine (std::map) vs FlatMatchingEngine (Contiguous Sorted Vector)\n";
    std::cout << "=======================================================================================================================\n\n";

    std::cout << "Timer: TSC (empirical calibration) — " << std::fixed << std::setprecision(3)
              << timer.tsc_ghz() << " GHz\n";
    std::cout << "Note:  Throughput = total_ops / wall_clock_seconds (uninstrumented bulk loop)\n";
    std::cout << "       Latency    = per-op TSC samples; mean/p50/p95/p99/max from the SAME sample array\n\n";

    bool run_10m = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--include-10m") {
            run_10m = true;
        }
    }

    const std::vector<size_t> sizes = {10000, 100000, 1000000};
    std::vector<std::pair<BenchmarkResult, BenchmarkResult>> comparisons;

    for (size_t count : sizes) {
        std::cout << ">>> BENCHMARKING SCALE: " << count << " OPERATIONS <<<\n";

        generator.reset(seed + 1);
        auto add_ops = generator.generate_add_heavy(count);
        auto m_add   = BenchmarkRunner::run<hft::MatchingEngine>(timer, "Map",  "ADD-HEAVY",    add_ops, true);
        auto f_add   = BenchmarkRunner::run<hft::FlatMatchingEngine>(timer, "Flat", "ADD-HEAVY", add_ops, true);
        comparisons.push_back({m_add, f_add});

        generator.reset(seed + 2);
        auto match_ops = generator.generate_match_heavy(count);
        auto m_match   = BenchmarkRunner::run<hft::MatchingEngine>(timer, "Map",  "MATCH-HEAVY",    match_ops, true);
        auto f_match   = BenchmarkRunner::run<hft::FlatMatchingEngine>(timer, "Flat", "MATCH-HEAVY", match_ops, true);
        comparisons.push_back({m_match, f_match});

        generator.reset(seed + 3);
        auto cancel_ops = generator.generate_cancel_heavy(count);
        auto m_cancel   = BenchmarkRunner::run<hft::MatchingEngine>(timer, "Map",  "CANCEL-HEAVY",    cancel_ops, true);
        auto f_cancel   = BenchmarkRunner::run<hft::FlatMatchingEngine>(timer, "Flat", "CANCEL-HEAVY", cancel_ops, true);
        comparisons.push_back({m_cancel, f_cancel});

        generator.reset(seed + 4);
        auto mixed_ops = generator.generate_mixed(count);
        auto m_mixed   = BenchmarkRunner::run<hft::MatchingEngine>(timer, "Map",  "MIXED",    mixed_ops, true);
        auto f_mixed   = BenchmarkRunner::run<hft::FlatMatchingEngine>(timer, "Flat", "MIXED", mixed_ops, true);
        comparisons.push_back({m_mixed, f_mixed});

        std::cout << "\n";
    }

    if (run_10m) {
        const size_t count = 10000000;
        std::cout << ">>> BENCHMARKING 10M LARGE WORKLOAD <<<\n";
        generator.reset(seed + 10);
        auto mixed_10m = generator.generate_mixed(count);
        auto m_10m = BenchmarkRunner::run<hft::MatchingEngine>(timer, "Map",  "MIXED-10M",    mixed_10m, true);
        auto f_10m = BenchmarkRunner::run<hft::FlatMatchingEngine>(timer, "Flat", "MIXED-10M", mixed_10m, true);
        comparisons.push_back({m_10m, f_10m});
        std::cout << "\n";
    }

    // Summary table
    std::cout << "\n=================================================================================================================================\n";
    std::cout << " COMPARISON: Map (std::map) vs Flat (Contiguous Sorted Vector) — Format: [Map] / [Flat]\n";
    std::cout << " All latency columns derived from the same TSC sample array\n";
    std::cout << "=================================================================================================================================\n";
    std::cout << std::left
              << std::setw(14) << "Workload"
              << std::setw(9)  << "Size"
              << std::setw(14) << "p50 (ns)"
              << std::setw(14) << "p95 (ns)"
              << std::setw(14) << "p99 (ns)"
              << std::setw(14) << "p99.9 (ns)"
              << std::setw(14) << "Mean (ns)"
              << std::setw(18) << "Allocs/Op"
              << "Throughput (M ops/s)\n";
    std::cout << "---------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& [map_res, flat_res] : comparisons) {
        print_comparison_row(map_res, flat_res);
    }
    std::cout << "=================================================================================================================================\n";

    return 0;
}
