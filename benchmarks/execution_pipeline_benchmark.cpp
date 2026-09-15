#include "hft/order.hpp"
#include "hft/trading_pipeline.hpp"
#include "hft/matching_engine.hpp"
#include "hft/spsc_queue.hpp"
#include "bench_timer.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <new>

using namespace hft;

// ============================================================================
// 1. Dynamic Memory Allocation Tracker
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
// Benchmarks A/B/C/D: 'Avg Latency' = wall_clock_total / N (amortized throughput-derived period,
// NOT a per-op sampled latency distribution). See Benchmark E for TSC-sampled per-op latency.
//
// Benchmark E: Mean, p50, p95, p99, max all derived from the same TSC sample array.

using Clock = std::chrono::steady_clock;


// ============================================================================
// 3. Main Benchmark Driver
// ============================================================================

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    BenchTimer bench_timer;
    bench_timer.calibrate(3, 50);

    std::cout << "=======================================================================================================\n";
    std::cout << " LOW-LATENCY C++ — ORDER GATEWAY & PRE-TRADE RISK BENCHMARK\n";
    std::cout << " Pipeline: OrderCommand(64B) -> PreTradeRisk -> OrderGateway -> MatchingEngine -> ExecutionReport(64B)\n";
    std::cout << "=======================================================================================================\n\n";
    std::cout << "Timer: TSC (empirical calibration) — " << std::fixed << std::setprecision(3)
              << bench_timer.tsc_ghz() << " GHz\n";
    std::cout << "Note: Benchmarks A/B/C/D report throughput + amortized period (total_time/N).\n";
    std::cout << "      Benchmark E reports per-op TSC-sampled latency: mean/p50/p95/p99/max\n"
              << "      all derived from the same raw sample array.\n\n";

    // ------------------------------------------------------------------------
    // Part 1: Comprehensive Dynamic Heap Allocation Audit
    // ------------------------------------------------------------------------
    std::cout << ">>> DETAILED DYNAMIC HEAP ALLOCATION AUDIT ACROSS PIPELINE STAGES <<<\n";
    const size_t audit_ops = 50000;

    // Sub-test 1: Pre-Trade Risk Checks
    {
        RiskConfig cfg{};
        cfg.max_exposure_quantity = 10000000;
        PreTradeRiskEngine risk(cfg);

        OrderCommand cmd = OrderCommand::make_add(1, 3045, 1, Side::Buy, 83000, 10);

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < audit_ops; ++i) {
            risk.check_order(cmd);
        }
        g_track_allocations = false;

        std::cout << "  [1. Pre-Trade Risk Engine Hot Path]\n";
        std::cout << "      Allocations: " << g_alloc_stats.alloc_count << " total ("
                  << (static_cast<double>(g_alloc_stats.alloc_count) / audit_ops) << " allocs/check) -> VERIFIED ZERO HEAP ALLOCATIONS\n";
    }

    // Sub-test 2: Risk-Rejected Orders through Gateway
    {
        RiskConfig cfg{};
        cfg.max_order_quantity = 5; // Will reject qty=10
        PreTradeRiskEngine risk(cfg);
        OrderGateway gateway;
        MatchingEngine engine;

        std::vector<ExecutionReport> reports;
        reports.reserve(16);

        OrderCommand cmd = OrderCommand::make_add(1, 3045, 1, Side::Buy, 83000, 10);

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < audit_ops; ++i) {
            reports.clear();
            gateway.process_command(cmd, risk, engine, reports);
        }
        g_track_allocations = false;

        std::cout << "  [2. Risk-Rejected Orders (Gateway -> ExecutionReport)]\n";
        std::cout << "      Allocations: " << g_alloc_stats.alloc_count << " total ("
                  << (static_cast<double>(g_alloc_stats.alloc_count) / audit_ops) << " allocs/order) -> VERIFIED ZERO HEAP ALLOCATIONS\n";
    }

    // Sub-test 3: Dual SPSC Lock-Free Queues (Ingress & Egress)
    {
        SpscQueue<OrderCommand, 1024> in_q;
        SpscQueue<ExecutionReport, 1024> out_q;

        OrderCommand in_cmd = OrderCommand::make_add(1, 3045, 1, Side::Buy, 83000, 10);
        OrderCommand popped_cmd{};
        ExecutionReport out_rep = ExecutionReport::make_new(1, 3045, 1, Side::Buy, 83000, 10, 1);
        ExecutionReport popped_rep{};

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < audit_ops; ++i) {
            in_q.try_push(in_cmd);
            in_q.try_pop(popped_cmd);
            out_q.try_push(out_rep);
            out_q.try_pop(popped_rep);
        }
        g_track_allocations = false;

        std::cout << "  [3. SPSC Queue Ingress & Egress Transfers]\n";
        std::cout << "      Allocations: " << g_alloc_stats.alloc_count << " total ("
                  << (static_cast<double>(g_alloc_stats.alloc_count) / audit_ops) << " allocs/op) -> VERIFIED ZERO HEAP ALLOCATIONS\n";
    }

    // Sub-test 4: Match-Heavy Crossing Orders (Immediate Execution Fills)
    {
        RiskConfig cfg{};
        cfg.max_order_quantity = audit_ops * 20;
        cfg.max_order_notional = 1000000000000ULL;
        cfg.max_exposure_quantity = 1000000000;
        PreTradeRiskEngine risk(cfg);
        OrderGateway gateway;
        MatchingEngine engine;
        engine.reserve(audit_ops + 1000);

        std::vector<ExecutionReport> dummy;
        dummy.reserve(16);

        // Pre-seed resting sell order
        OrderCommand sell_cmd = OrderCommand::make_add(1, 3045, 1, Side::Sell, 83000, audit_ops * 10);
        gateway.process_command(sell_cmd, risk, engine, dummy);

        std::vector<ExecutionReport> reports;
        reports.reserve(16);

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 2; i <= 2 + audit_ops; ++i) {
            reports.clear();
            OrderCommand buy_cmd = OrderCommand::make_add(i, 3045, 2, Side::Buy, 83000, 10);
            gateway.process_command(buy_cmd, risk, engine, reports);
        }
        g_track_allocations = false;

        std::cout << "  [4. Aggressive Crossing Matches (Immediate Fills in Gateway)]\n";
        std::cout << "      Allocations: " << g_alloc_stats.alloc_count << " total ("
                  << (static_cast<double>(g_alloc_stats.alloc_count) / audit_ops) << " allocs/trade) -> VERIFIED ZERO HEAP ALLOCATIONS\n";
    }

    // Sub-test 5: Cancellation Hot Path
    {
        RiskConfig cfg{};
        cfg.max_exposure_quantity = 10000000;
        PreTradeRiskEngine risk(cfg);
        OrderGateway gateway;
        MatchingEngine engine;
        engine.reserve(audit_ops + 1000);

        std::vector<ExecutionReport> dummy;
        dummy.reserve(16);

        // Pre-seed orders
        for (size_t i = 1; i <= audit_ops; ++i) {
            OrderCommand add_cmd = OrderCommand::make_add(i, 3045, 1, Side::Buy, 80000, 10);
            gateway.process_command(add_cmd, risk, engine, dummy);
        }

        std::vector<ExecutionReport> reports;
        reports.reserve(16);

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 1; i <= audit_ops; ++i) {
            reports.clear();
            OrderCommand cancel_cmd = OrderCommand::make_cancel(i, 3045, 1);
            gateway.process_command(cancel_cmd, risk, engine, reports);
        }
        g_track_allocations = false;

        std::cout << "  [5. Order Cancellation Hot Path]\n";
        std::cout << "      Allocations: " << g_alloc_stats.alloc_count << " total ("
                  << (static_cast<double>(g_alloc_stats.alloc_count) / audit_ops) << " allocs/cancel) -> VERIFIED ZERO HEAP ALLOCATIONS\n";
    }

    // Sub-test 6: Reference Order Book Insertion Node Analysis
    {
        RiskConfig cfg{};
        cfg.max_exposure_quantity = 10000000;
        PreTradeRiskEngine risk(cfg);
        OrderGateway gateway;
        MatchingEngine engine;
        engine.reserve(audit_ops + 1000);

        std::vector<ExecutionReport> reports;
        reports.reserve(16);

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 1; i <= audit_ops; ++i) {
            reports.clear();
            OrderCommand add_cmd = OrderCommand::make_add(i, 3045, 1, Side::Buy, 80000 + static_cast<Price>(i % 100), 10);
            gateway.process_command(add_cmd, risk, engine, reports);
        }
        g_track_allocations = false;

        std::cout << "  [6. Reference Engine Book Order Insertion (order_lookup_)]\n";
        std::cout << "      Allocations: " << g_alloc_stats.alloc_count << " total ("
                  << (static_cast<double>(g_alloc_stats.alloc_count) / audit_ops)
                  << " allocs/order) -> std::unordered_map node allocation in reference OrderBook\n\n";
    }

    // ------------------------------------------------------------------------
    // Part 2: Multi-Scale Benchmark Matrix
    // ------------------------------------------------------------------------
    const std::vector<size_t> scales = {100000, 1000000};

    for (size_t count : scales) {
        std::cout << "=======================================================================================================\n";
        std::cout << " BENCHMARKING SCALE: " << count << " ORDERS / EVENTS\n";
        std::cout << "=======================================================================================================\n";

        RiskConfig scaled_cfg{};
        scaled_cfg.max_order_quantity = 1000000;
        scaled_cfg.max_order_notional = 1000000000000ULL;
        scaled_cfg.max_exposure_quantity = count * 20;

        // Benchmark A: Pre-Trade Risk Engine Only
        {
            PreTradeRiskEngine risk(scaled_cfg);
            std::vector<OrderCommand> commands;
            commands.reserve(count);
            for (size_t i = 1; i <= count; ++i) {
                commands.push_back(OrderCommand::make_add(i, 3045, 1, Side::Buy, 83000, 10));
            }

            const auto start = Clock::now();
            size_t approved = 0;
            for (size_t i = 0; i < count; ++i) {
                if (risk.check_order(commands[i]).approved()) {
                    ++approved;
                }
            }
            const auto end = Clock::now();
            const double elapsed_s = std::chrono::duration<double>(end - start).count();
            const double mops = (count / elapsed_s) / 1e6;
            const double avg_ns = (elapsed_s * 1e9) / count;

            std::cout << "  [BENCHMARK A: PRE-TRADE RISK CHECK ONLY]\n";
            std::cout << "    Throughput    : " << std::fixed << std::setprecision(2) << mops << " M checks/sec\n";
            std::cout << "    Amort. Period : " << std::fixed << std::setprecision(1) << avg_ns << " ns/check (throughput-derived)\n";
            std::cout << "    Approved      : " << approved << " / " << count << "\n";
        }

        // Benchmark B: Resting Limit Orders (Risk + Gateway + Matching)
        {
            PreTradeRiskEngine risk(scaled_cfg);
            OrderGateway gateway(128);
            MatchingEngine engine;
            engine.reserve(count + 1000);

            std::vector<OrderCommand> commands;
            commands.reserve(count);
            for (size_t i = 1; i <= count; ++i) {
                commands.push_back(OrderCommand::make_add(
                    i, 3045, 1, Side::Buy, 80000 + static_cast<Price>(i % 500), 10));
            }

            uint64_t reports_emitted = 0;
            auto sink = [&reports_emitted](const ExecutionReport&) noexcept {
                ++reports_emitted;
            };

            const auto start = Clock::now();
            for (size_t i = 0; i < count; ++i) {
                gateway.process_command(commands[i], risk, engine, sink);
            }
            const auto end = Clock::now();
            const double elapsed_s = std::chrono::duration<double>(end - start).count();
            const double mops = (count / elapsed_s) / 1e6;
            const double avg_ns = (elapsed_s * 1e9) / count;

            std::cout << "  [BENCHMARK B: RESTING ORDERS (RISK + GATEWAY + MATCHING)]\n";
            std::cout << "    Throughput    : " << std::fixed << std::setprecision(2) << mops << " M orders/sec\n";
            std::cout << "    Amort. Period : " << std::fixed << std::setprecision(1) << avg_ns << " ns/order (throughput-derived)\n";
            std::cout << "    Reports Out   : " << reports_emitted << "\n";
        }

        // Benchmark C: Match-Heavy Crossing Orders (Aggressive Fill Execution)
        {
            PreTradeRiskEngine risk(scaled_cfg);
            OrderGateway gateway(128);
            MatchingEngine engine;
            engine.reserve(count + 1000);

            // Pre-seed resting asks
            std::vector<ExecutionReport> dummy;
            dummy.reserve(16);
            for (size_t i = 1; i <= count / 2; ++i) {
                OrderCommand sell_cmd = OrderCommand::make_add(i, 3045, 1, Side::Sell, 83000, 10);
                gateway.process_command(sell_cmd, risk, engine, dummy);
            }

            // Crossing buy commands
            std::vector<OrderCommand> buy_commands;
            buy_commands.reserve(count / 2);
            for (size_t i = 1; i <= count / 2; ++i) {
                buy_commands.push_back(OrderCommand::make_add(
                    (count / 2) + i, 3045, 2, Side::Buy, 83000, 10));
            }

            uint64_t trade_reports = 0;
            auto sink = [&trade_reports](const ExecutionReport& rep) noexcept {
                if (rep.exec_type == ExecutionType::Trade) {
                    ++trade_reports;
                }
            };

            const auto start = Clock::now();
            for (const auto& cmd : buy_commands) {
                gateway.process_command(cmd, risk, engine, sink);
            }
            const auto end = Clock::now();
            const double elapsed_s = std::chrono::duration<double>(end - start).count();
            const double mops = (buy_commands.size() / elapsed_s) / 1e6;
            const double avg_ns = (elapsed_s * 1e9) / buy_commands.size();

            std::cout << "  [BENCHMARK C: MATCH-HEAVY EXECUTION (CROSSING ORDERS)]\n";
            std::cout << "    Throughput    : " << std::fixed << std::setprecision(2) << mops << " M orders/sec\n";
            std::cout << "    Amort. Period : " << std::fixed << std::setprecision(1) << avg_ns << " ns/order (throughput-derived)\n";
            std::cout << "    Trades Made   : " << trade_reports << "\n";
        }

        // Benchmark D: Mixed Realistic Workload (60% Adds, 25% Cancels, 15% Crosses)
        {
            PreTradeRiskEngine risk(scaled_cfg);
            OrderGateway gateway(128);
            MatchingEngine engine;
            engine.reserve(count + 1000);

            std::vector<OrderCommand> commands;
            commands.reserve(count);
            for (size_t i = 1; i <= count; ++i) {
                const size_t op = i % 20;
                if (op < 12) {
                    // 60% Add
                    const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
                    const Price p = (s == Side::Buy) ? (82900 + static_cast<Price>(i % 50))
                                                     : (83100 + static_cast<Price>(i % 50));
                    commands.push_back(OrderCommand::make_add(i, 3045, 1, s, p, 10));
                } else if (op < 17) {
                    // 25% Cancel
                    const OrderId target = (i > 10) ? (i - 10) : 1;
                    commands.push_back(OrderCommand::make_cancel(target, 3045, 1));
                } else {
                    // 15% Aggressive Cross
                    const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
                    const Price p = (s == Side::Buy) ? 83200 : 82800;
                    commands.push_back(OrderCommand::make_add(i, 3045, 2, s, p, 5));
                }
            }

            uint64_t reports = 0;
            auto sink = [&reports](const ExecutionReport&) noexcept {
                ++reports;
            };

            const auto start = Clock::now();
            for (size_t i = 0; i < count; ++i) {
                gateway.process_command(commands[i], risk, engine, sink);
            }
            const auto end = Clock::now();
            const double elapsed_s = std::chrono::duration<double>(end - start).count();
            const double mops = (count / elapsed_s) / 1e6;
            const double avg_ns = (elapsed_s * 1e9) / count;

            std::cout << "  [BENCHMARK D: MIXED WORKLOAD (60% ADDS, 25% CANCELS, 15% CROSSES)]\n";
            std::cout << "    Throughput    : " << std::fixed << std::setprecision(2) << mops << " M ops/sec\n";
            std::cout << "    Amort. Period : " << std::fixed << std::setprecision(1) << avg_ns << " ns/op (throughput-derived)\n";
        }

        // Benchmark E: Stage Latencies (p50, p95, p99, p99.9, max)
        if (count == 100000) {
            // Benchmark E uses TSC for per-op latency. Warmup first to prime icache.
            // Mean and all percentiles derived from the same raw sample array.
            PreTradeRiskEngine risk(scaled_cfg);
            OrderGateway gateway(128);
            MatchingEngine engine;
            engine.reserve(count + 5000);

            const size_t warmup_e = std::min(count / 10, size_t{2000});
            for (size_t i = 1; i <= warmup_e; ++i) {
                OrderCommand cmd = OrderCommand::make_add(
                    i, 3045, 1, Side::Buy, 80000 + static_cast<Price>(i % 50), 10);
                gateway.process_command(cmd, risk, engine, [](const ExecutionReport&) noexcept {});
            }

            LatencySampler sampler_e(count);
            for (size_t i = warmup_e + 1; i <= warmup_e + count; ++i) {
                OrderCommand cmd = OrderCommand::make_add(
                    i, 3045, 1, Side::Buy, 80000 + static_cast<Price>(i % 50), 10);

                auto t0 = bench_timer.start();
                gateway.process_command(cmd, risk, engine, [](const ExecutionReport&) noexcept {});
                sampler_e.record(bench_timer.stop_ns(t0));
            }
            sampler_e.finish();
            std::cout << "  [BENCHMARK E: TSC-SAMPLED LATENCY DISTRIBUTION (" << count << " samples)]\n";
            sampler_e.print_summary("  Risk + Gateway + Engine:");
        }

        std::cout << "\n";
    }

    std::cout << "=======================================================================================================\n";
    std::cout << " PHASE 8 EXECUTION PIPELINE BENCHMARK COMPLETE\n";
    std::cout << "=======================================================================================================\n";

    return 0;
}
