#include "hft/matching_engine.hpp"
#include "hft/event_pipeline.hpp"
#include "hft/event.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <algorithm>
#include <string>
#include <cstdlib>

// ============================================================================
// 1. Workload Generator
// ============================================================================

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

    void reset(uint64_t seed = 0x12345678ULL) noexcept {
        state_ = seed;
        initial_seed_ = seed;
    }

    std::vector<hft::OrderEvent> generate_add_heavy(size_t count) {
        std::vector<hft::OrderEvent> events;
        events.reserve(count);
        hft::OrderId next_id = 1;

        for (size_t i = 0; i < count; ++i) {
            hft::OrderId id = next_id++;
            hft::Side side = (next_u64() % 2 == 0) ? hft::Side::Buy : hft::Side::Sell;
            hft::Price price = (side == hft::Side::Buy)
                ? static_cast<hft::Price>(next_range(9000, 9999))
                : static_cast<hft::Price>(next_range(10001, 11000));
            hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 100));

            events.push_back(hft::OrderEvent::make_add(id, side, price, qty));
        }
        return events;
    }

    std::vector<hft::OrderEvent> generate_match_heavy(size_t count) {
        std::vector<hft::OrderEvent> events;
        events.reserve(count);
        hft::OrderId next_id = 1;

        for (size_t i = 0; i < count; ++i) {
            hft::OrderId id = next_id++;
            if (i % 2 == 0) {
                hft::Price price = static_cast<hft::Price>(next_range(10000, 10020));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 50));
                events.push_back(hft::OrderEvent::make_add(id, hft::Side::Sell, price, qty));
            } else {
                hft::Price price = static_cast<hft::Price>(next_range(10020, 10030));
                hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 60));
                events.push_back(hft::OrderEvent::make_add(id, hft::Side::Buy, price, qty));
            }
        }
        return events;
    }

    std::vector<hft::OrderEvent> generate_cancel_heavy(size_t count) {
        std::vector<hft::OrderEvent> events;
        events.reserve(count);
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

                events.push_back(hft::OrderEvent::make_add(id, side, price, qty));
                live_ids.push_back(id);
            } else {
                size_t idx = static_cast<size_t>(next_range(0, live_ids.size() - 1));
                hft::OrderId id = live_ids[idx];
                live_ids[idx] = live_ids.back();
                live_ids.pop_back();

                events.push_back(hft::OrderEvent::make_cancel(id));
            }
        }
        return events;
    }

    std::vector<hft::OrderEvent> generate_mixed(size_t count) {
        std::vector<hft::OrderEvent> events;
        events.reserve(count);
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

                events.push_back(hft::OrderEvent::make_add(id, side, price, qty));
                active_ids.push_back(id);
            } else if (roll <= 80) {
                size_t idx = static_cast<size_t>(next_range(0, active_ids.size() - 1));
                hft::OrderId id = active_ids[idx];
                active_ids[idx] = active_ids.back();
                active_ids.pop_back();

                events.push_back(hft::OrderEvent::make_cancel(id));
            } else {
                size_t idx = static_cast<size_t>(next_range(0, active_ids.size() - 1));
                hft::OrderId id = active_ids[idx];
                hft::Price new_price = static_cast<hft::Price>(next_range(9970, 10030));
                hft::Quantity new_qty = static_cast<hft::Quantity>(next_range(10, 80));

                events.push_back(hft::OrderEvent::make_modify(id, new_price, new_qty));
            }
        }
        return events;
    }

private:
    uint64_t state_;
    uint64_t initial_seed_;
};

// ============================================================================
// 2. Metrics & Execution Runner
// ============================================================================

struct PipelineBenchmarkResult {
    std::string mode_name;
    std::string workload_name;
    size_t operation_count{0};
    double elapsed_sec{0.0};
    double throughput_mops{0.0};
    uint64_t p50_ns{0};
    uint64_t p95_ns{0};
    uint64_t p99_ns{0};
    uint64_t p999_ns{0};
    uint64_t max_ns{0};
};

class PipelineBenchmarkRunner {
public:
    using Clock = std::chrono::steady_clock;

    static PipelineBenchmarkResult run_direct(const std::string& name, const std::vector<hft::OrderEvent>& events) {
        const size_t count = events.size();

        // 1. Pure throughput run
        double best_sec = 1e9;
        const int iters = (count >= 1000000) ? 2 : 3;
        for (int i = 0; i < iters; ++i) {
            hft::MatchingEngine engine;
            engine.reserve(count);
            std::vector<hft::Trade> trades;
            trades.reserve(128);

            auto t0 = Clock::now();
            for (const auto& ev : events) {
                switch (ev.type) {
                    case hft::EventType::Add:
                        engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, trades);
                        trades.clear();
                        break;
                    case hft::EventType::Cancel:
                        engine.cancel_order(ev.id);
                        break;
                    case hft::EventType::Modify:
                        engine.modify_order(ev.id, ev.price, ev.qty, trades);
                        trades.clear();
                        break;
                }
            }
            auto t1 = Clock::now();
            double sec = std::chrono::duration<double>(t1 - t0).count();
            if (sec < best_sec) best_sec = sec;
        }

