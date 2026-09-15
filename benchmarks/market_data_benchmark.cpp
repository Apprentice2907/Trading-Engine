// market_data_benchmark.cpp — Angel One decoder and SPSC market-data throughput/latency benchmark.
//
// Methodology:
//   Throughput : uninstrumented bulk loop (total_ops / wall_clock_seconds).
//   Latency    : per-op TSC measurements. Mean, p50, p95, p99, max all
//                derived from the SAME sorted raw sample array.

#include "hft/market_data.hpp"
#include "hft/spsc_queue.hpp"
#include "bench_timer.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <new>

using namespace hft;
using namespace hft::broker;

// ============================================================================
// 1. Memory Allocation Tracker
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
    if (g_track_allocations && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

void operator delete(void* p, size_t) noexcept {
    if (g_track_allocations && p) ++g_alloc_stats.dealloc_count;
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
    if (g_track_allocations && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
    if (g_track_allocations && p) ++g_alloc_stats.dealloc_count;
    std::free(p);
}

// ============================================================================
// 2. Main benchmark
// ============================================================================

int main() {
    BenchTimer timer;
    timer.calibrate(3, 50);

    std::cout << "=======================================================================================================\n";
    std::cout << " LOW-LATENCY C++ — MARKET DATA DECODER & SPSC BENCHMARK\n";
    std::cout << " Protocol: Angel One SmartStream Binary | MarketEvent: 128 bytes (2 cache lines)\n";
    std::cout << "=======================================================================================================\n\n";
    std::cout << "Timer: TSC (empirical calibration) — " << std::fixed << std::setprecision(3)
              << timer.tsc_ghz() << " GHz\n";
    std::cout << "Note:  Throughput = total_ops / wall_clock_seconds (uninstrumented bulk loop)\n";
    std::cout << "       Latency columns (mean/p50/p95/p99/max) all from the same TSC sample array\n\n";

    // -------------------------------------------------------------------------
    // Alloc verification
    // -------------------------------------------------------------------------
    std::cout << ">>> HOT-PATH ALLOCATION VERIFICATION <<<\n";
    {
        constexpr size_t VERIFY_N = 50000;
        uint8_t packet[AngelConstants::PACKET_SIZE_SNAP_QUOTE]{0};
        MockAngelFeed::build_snap_quote_packet(
            packet, sizeof(packet), "3045", 83050, 100, 83045, 500, 83055, 600, 1, 1710000000000LL, 10000);

        hft::MarketEvent ev{};
        hft::SpscQueue<hft::MarketEvent, 1024> queue;

        // Warmup
        for (size_t i = 0; i < 1000; ++i) {
            AngelDecoder::decode(packet, sizeof(packet), ev, i);
            queue.try_push(ev);
            queue.try_pop(ev);
        }

        g_alloc_stats.reset();
        g_track_allocations = true;
        for (size_t i = 0; i < VERIFY_N; ++i) {
            AngelDecoder::decode(packet, sizeof(packet), ev, i);
            queue.try_push(ev);
            queue.try_pop(ev);
        }
        g_track_allocations = false;

        std::cout << "  Decode + SPSC push/pop (" << VERIFY_N << " ops): "
                  << g_alloc_stats.alloc_count << " heap allocations\n";
        if (g_alloc_stats.alloc_count == 0) {
            std::cout << "  RESULT: VERIFIED ZERO DYNAMIC ALLOCATIONS\n\n";
        } else {
            std::cout << "  WARNING: " << g_alloc_stats.alloc_count << " allocations detected\n\n";
        }
    }

    // -------------------------------------------------------------------------
    // Per-scale benchmarks
    // -------------------------------------------------------------------------
    for (size_t count : {size_t{100000}, size_t{1000000}}) {
        std::cout << "=== SCALE: " << count << " packets ===\n";

        auto packets = MockAngelFeed::generate_synthetic_stream(count, 0xABCDEFULL);
        std::vector<hft::MarketEvent> events(count);

        // -- A: Decoder throughput (bulk, uninstrumented) ---------------------
        {
            // Warmup
            for (size_t i = 0; i < std::min(count / 10, size_t{5000}); ++i) {
                AngelDecoder::decode(packets[i].data(), packets[i].size(), events[i], i);
            }

            auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < count; ++i) {
                AngelDecoder::decode(packets[i].data(), packets[i].size(), events[i], i);
            }
            auto t1 = std::chrono::steady_clock::now();
            double sec = std::chrono::duration<double>(t1 - t0).count();
            std::cout << "  [A] Decoder throughput: "
                      << std::fixed << std::setprecision(2) << (count / sec / 1e6) << " M pkts/s\n";
        }

        // -- A: Decoder latency (per-op TSC, same samples for all stats) ------
        {
            const size_t N = std::min(count, size_t{100000});
            LatencySampler sampler(N);

            // Warmup
            for (size_t i = 0; i < std::min(N / 10, size_t{500}); ++i) {
                AngelDecoder::decode(packets[i].data(), packets[i].size(), events[i], i);
            }
            for (size_t i = 0; i < N; ++i) {
                auto t0 = timer.start();
                AngelDecoder::decode(packets[i].data(), packets[i].size(), events[i], i);
                sampler.record(timer.stop_ns(t0));
            }
            sampler.finish();
            sampler.print_summary("Decoder latency (TSC, per-op samples):");
        }

        // -- B: SPSC push+pop throughput & latency ----------------------------
        {
            hft::SpscQueue<hft::MarketEvent, 16384> spsc;
            hft::MarketEvent dummy{};
            const size_t N = std::min(count, size_t{100000});
            LatencySampler sampler(N);

            // Warmup
            for (size_t i = 0; i < std::min(N / 10, size_t{500}); ++i) {
                spsc.try_push(events[i]);
                spsc.try_pop(dummy);
            }

            auto t0_bulk = std::chrono::steady_clock::now();
            for (size_t i = 0; i < count; ++i) {
                while (!spsc.try_push(events[i % count])) {
                    spsc.try_pop(dummy);
                }
                if (i % 8 == 0) spsc.try_pop(dummy);
            }
            while (spsc.try_pop(dummy)) {}
            auto t1_bulk = std::chrono::steady_clock::now();
            double bulk_sec = std::chrono::duration<double>(t1_bulk - t0_bulk).count();
            std::cout << "  [B] SPSC push throughput: "
                      << std::fixed << std::setprecision(2) << (count / bulk_sec / 1e6) << " M ev/s\n";

            for (size_t i = 0; i < N; ++i) {
                auto t0 = timer.start();
                spsc.try_push(events[i]);
                spsc.try_pop(dummy);
                sampler.record(timer.stop_ns(t0));
            }
            sampler.finish();
            sampler.print_summary("SPSC push+pop latency (TSC, per-op samples):");
        }

        // -- C: Binary recorder & replay throughput ---------------------------
        {
            const std::string fname = "bench_mkt_" + std::to_string(count) + ".mktlog";
            hft::MarketEventRecorder recorder;
            recorder.open(fname);

            auto r0 = std::chrono::steady_clock::now();
            for (const auto& ev : events) recorder.write(ev);
            recorder.close();
            auto r1 = std::chrono::steady_clock::now();
            double rec_sec = std::chrono::duration<double>(r1 - r0).count();

            uintmax_t fsz = 0;
            try { fsz = std::filesystem::file_size(fname); } catch (...) {}

            std::cout << "  [C.1] .mktlog record: " << std::fixed << std::setprecision(2)
                      << (count / rec_sec / 1e6) << " M ev/s  (" << (fsz / 1024.0 / 1024.0) << " MB)\n";

            hft::MarketEventReplayer replayer;
            replayer.open(fname);
            hft::SpscQueue<hft::MarketEvent, 16384> spsc;
            hft::MarketEvent rdev{}, dummy{};

            auto p0 = std::chrono::steady_clock::now();
            while (replayer.next(rdev)) {
                while (!spsc.try_push(rdev)) spsc.try_pop(dummy);
                spsc.try_pop(dummy);
            }
            while (spsc.try_pop(dummy)) {}
            auto p1 = std::chrono::steady_clock::now();
            double rep_sec = std::chrono::duration<double>(p1 - p0).count();
            replayer.close();
            std::filesystem::remove(fname);

            std::cout << "  [C.2] .mktlog replay: " << std::fixed << std::setprecision(2)
                      << (count / rep_sec / 1e6) << " M ev/s\n";
        }

        std::cout << "\n";
    }

    std::cout << "=======================================================================================================\n";
    std::cout << " MARKET DATA BENCHMARK COMPLETE\n";
    std::cout << "=======================================================================================================\n";
    return 0;
}
