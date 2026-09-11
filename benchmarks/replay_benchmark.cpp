#include "hft/event.hpp"
#include "hft/binary_format.hpp"
#include "hft/event_recorder.hpp"
#include "hft/event_replayer.hpp"
#include "hft/matching_engine.hpp"
#include "hft/spsc_queue.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <filesystem>
#include <thread>
#include <atomic>
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
// 2. Deterministic Event Generator
// ============================================================================

std::vector<hft::OrderEvent> generate_benchmark_events(size_t count, uint64_t seed = 0x12345678ULL) {
    std::vector<hft::OrderEvent> events;
    events.reserve(count);
    uint64_t state = seed;
    auto next_u64 = [&state]() -> uint64_t {
        state = state * 6364136223846793005ULL + 1ULL;
        return state >> 32;
    };
    auto next_range = [&](uint64_t min_v, uint64_t max_v) -> uint64_t {
        return min_v + (next_u64() % (max_v - min_v + 1));
    };

    hft::OrderId next_id = 1;
    std::vector<hft::OrderId> active_ids;

    for (size_t i = 0; i < count; ++i) {
        const uint64_t roll = next_range(1, 100);
        if (roll <= 60 || active_ids.empty()) {
            hft::OrderId id = next_id++;
            hft::Side side = (next_u64() % 2 == 0) ? hft::Side::Buy : hft::Side::Sell;
            hft::Price price = static_cast<hft::Price>(next_range(9950, 10050));
            hft::Quantity qty = static_cast<hft::Quantity>(next_range(10, 100));
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
            hft::Price new_price = static_cast<hft::Price>(next_range(9960, 10040));
            hft::Quantity new_qty = static_cast<hft::Quantity>(next_range(10, 80));
            events.push_back(hft::OrderEvent::make_modify(id, new_price, new_qty));
        }
    }
    return events;
}

// ============================================================================
// 3. Benchmarks Execution
// ============================================================================

