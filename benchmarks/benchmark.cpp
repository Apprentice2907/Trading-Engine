#include "hft/matching_engine.hpp"
#include "hft/baseline/baseline_matching_engine.hpp"

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
// 2. Deterministic Workload Definitions & Generator
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
                // Passive resting ask @ 10000..10020
                hft::Price price = static_cast<hft::Price>(next_range(10000, 10020));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 50));
                ops.push_back(BenchmarkOp{OpType::SubmitLimit, id, hft::Side::Sell, price, qty});
            } else {
                // Aggressive crossing buy @ 10030 (sweeps asks)
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
// 3. Benchmark Execution & Metrics
// ============================================================================

struct BenchmarkResult {
    std::string engine_type;
    std::string workload_name;
    size_t operation_count{0};
    double elapsed_seconds{0.0};
    double throughput_mops{0.0};
    uint64_t p50_ns{0};
    uint64_t p95_ns{0};
    uint64_t p99_ns{0};
    uint64_t p999_ns{0};
    uint64_t max_ns{0};
    uint64_t total_trades{0};
    double allocs_per_op{0.0};
    double deallocs_per_op{0.0};
    double bytes_per_op{0.0};
};

class BenchmarkRunner {
public:
    using Clock = std::chrono::steady_clock;

    static double measure_timer_overhead_ns() {
        constexpr size_t iterations = 1000000;
        auto start = Clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            auto t = Clock::now();
            (void)t;
        }
        auto end = Clock::now();
        std::chrono::duration<double, std::nano> elapsed = end - start;
        return elapsed.count() / static_cast<double>(iterations);
    }

    template <typename EngineT>
    static BenchmarkResult run(const std::string& engine_type, const std::string& name,
                               const std::vector<BenchmarkOp>& ops, bool pre_reserve = false) {
        const size_t count = ops.size();

        // 1. Allocation Measurement Run (on sample size)
        AllocStats alloc_res;
        {
            EngineT engine;
            if constexpr (std::is_same_v<EngineT, hft::MatchingEngine>) {
                if (pre_reserve) {
                    engine.reserve(count);
                }
            }

            std::vector<hft::Trade> trades;
            trades.reserve(128);

            const size_t alloc_sample_size = std::min(count, size_t{50000});
            g_alloc_stats.reset();
            g_track_allocations = true;

            for (size_t i = 0; i < alloc_sample_size; ++i) {
                const auto& op = ops[i];
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

            g_track_allocations = false;
            alloc_res = g_alloc_stats;
        }

        // 2. Pure Throughput Run (Zero timing overhead inside hot path)
        double best_elapsed_sec = 1e9;
        uint64_t final_trades_count = 0;

        const int iterations = (count >= 10000000) ? 1 : 3;
        for (int iter = 0; iter < iterations; ++iter) {
            EngineT engine;
            if constexpr (std::is_same_v<EngineT, hft::MatchingEngine>) {
                if (pre_reserve) {
                    engine.reserve(count);
                }
            }

            std::vector<hft::Trade> trades;
            trades.reserve(256);

            auto t_start = Clock::now();
            for (size_t i = 0; i < count; ++i) {
                const auto& op = ops[i];
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
            auto t_end = Clock::now();
            std::chrono::duration<double> diff = t_end - t_start;
            if (diff.count() < best_elapsed_sec) {
                best_elapsed_sec = diff.count();
                final_trades_count = engine.total_trades_generated();
            }
        }

        // 3. Latency Distribution Run
        const size_t latency_samples = std::min(count, size_t{1000000});
        std::vector<uint32_t> latencies_ns(latency_samples);

        {
            EngineT engine;
            if constexpr (std::is_same_v<EngineT, hft::MatchingEngine>) {
                if (pre_reserve) {
                    engine.reserve(count);
                }
            }

            std::vector<hft::Trade> trades;
            trades.reserve(128);

            for (size_t i = 0; i < latency_samples; ++i) {
                const auto& op = ops[i];
                auto t0 = Clock::now();
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
                auto t1 = Clock::now();
                latencies_ns[i] = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
        }

        std::sort(latencies_ns.begin(), latencies_ns.end());

        auto get_percentile = [&latencies_ns](double p) -> uint64_t {
            size_t idx = static_cast<size_t>(p * static_cast<double>(latencies_ns.size() - 1));
            return latencies_ns[idx];
        };

        BenchmarkResult res;
        res.engine_type = engine_type;
        res.workload_name = name;
        res.operation_count = count;
        res.elapsed_seconds = best_elapsed_sec;
        res.throughput_mops = (static_cast<double>(count) / best_elapsed_sec) / 1e6;
        res.p50_ns = get_percentile(0.50);
        res.p95_ns = get_percentile(0.95);
        res.p99_ns = get_percentile(0.99);
        res.p999_ns = get_percentile(0.999);
        res.max_ns = latencies_ns.back();
        res.total_trades = final_trades_count;

        const double sample_count = static_cast<double>(std::min(count, size_t{50000}));
        res.allocs_per_op = static_cast<double>(alloc_res.alloc_count) / sample_count;
        res.deallocs_per_op = static_cast<double>(alloc_res.dealloc_count) / sample_count;
        res.bytes_per_op = static_cast<double>(alloc_res.bytes_allocated) / sample_count;

        return res;
    }
};

void print_comparison_row(const BenchmarkResult& base, const BenchmarkResult& opt) {
    const double tput_gain = ((opt.throughput_mops - base.throughput_mops) / base.throughput_mops) * 100.0;

    std::cout << std::left
              << std::setw(14) << base.workload_name
              << std::setw(9)  << base.operation_count
              << std::setw(11) << (std::to_string(base.p50_ns) + " / " + std::to_string(opt.p50_ns))
              << std::setw(11) << (std::to_string(base.p99_ns) + " / " + std::to_string(opt.p99_ns))
              << std::setw(12) << (std::to_string(base.p999_ns) + " / " + std::to_string(opt.p999_ns))
              << std::setw(20) << (std::to_string(base.max_ns / 1000) + "us / " + std::to_string(opt.max_ns / 1000) + "us")
              << std::setw(18) << (std::to_string(base.allocs_per_op).substr(0, 4) + " -> " + std::to_string(opt.allocs_per_op).substr(0, 4))
              << std::fixed << std::setprecision(2) << base.throughput_mops << " -> " << opt.throughput_mops
              << " (" << (tput_gain >= 0.0 ? "+" : "") << std::setprecision(1) << tput_gain << "%)\n";
}

int main(int argc, char* argv[]) {
    const uint64_t seed = 0x12345678ULL;
    WorkloadGenerator generator(seed);

    const double timer_overhead = BenchmarkRunner::measure_timer_overhead_ns();

    std::cout << "=======================================================================================================\n";
    std::cout << " LOW-LATENCY C++ EXCHANGE ENGINE: PHASE 3 MEMORY OPTIMIZATION BENCHMARK\n";
    std::cout << " Comparing: Baseline (std::list) vs Optimized (OrderPool + Rehash Control)\n";
    std::cout << "=======================================================================================================\n\n";

    std::cout << "Environment:\n";
    std::cout << "  Platform         : Windows x64\n";
    std::cout << "  Compiler         : MSVC 19.50 (C++20 Release /O2)\n";
    std::cout << "  Timer            : std::chrono::steady_clock (QPC overhead: " << std::fixed << std::setprecision(1) << timer_overhead << " ns)\n";
    std::cout << "  Deterministic Seed: 0x" << std::hex << seed << std::dec << "\n\n";

    bool run_10m = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--include-10m") {
            run_10m = true;
        }
    }

    std::vector<size_t> sizes = {10000, 100000, 1000000};

    std::vector<std::pair<BenchmarkResult, BenchmarkResult>> comparisons;

    for (size_t count : sizes) {
        std::cout << ">>> BENCHMARKING SCALE: " << count << " OPERATIONS <<<\n";

        // ADD-HEAVY
        generator.reset(seed + 1);
        auto add_ops = generator.generate_add_heavy(count);
        auto b_add = BenchmarkRunner::run<hft::baseline::MatchingEngine>("Baseline", "ADD-HEAVY", add_ops, false);
        auto o_add = BenchmarkRunner::run<hft::MatchingEngine>("Optimized", "ADD-HEAVY", add_ops, true);
        comparisons.push_back({b_add, o_add});
        std::cout << "  [ADD-HEAVY]   Baseline: " << std::fixed << std::setprecision(2) << b_add.throughput_mops
                  << " M/s (" << b_add.allocs_per_op << " allocs/op)  -->  Optimized: "
                  << o_add.throughput_mops << " M/s (" << o_add.allocs_per_op << " allocs/op)\n";

        // MATCH-HEAVY
        generator.reset(seed + 2);
        auto match_ops = generator.generate_match_heavy(count);
        auto b_match = BenchmarkRunner::run<hft::baseline::MatchingEngine>("Baseline", "MATCH-HEAVY", match_ops, false);
        auto o_match = BenchmarkRunner::run<hft::MatchingEngine>("Optimized", "MATCH-HEAVY", match_ops, true);
        comparisons.push_back({b_match, o_match});
        std::cout << "  [MATCH-HEAVY] Baseline: " << std::fixed << std::setprecision(2) << b_match.throughput_mops
                  << " M/s (" << b_match.allocs_per_op << " allocs/op)  -->  Optimized: "
                  << o_match.throughput_mops << " M/s (" << o_match.allocs_per_op << " allocs/op)\n";

        // CANCEL-HEAVY
        generator.reset(seed + 3);
        auto cancel_ops = generator.generate_cancel_heavy(count);
        auto b_cancel = BenchmarkRunner::run<hft::baseline::MatchingEngine>("Baseline", "CANCEL-HEAVY", cancel_ops, false);
        auto o_cancel = BenchmarkRunner::run<hft::MatchingEngine>("Optimized", "CANCEL-HEAVY", cancel_ops, true);
        comparisons.push_back({b_cancel, o_cancel});
        std::cout << "  [CANCEL-HEAVY]Baseline: " << std::fixed << std::setprecision(2) << b_cancel.throughput_mops
                  << " M/s (" << b_cancel.allocs_per_op << " allocs/op)  -->  Optimized: "
                  << o_cancel.throughput_mops << " M/s (" << o_cancel.allocs_per_op << " allocs/op)\n";

        // MIXED
        generator.reset(seed + 4);
        auto mixed_ops = generator.generate_mixed(count);
        auto b_mixed = BenchmarkRunner::run<hft::baseline::MatchingEngine>("Baseline", "MIXED", mixed_ops, false);
        auto o_mixed = BenchmarkRunner::run<hft::MatchingEngine>("Optimized", "MIXED", mixed_ops, true);
        comparisons.push_back({b_mixed, o_mixed});
        std::cout << "  [MIXED]       Baseline: " << std::fixed << std::setprecision(2) << b_mixed.throughput_mops
                  << " M/s (" << b_mixed.allocs_per_op << " allocs/op)  -->  Optimized: "
                  << o_mixed.throughput_mops << " M/s (" << o_mixed.allocs_per_op << " allocs/op)\n\n";
    }

    if (run_10m) {
        std::cout << ">>> BENCHMARKING 10M LARGE WORKLOAD (STRESS TEST) <<<\n";
        const size_t count = 10000000;
        generator.reset(seed + 10);
        auto mixed_10m = generator.generate_mixed(count);
        auto b_10m = BenchmarkRunner::run<hft::baseline::MatchingEngine>("Baseline", "MIXED-10M", mixed_10m, false);
        auto o_10m = BenchmarkRunner::run<hft::MatchingEngine>("Optimized", "MIXED-10M", mixed_10m, true);
        comparisons.push_back({b_10m, o_10m});
        std::cout << "  [MIXED-10M]   Baseline: " << std::fixed << std::setprecision(2) << b_10m.throughput_mops
                  << " M/s (" << b_10m.allocs_per_op << " allocs/op)  -->  Optimized: "
                  << o_10m.throughput_mops << " M/s (" << o_10m.allocs_per_op << " allocs/op)\n\n";
    }

    std::cout << "\n===============================================================================================================================\n";
    std::cout << " PHASE 3 COMPARISON: BASELINE (std::list) vs OPTIMIZED (OrderPool + Rehash Control)\n";
    std::cout << " Format: [Baseline] / [Optimized]\n";
    std::cout << "===============================================================================================================================\n";
    std::cout << std::left
              << std::setw(14) << "Workload"
              << std::setw(9)  << "Size"
              << std::setw(11) << "p50 (ns)"
              << std::setw(11) << "p99 (ns)"
              << std::setw(12) << "p99.9 (ns)"
              << std::setw(20) << "Max Latency"
              << std::setw(18) << "Allocs/Op"
              << "Throughput (M ops/s)\n";
    std::cout << "-------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& [base, opt] : comparisons) {
        print_comparison_row(base, opt);
    }
    std::cout << "===============================================================================================================================\n";

    return 0;
}