        // 2. Latency measurement run
        const size_t latency_samples = std::min(count, size_t{1000000});
        std::vector<uint32_t> latencies(latency_samples);
        {
            hft::MatchingEngine engine;
            engine.reserve(count);
            std::vector<hft::Trade> trades;
            trades.reserve(128);

            for (size_t i = 0; i < latency_samples; ++i) {
                const auto& ev = events[i];
                auto t0 = Clock::now();
                switch (ev.type) {
                    case hft::EventType::Add:
                        engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, trades);
                        trades.clear();
                        break;
                    case hft::EventType::Cancel:
                        engine.cancel_order(ev.id);
                        break;
                    case hft::EventType::Modify:
                        engine.modify_order(ev.id, ev.price, ev.qty, trades);
                        trades.clear();
                        break;
                }
                auto t1 = Clock::now();
                latencies[i] = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
        }
        std::sort(latencies.begin(), latencies.end());

        auto get_pct = [&](double p) -> uint64_t {
            size_t idx = static_cast<size_t>(p * static_cast<double>(latencies.size() - 1));
            return latencies[idx];
        };

        PipelineBenchmarkResult res;
        res.mode_name = "Direct";
        res.workload_name = name;
        res.operation_count = count;
        res.elapsed_sec = best_sec;
        res.throughput_mops = (static_cast<double>(count) / best_sec) / 1e6;
        res.p50_ns = get_pct(0.50);
        res.p95_ns = get_pct(0.95);
        res.p99_ns = get_pct(0.99);
        res.p999_ns = get_pct(0.999);
        res.max_ns = latencies.back();
        return res;
    }

    static PipelineBenchmarkResult run_pipeline(const std::string& name, const std::vector<hft::OrderEvent>& events) {
        const size_t count = events.size();

        // 1. Throughput run (producer enqueue + consumer execute)
        double best_sec = 1e9;
        const int iters = (count >= 1000000) ? 2 : 3;
        for (int iter = 0; iter < iters; ++iter) {
            hft::MatchingEnginePipeline pipeline(hft::MatchingEnginePipeline::DEFAULT_QUEUE_CAPACITY, count);
            pipeline.start();

            auto t0 = Clock::now();
            for (const auto& ev : events) {
                pipeline.enqueue_event_wait(ev);
            }
            pipeline.stop_and_join();
            auto t1 = Clock::now();

            double sec = std::chrono::duration<double>(t1 - t0).count();
            if (sec < best_sec) best_sec = sec;
        }

        // 2. Latency measurement run (Producer enqueue latency)
        const size_t latency_samples = std::min(count, size_t{1000000});
        std::vector<uint32_t> latencies(latency_samples);
        {
            hft::MatchingEnginePipeline pipeline(hft::MatchingEnginePipeline::DEFAULT_QUEUE_CAPACITY, count);
            pipeline.start();

            for (size_t i = 0; i < latency_samples; ++i) {
                const auto& ev = events[i];
                auto t0 = Clock::now();
                pipeline.enqueue_event_wait(ev);
                auto t1 = Clock::now();
                latencies[i] = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
            pipeline.stop_and_join();
        }
        std::sort(latencies.begin(), latencies.end());

        auto get_pct = [&](double p) -> uint64_t {
            size_t idx = static_cast<size_t>(p * static_cast<double>(latencies.size() - 1));
            return latencies[idx];
        };

        PipelineBenchmarkResult res;
        res.mode_name = "SPSC Pipeline";
        res.workload_name = name;
        res.operation_count = count;
        res.elapsed_sec = best_sec;
        res.throughput_mops = (static_cast<double>(count) / best_sec) / 1e6;
        res.p50_ns = get_pct(0.50);
        res.p95_ns = get_pct(0.95);
        res.p99_ns = get_pct(0.99);
        res.p999_ns = get_pct(0.999);
        res.max_ns = latencies.back();
        return res;
    }
};

void print_comparison_row(const PipelineBenchmarkResult& direct, const PipelineBenchmarkResult& pipe) {
    const double overhead_pct = ((pipe.elapsed_sec - direct.elapsed_sec) / direct.elapsed_sec) * 100.0;

    std::cout << std::left
              << std::setw(14) << direct.workload_name
              << std::setw(9)  << direct.operation_count
              << std::setw(13) << (std::to_string(direct.p50_ns) + " / " + std::to_string(pipe.p50_ns))
              << std::setw(13) << (std::to_string(direct.p95_ns) + " / " + std::to_string(pipe.p95_ns))
              << std::setw(13) << (std::to_string(direct.p99_ns) + " / " + std::to_string(pipe.p99_ns))
              << std::setw(14) << (std::to_string(direct.p999_ns) + " / " + std::to_string(pipe.p999_ns))
              << std::setw(20) << (std::to_string(direct.max_ns / 1000) + "us / " + std::to_string(pipe.max_ns / 1000) + "us")
              << std::fixed << std::setprecision(2) << direct.throughput_mops << " -> " << pipe.throughput_mops
              << " (" << (overhead_pct >= 0 ? "+" : "") << std::setprecision(1) << overhead_pct << "% wall time)\n";
}