int main(int argc, char* argv[]) {
    using Clock = std::chrono::steady_clock;

    bool include_10m = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--include-10m") {
            include_10m = true;
        }
    }

    std::cout << "=======================================================================================================\n";
    std::cout << " LOW-LATENCY C++ EXCHANGE ENGINE: PHASE 6 BINARY RECORDING & REPLAY BENCHMARK\n";
    std::cout << " File Format: .hftlog (64B Cache-Aligned Header, IEEE 802.3 CRC32, 32B OrderEvent)\n";
    std::cout << "=======================================================================================================\n\n";

    // 1. Allocation Verification
    std::cout << ">>> STEP 15: ALLOCATION VERIFICATION TEST <<<\n";
    {
        const std::string alloc_file = "benchmark_alloc_test.hftlog";
        constexpr size_t AllocTestEvents = 50000;
        auto sample_events = generate_benchmark_events(AllocTestEvents);

        // Record Allocation Check
        hft::EventRecorder recorder;
        recorder.open(alloc_file);
        g_alloc_stats.reset();
        g_track_allocations = true;
        for (const auto& ev : sample_events) {
            recorder.write(ev);
        }
        g_track_allocations = false;
        const auto rec_allocs = g_alloc_stats.alloc_count;
        recorder.close();

        // Replay Allocation Check
        hft::EventReplayer replayer;
        replayer.open(alloc_file);
        hft::OrderEvent dummy{};
        g_alloc_stats.reset();
        g_track_allocations = true;
        while (replayer.next(dummy)) {}
        g_track_allocations = false;
        const auto rep_allocs = g_alloc_stats.alloc_count;
        replayer.close();

        std::filesystem::remove(alloc_file);

        std::cout << "  Recording Allocs/Event : " << (static_cast<double>(rec_allocs) / AllocTestEvents) << " allocs/event\n";
        std::cout << "  Replay Allocs/Event    : " << (static_cast<double>(rep_allocs) / AllocTestEvents) << " allocs/event\n";
        if (rec_allocs == 0 && rep_allocs == 0) {
            std::cout << "  >>> RESULT: VERIFIED ZERO DYNAMIC ALLOCATIONS (0.00 allocs/event) <<<\n\n";
        }
    }

    std::vector<size_t> test_scales = {100000, 1000000};
    if (include_10m) {
        test_scales.push_back(10000000);
    }

    for (size_t count : test_scales) {
        std::cout << "=======================================================================================================\n";
        std::cout << " BENCHMARKING SCALE: " << count << " EVENTS\n";
        std::cout << "=======================================================================================================\n";

        const std::string log_file = "benchmark_run_" + std::to_string(count) + ".hftlog";

        // Generate events
        auto gen_t0 = Clock::now();
        auto events = generate_benchmark_events(count, 0x12345678ULL);
        auto gen_t1 = Clock::now();
        (void)gen_t0;
        (void)gen_t1;

        // --------------------------------------------------------------------
        // A. Recording Benchmark (Step 10)
        // --------------------------------------------------------------------
        hft::EventRecorder recorder;
        recorder.open(log_file);

        auto rec_t0 = Clock::now();
        for (const auto& ev : events) {
            recorder.write(ev);
        }
        recorder.close();
        auto rec_t1 = Clock::now();

        double rec_sec = std::chrono::duration<double>(rec_t1 - rec_t0).count();
        double rec_mops = (static_cast<double>(count) / rec_sec) / 1e6;
        double rec_ns_per_ev = (rec_sec / static_cast<double>(count)) * 1e9;
        uint64_t file_bytes = std::filesystem::file_size(log_file);
        double bytes_per_ev = static_cast<double>(file_bytes) / static_cast<double>(count);

        std::cout << "  [RECORDING BENCHMARK]\n";
        std::cout << "    Throughput       : " << std::fixed << std::setprecision(2) << rec_mops << " M events/sec\n";
        std::cout << "    Latency          : " << std::fixed << std::setprecision(1) << rec_ns_per_ev << " ns/event\n";
        std::cout << "    File Size        : " << (file_bytes / (1024 * 1024)) << " MB (" << file_bytes << " bytes)\n";
        std::cout << "    Density          : " << std::setprecision(4) << bytes_per_ev << " bytes/event (Theoretical: 32.0 B/ev)\n";
        std::cout << "    Header Overhead  : 64 bytes (" << std::setprecision(5) << (64.0 / static_cast<double>(file_bytes) * 100.0) << "%)\n\n";

        // --------------------------------------------------------------------
        // B. Raw Replay Benchmark (Step 11 - 1)
        // --------------------------------------------------------------------
        hft::EventReplayer replayer;
        replayer.open(log_file);

        volatile uint64_t sink = 0;
        hft::OrderEvent ev{};

        auto raw_t0 = Clock::now();
        while (replayer.next(ev)) {
            sink += ev.id;
        }
        auto raw_t1 = Clock::now();
        replayer.close();

        double raw_sec = std::chrono::duration<double>(raw_t1 - raw_t0).count();
        double raw_mops = (static_cast<double>(count) / raw_sec) / 1e6;
        double raw_ns_per_ev = (raw_sec / static_cast<double>(count)) * 1e9;

        std::cout << "  [REPLAY BENCHMARK: RAW FILE READ + PARSING]\n";
        std::cout << "    Throughput       : " << std::fixed << std::setprecision(2) << raw_mops << " M events/sec\n";
        std::cout << "    Latency          : " << std::fixed << std::setprecision(1) << raw_ns_per_ev << " ns/event\n\n";

        // --------------------------------------------------------------------
        // C. Replay -> SPSC Queue (Step 11 - 2)
        // --------------------------------------------------------------------
        replayer.open(log_file);
        auto queue = std::make_unique<hft::SpscQueue<hft::OrderEvent, 65536, true>>();
        std::atomic<bool> reader_done{false};

        auto spsc_t0 = Clock::now();
        std::thread consumer([&]() {
            hft::OrderEvent q_ev{};
            uint64_t received = 0;
            while (received < count) {
                if (queue->try_pop(q_ev)) {
                    ++received;
                } else {
                    std::this_thread::yield();
                }
            }
        });

        while (replayer.next(ev)) {
            while (!queue->try_push(ev)) {
                std::this_thread::yield();
            }
        }
        replayer.close();
        consumer.join();
        auto spsc_t1 = Clock::now();

        double spsc_sec = std::chrono::duration<double>(spsc_t1 - spsc_t0).count();
        double spsc_mops = (static_cast<double>(count) / spsc_sec) / 1e6;
        double spsc_ns = (spsc_sec / static_cast<double>(count)) * 1e9;

        std::cout << "  [REPLAY BENCHMARK: REPLAY -> SPSC QUEUE]\n";
        std::cout << "    Throughput       : " << std::fixed << std::setprecision(2) << spsc_mops << " M events/sec\n";
        std::cout << "    Latency          : " << std::fixed << std::setprecision(1) << spsc_ns << " ns/event\n\n";

        // --------------------------------------------------------------------
        // D. Replay -> MatchingEngine (Step 11 - 3)
        // --------------------------------------------------------------------
        replayer.open(log_file);
        hft::MatchingEngine engine;
        engine.reserve(count);
        std::vector<hft::Trade> trades;
        trades.reserve(128);

        auto eng_t0 = Clock::now();
        while (replayer.next(ev)) {
            trades.clear();
            switch (ev.type) {
                case hft::EventType::Add:
                    engine.submit_limit_order(ev.id, ev.side, ev.price, ev.qty, trades);
                    break;
                case hft::EventType::Cancel:
                    engine.cancel_order(ev.id);
                    break;
                case hft::EventType::Modify:
                    engine.modify_order(ev.id, ev.price, ev.qty, trades);
                    break;
            }
        }
        auto eng_t1 = Clock::now();
        replayer.close();

        double eng_sec = std::chrono::duration<double>(eng_t1 - eng_t0).count();
        double eng_mops = (static_cast<double>(count) / eng_sec) / 1e6;
        double eng_ns = (eng_sec / static_cast<double>(count)) * 1e9;

        std::cout << "  [REPLAY BENCHMARK: REPLAY -> MATCHING ENGINE]\n";
        std::cout << "    Throughput       : " << std::fixed << std::setprecision(2) << eng_mops << " M events/sec\n";
        std::cout << "    Latency          : " << std::fixed << std::setprecision(1) << eng_ns << " ns/event\n";
        std::cout << "    Trades Generated : " << engine.total_trades_generated() << "\n\n";

        // Clean up temporary benchmark file
        std::filesystem::remove(log_file);
    }

    std::cout << "=======================================================================================================\n";
    std::cout << " PHASE 6 BENCHMARK SUITE COMPLETE\n";
    std::cout << "=======================================================================================================\n";

    return 0;
}
