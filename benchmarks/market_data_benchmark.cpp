#include "hft/market_data.hpp"
#include "hft/spsc_queue.hpp"

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
// 2. Benchmark Utilities
// ============================================================================

using Clock = std::chrono::high_resolution_clock;

struct LatencyStats {
    double p50_ns{0.0};
    double p99_ns{0.0};
    double p999_ns{0.0};
    double max_ns{0.0};
    double mean_ns{0.0};
};

LatencyStats compute_percentiles(std::vector<double>& latencies_ns) {
    if (latencies_ns.empty()) return {};
    std::sort(latencies_ns.begin(), latencies_ns.end());
    size_t n = latencies_ns.size();

    double sum = 0.0;
    for (double val : latencies_ns) sum += val;

    LatencyStats s;
    s.mean_ns = sum / static_cast<double>(n);
    s.p50_ns  = latencies_ns[n * 50 / 100];
    s.p99_ns  = latencies_ns[n * 99 / 100];
    s.p999_ns = latencies_ns[n * 999 / 1000];
    s.max_ns  = latencies_ns.back();
    return s;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    std::cout << "=======================================================================================================\n";
    std::cout << " LOW-LATENCY C++ EXCHANGE ENGINE: PHASE 7 ANGEL ONE MARKET DATA BENCHMARK\n";
    std::cout << " Protocol: SmartStream Binary (Little-Endian) | Event: 128B MarketEvent (2 Cache Lines)\n";
    std::cout << "=======================================================================================================\n\n";

    // ------------------------------------------------------------------------
    // Part 1: Allocation Verification
    // ------------------------------------------------------------------------
    std::cout << ">>> VERIFYING DYNAMIC HEAP ALLOCATIONS ON HOT PATH <<<\n";
    {
        uint8_t packet[AngelConstants::PACKET_SIZE_SNAP_QUOTE]{0};
        MockAngelFeed::build_snap_quote_packet(
            packet, sizeof(packet), "3045", 83050, 100, 83045, 500, 83055, 600, 1, 1710000000000LL, 10000);

        hft::MarketEvent ev{};
        hft::SpscQueue<hft::MarketEvent, 1024> queue;

        // Warmup
        hft::broker::AngelDecoder::decode(packet, sizeof(packet), ev, 12345);
        queue.try_push(ev);
        queue.try_pop(ev);

        g_alloc_stats.reset();
        g_track_allocations = true;

        const size_t test_ops = 50000;
        for (size_t i = 0; i < test_ops; ++i) {
            hft::broker::AngelDecoder::decode(packet, sizeof(packet), ev, i);
            queue.try_push(ev);
            queue.try_pop(ev);
        }

        g_track_allocations = false;
        double allocs_per_op = static_cast<double>(g_alloc_stats.alloc_count) / test_ops;

        std::cout << "  Hot Path Allocs/Op: " << allocs_per_op << " allocs/op ("
                  << g_alloc_stats.alloc_count << " total)\n";
        if (g_alloc_stats.alloc_count == 0) {
            std::cout << "  >>> RESULT: VERIFIED ZERO DYNAMIC ALLOCATIONS (0.00 allocs/event) <<<\n\n";
        } else {
            std::cout << "  >>> WARNING: ALLOCATIONS DETECTED: " << g_alloc_stats.alloc_count << " <<<\n\n";
        }
    }

    const std::vector<size_t> test_scales = {100000, 1000000};

    for (size_t count : test_scales) {
        std::cout << "=======================================================================================================\n";
        std::cout << " BENCHMARKING SCALE: " << count << " PACKETS / EVENTS\n";
        std::cout << "=======================================================================================================\n";

        // Generate synthetic stream of raw packets
        auto packets = hft::broker::MockAngelFeed::generate_synthetic_stream(count, 0xABCDEFULL);

        // --------------------------------------------------------------------
        // Benchmark A: Raw Decode + Normalization
        // --------------------------------------------------------------------
        std::vector<hft::MarketEvent> normalized_events(count);
        std::vector<double> decode_latencies_ns;
        if (count <= 100000) decode_latencies_ns.reserve(count);

        auto dec_t0 = Clock::now();
        for (size_t i = 0; i < count; ++i) {
            if (count <= 100000) {
                auto t_start = Clock::now();
                hft::broker::AngelDecoder::decode(packets[i].data(), packets[i].size(), normalized_events[i], i);
                auto t_end = Clock::now();
                decode_latencies_ns.push_back(std::chrono::duration<double, std::nano>(t_end - t_start).count());
            } else {
                hft::broker::AngelDecoder::decode(packets[i].data(), packets[i].size(), normalized_events[i], i);
            }
        }
        auto dec_t1 = Clock::now();
        double dec_sec = std::chrono::duration<double>(dec_t1 - dec_t0).count();
        double dec_tput = count / dec_sec / 1e6;

        std::cout << "  [BENCHMARK A: DECODE + NORMALIZATION]\n";
        std::cout << "    Throughput  : " << std::fixed << std::setprecision(2) << dec_tput << " M packets/sec\n";
        std::cout << "    Avg Latency : " << std::fixed << std::setprecision(1) << (dec_sec * 1e9 / count) << " ns/packet\n";
        if (!decode_latencies_ns.empty()) {
            auto stats = compute_percentiles(decode_latencies_ns);
            std::cout << "    Latency p50 : " << stats.p50_ns << " ns\n";
            std::cout << "    Latency p99 : " << stats.p99_ns << " ns\n";
            std::cout << "    Latency max : " << stats.max_ns << " ns\n";
        }

        // --------------------------------------------------------------------
        // Benchmark B: MarketEvent -> SPSC Queue (Single-Threaded Burst)
        // --------------------------------------------------------------------
        hft::SpscQueue<hft::MarketEvent, 16384> spsc;
        hft::MarketEvent dummy{};

        auto spsc_t0 = Clock::now();
        for (size_t i = 0; i < count; ++i) {
            while (!spsc.try_push(normalized_events[i])) {
                spsc.try_pop(dummy);
            }
            if (i % 8 == 0) {
                spsc.try_pop(dummy);
            }
        }
        while (spsc.try_pop(dummy)) {}
        auto spsc_t1 = Clock::now();

        double spsc_sec = std::chrono::duration<double>(spsc_t1 - spsc_t0).count();
        double spsc_tput = count / spsc_sec / 1e6;

        std::cout << "\n  [BENCHMARK B: MARKET EVENT -> SPSC QUEUE]\n";
        std::cout << "    Throughput  : " << std::fixed << std::setprecision(2) << spsc_tput << " M events/sec\n";
        std::cout << "    Avg Latency : " << std::fixed << std::setprecision(1) << (spsc_sec * 1e9 / count) << " ns/event\n";

        // --------------------------------------------------------------------
        // Benchmark C: Binary Recording (.mktlog) & Replay -> SPSC
        // --------------------------------------------------------------------
        const std::string mkt_file = "bench_temp_" + std::to_string(count) + ".mktlog";

        // 1. Recording
        hft::MarketEventRecorder recorder;
        recorder.open(mkt_file);
        auto rec_t0 = Clock::now();
        for (const auto& ev : normalized_events) {
            recorder.write(ev);
        }
        recorder.close();
        auto rec_t1 = Clock::now();
        double rec_sec = std::chrono::duration<double>(rec_t1 - rec_t0).count();
        double rec_tput = count / rec_sec / 1e6;
        uintmax_t file_bytes = std::filesystem::file_size(mkt_file);

        std::cout << "\n  [BENCHMARK C.1: .MKTLOG BINARY RECORDING]\n";
        std::cout << "    Throughput  : " << std::fixed << std::setprecision(2) << rec_tput << " M events/sec ("
                  << (file_bytes / (rec_sec * 1024.0 * 1024.0)) << " MB/sec)\n";
        std::cout << "    File Size   : " << (file_bytes / (1024.0 * 1024.0)) << " MB (" << file_bytes << " bytes)\n";
        std::cout << "    Density     : " << (static_cast<double>(file_bytes) / count) << " bytes/event\n";

        // 2. Replay -> SPSC
        hft::MarketEventReplayer replayer;
        replayer.open(mkt_file);
        hft::MarketEvent read_ev{};

        auto rep_t0 = Clock::now();
        while (replayer.next(read_ev)) {
            while (!spsc.try_push(read_ev)) {
                spsc.try_pop(dummy);
            }
            spsc.try_pop(dummy);
        }
        while (spsc.try_pop(dummy)) {}
        auto rep_t1 = Clock::now();
        double rep_sec = std::chrono::duration<double>(rep_t1 - rep_t0).count();
        double rep_tput = count / rep_sec / 1e6;
        replayer.close();

        std::cout << "\n  [BENCHMARK C.2: REPLAY .MKTLOG -> SPSC]\n";
        std::cout << "    Throughput  : " << std::fixed << std::setprecision(2) << rep_tput << " M events/sec\n";
        std::cout << "    Avg Latency : " << std::fixed << std::setprecision(1) << (rep_sec * 1e9 / count) << " ns/event\n";

        std::filesystem::remove(mkt_file);
        std::cout << "\n";
    }

    std::cout << "=======================================================================================================\n";
    std::cout << " PHASE 7 MARKET DATA BENCHMARK COMPLETE\n";
    std::cout << "=======================================================================================================\n";

    return 0;
}