int main() {
    const uint64_t seed = 0x12345678ULL;
    WorkloadGenerator generator(seed);

    std::cout << "=======================================================================================================================\n";
    std::cout << " LOW-LATENCY C++ EXCHANGE ENGINE: PHASE 5 PIPELINE OVERHEAD BENCHMARK\n";
    std::cout << " Comparing: Direct Single-Threaded Engine vs Threaded SPSC Pipeline (Producer -> SPSC -> Consumer)\n";
    std::cout << "=======================================================================================================================\n\n";

    std::vector<size_t> sizes = {10000, 100000, 1000000};
    std::vector<std::pair<PipelineBenchmarkResult, PipelineBenchmarkResult>> results;

    for (size_t count : sizes) {
        std::cout << ">>> BENCHMARKING SCALE: " << count << " OPERATIONS <<<\n";

        // ADD-HEAVY
        generator.reset(seed + 1);
        auto add_ops = generator.generate_add_heavy(count);
        auto d_add = PipelineBenchmarkRunner::run_direct("ADD-HEAVY", add_ops);
        auto p_add = PipelineBenchmarkRunner::run_pipeline("ADD-HEAVY", add_ops);
        results.push_back({d_add, p_add});
        std::cout << "  [ADD-HEAVY]   Direct: " << std::fixed << std::setprecision(2) << d_add.throughput_mops
                  << " M/s  -->  Pipeline: " << p_add.throughput_mops << " M/s\n";

        // MATCH-HEAVY
        generator.reset(seed + 2);
        auto match_ops = generator.generate_match_heavy(count);
        auto d_match = PipelineBenchmarkRunner::run_direct("MATCH-HEAVY", match_ops);
        auto p_match = PipelineBenchmarkRunner::run_pipeline("MATCH-HEAVY", match_ops);
        results.push_back({d_match, p_match});
        std::cout << "  [MATCH-HEAVY] Direct: " << std::fixed << std::setprecision(2) << d_match.throughput_mops
                  << " M/s  -->  Pipeline: " << p_match.throughput_mops << " M/s\n";

        // CANCEL-HEAVY
        generator.reset(seed + 3);
        auto cancel_ops = generator.generate_cancel_heavy(count);
        auto d_cancel = PipelineBenchmarkRunner::run_direct("CANCEL-HEAVY", cancel_ops);
        auto p_cancel = PipelineBenchmarkRunner::run_pipeline("CANCEL-HEAVY", cancel_ops);
        results.push_back({d_cancel, p_cancel});
        std::cout << "  [CANCEL-HEAVY]Direct: " << std::fixed << std::setprecision(2) << d_cancel.throughput_mops
                  << " M/s  -->  Pipeline: " << p_cancel.throughput_mops << " M/s\n";

        // MIXED
        generator.reset(seed + 4);
        auto mixed_ops = generator.generate_mixed(count);
        auto d_mixed = PipelineBenchmarkRunner::run_direct("MIXED", mixed_ops);
        auto p_mixed = PipelineBenchmarkRunner::run_pipeline("MIXED", mixed_ops);
        results.push_back({d_mixed, p_mixed});
        std::cout << "  [MIXED]       Direct: " << std::fixed << std::setprecision(2) << d_mixed.throughput_mops
                  << " M/s  -->  Pipeline: " << p_mixed.throughput_mops << " M/s\n\n";
    }

    std::cout << "\n====================================================================================================================================================\n";
    std::cout << " PHASE 5 PIPELINE OVERHEAD COMPARISON: DIRECT ENGINE vs SPSC PIPELINE\n";
    std::cout << " Format: [Direct Engine] / [SPSC Pipeline Producer]\n";
    std::cout << "====================================================================================================================================================\n";
    std::cout << std::left
              << std::setw(14) << "Workload"
              << std::setw(9)  << "Size"
              << std::setw(13) << "p50 (ns)"
              << std::setw(13) << "p95 (ns)"
              << std::setw(13) << "p99 (ns)"
              << std::setw(14) << "p99.9 (ns)"
              << std::setw(20) << "Max Latency"
              << "Throughput (M ops/s)\n";
    std::cout << "----------------------------------------------------------------------------------------------------------------------------------------------------\n";

    for (const auto& [dir_res, pipe_res] : results) {
        print_comparison_row(dir_res, pipe_res);
    }
    std::cout << "====================================================================================================================================================\n";

    return 0;
}
