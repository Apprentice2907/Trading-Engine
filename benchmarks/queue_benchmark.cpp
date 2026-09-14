#include "hft/order.hpp"
#include "hft/spsc_queue.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

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
// 2. Reference Baseline: Mutex-guarded Bounded Queue
// ============================================================================

template <typename T>
class MutexQueue {
public:
    explicit MutexQueue(size_t capacity) : capacity_(capacity) {}

    bool try_push(const T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (q_.size() >= capacity_) {
            return false;
        }
        q_.push(item);
        return true;
    }

    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (q_.empty()) {
            return false;
        }
        item = q_.front();
        q_.pop();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return q_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return q_.size();
    }

private:
    size_t capacity_;
    mutable std::mutex mtx_;
    std::queue<T> q_;
};

// ============================================================================
// 3. Step 12: Allocation Verification
// ============================================================================

void run_allocation_verification() {
    std::cout << "===================================================================================\n";
    std::cout << " STEP 12: ALLOCATION VERIFICATION TEST\n";
    std::cout << " Verifying zero dynamic heap allocations during normal queue push/pop\n";
    std::cout << "===================================================================================\n";

    constexpr size_t N = 100000;
    hft::SpscQueue<hft::OrderEvent, 1024, true> queue;

    g_alloc_stats.reset();
    g_track_allocations = true;

    for (size_t i = 0; i < N; ++i) {
        hft::OrderEvent in = hft::OrderEvent::make_add(i + 1, hft::Side::Buy, 10000, 10);
        queue.try_push(in);
        hft::OrderEvent out{};
        queue.try_pop(out);
    }

    g_track_allocations = false;

    std::cout << "  Operations tested : " << (N * 2) << " (100k push + 100k pop)\n";
    std::cout << "  Dynamic Allocs    : " << g_alloc_stats.alloc_count << "\n";
    std::cout << "  Dynamic Deallocs  : " << g_alloc_stats.dealloc_count << "\n";
    std::cout << "  Allocs per Event  : " << (static_cast<double>(g_alloc_stats.alloc_count) / N) << " allocs/event\n";

    if (g_alloc_stats.alloc_count == 0 && g_alloc_stats.dealloc_count == 0) {
        std::cout << "  >>> RESULT: VERIFIED ZERO DYNAMIC ALLOCATIONS (0.00 allocs/event) <<<\n";
    } else {
        std::cout << "  >>> RESULT: ALLOCATION OCCURRED IN QUEUE HOT PATH! <<<\n";
    }
    std::cout << "===================================================================================\n\n";
}

// ============================================================================
// 4. Step 6 & 11: Queue Microbenchmark (SPSC vs Mutex, Aligned vs Unaligned)
// ============================================================================

struct QueueBenchResult {
    std::string queue_name;
    size_t capacity{0};
    uint64_t events_transferred{0};
    double elapsed_sec{0.0};
    double throughput_mevents_per_sec{0.0};
    double avg_latency_ns{0.0};
};

template <typename QueueT>
QueueBenchResult benchmark_queue_1p1c(const std::string& name, size_t capacity, uint64_t total_events,
                                     bool pin_threads = false) {
    QueueT queue(capacity);
    std::atomic<bool> start_flag{false};
    std::atomic<bool> producer_done{false};

    auto producer_fn = [&]() {
#ifdef _WIN32
        if (pin_threads) {
            SetThreadAffinityMask(GetCurrentThread(), 1ULL << 0); // Core 0
        }
#endif
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (uint64_t i = 1; i <= total_events; ++i) {
            hft::OrderEvent ev = hft::OrderEvent::make_add(i, hft::Side::Buy, 10000, 10);
            while (!queue.try_push(ev)) {
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#else
                std::this_thread::yield();
#endif
            }
        }
        producer_done.store(true, std::memory_order_release);
    };

    auto consumer_fn = [&]() {
#ifdef _WIN32
        if (pin_threads) {
            SetThreadAffinityMask(GetCurrentThread(), 1ULL << 2); // Core 2
        }
#endif
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t received = 0;
        hft::OrderEvent ev{};
        while (received < total_events) {
            if (queue.try_pop(ev)) {
                ++received;
            } else {
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#else
                std::this_thread::yield();
#endif
            }
        }
    };

    std::thread prod_th(producer_fn);
    std::thread cons_th(consumer_fn);

    auto t0 = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    cons_th.join();
    prod_th.join();
    auto t1 = std::chrono::steady_clock::now();

    std::chrono::duration<double> diff = t1 - t0;
    double sec = diff.count();

    QueueBenchResult res;
    res.queue_name = name;
    res.capacity = capacity;
    res.events_transferred = total_events;
    res.elapsed_sec = sec;
    res.throughput_mevents_per_sec = (static_cast<double>(total_events) / sec) / 1e6;
    res.avg_latency_ns = (sec / static_cast<double>(total_events)) * 1e9;
    return res;
}

// Wrapper for templated SPSC queue to match benchmark signature
template <size_t Cap, bool Aligned>
struct SpscQueueWrapper {
    explicit SpscQueueWrapper(size_t) {}
    hft::SpscQueue<hft::OrderEvent, Cap, Aligned> q;
    bool try_push(const hft::OrderEvent& ev) { return q.try_push(ev); }
    bool try_pop(hft::OrderEvent& ev) { return q.try_pop(ev); }
};

void run_queue_microbenchmark() {
    std::cout << "===================================================================================\n";
    std::cout << " STEP 6 & 11: QUEUE MICROBENCHMARK (1 Producer / 1 Consumer Concurrent Transfer)\n";
    std::cout << " Testing: SPSC (Cache-Aligned) vs SPSC (Unaligned / False Sharing) vs Mutex+Queue\n";
    std::cout << " Capacities: 256, 1024, 4096, 16384 | 2 Million Events Transferred\n";
    std::cout << "===================================================================================\n";

    constexpr uint64_t Events = 2000000;

    std::cout << std::left
              << std::setw(24) << "Queue Type"
              << std::setw(11) << "Capacity"
              << std::setw(16) << "Elapsed (ms)"
              << std::setw(24) << "Throughput (M ev/s)"
              << "Avg Latency (ns)\n";
    std::cout << "-----------------------------------------------------------------------------------\n";

    auto test_capacity = [&](auto cap_tag) {
        constexpr size_t C = decltype(cap_tag)::value;

        // 1. SPSC Cache-Aligned (alignas(64))
        auto r_aligned = benchmark_queue_1p1c<SpscQueueWrapper<C, true>>("SPSC (Cache-Aligned)", C, Events, false);
        std::cout << std::left
                  << std::setw(24) << r_aligned.queue_name
                  << std::setw(11) << r_aligned.capacity
                  << std::setw(16) << std::fixed << std::setprecision(2) << (r_aligned.elapsed_sec * 1000.0)
                  << std::setw(24) << std::setprecision(2) << r_aligned.throughput_mevents_per_sec
                  << std::setprecision(1) << r_aligned.avg_latency_ns << " ns\n";

        // 2. SPSC Unaligned (Testing false sharing)
        auto r_unaligned = benchmark_queue_1p1c<SpscQueueWrapper<C, false>>("SPSC (Unaligned)", C, Events, false);
        std::cout << std::left
                  << std::setw(24) << r_unaligned.queue_name
                  << std::setw(11) << r_unaligned.capacity
                  << std::setw(16) << std::fixed << std::setprecision(2) << (r_unaligned.elapsed_sec * 1000.0)
                  << std::setw(24) << std::setprecision(2) << r_unaligned.throughput_mevents_per_sec
                  << std::setprecision(1) << r_unaligned.avg_latency_ns << " ns\n";

        // 3. Mutex + std::queue
        auto r_mutex = benchmark_queue_1p1c<MutexQueue<hft::OrderEvent>>("std::mutex + queue", C, Events, false);
        std::cout << std::left
                  << std::setw(24) << r_mutex.queue_name
                  << std::setw(11) << r_mutex.capacity
                  << std::setw(16) << std::fixed << std::setprecision(2) << (r_mutex.elapsed_sec * 1000.0)
                  << std::setw(24) << std::setprecision(2) << r_mutex.throughput_mevents_per_sec
                  << std::setprecision(1) << r_mutex.avg_latency_ns << " ns\n";

        std::cout << "-----------------------------------------------------------------------------------\n";
    };

    test_capacity(std::integral_constant<size_t, 256>{});
    test_capacity(std::integral_constant<size_t, 1024>{});
    test_capacity(std::integral_constant<size_t, 4096>{});
    test_capacity(std::integral_constant<size_t, 16384>{});

    // Step 10: CPU Pinning Experiment
    std::cout << "\n>>> STEP 10: CPU THREAD AFFINITY EXPERIMENT (Capacity 4096, 2M events) <<<\n";
    auto r_unpinned = benchmark_queue_1p1c<SpscQueueWrapper<4096, true>>("SPSC (OS Default)", 4096, Events, false);
    auto r_pinned = benchmark_queue_1p1c<SpscQueueWrapper<4096, true>>("SPSC (CPU Pinned)", 4096, Events, true);

    std::cout << "  Unpinned Scheduling : " << std::fixed << std::setprecision(2)
              << r_unpinned.throughput_mevents_per_sec << " M events/sec ("
              << std::setprecision(1) << r_unpinned.avg_latency_ns << " ns/event)\n";
    std::cout << "  Pinned Scheduling   : " << std::fixed << std::setprecision(2)
              << r_pinned.throughput_mevents_per_sec << " M events/sec ("
              << std::setprecision(1) << r_pinned.avg_latency_ns << " ns/event)\n";

    const double pin_speedup = ((r_pinned.throughput_mevents_per_sec - r_unpinned.throughput_mevents_per_sec)
                               / r_unpinned.throughput_mevents_per_sec) * 100.0;
    std::cout << "  Affinity Impact     : " << (pin_speedup >= 0 ? "+" : "")
              << std::setprecision(1) << pin_speedup << "%\n";
    std::cout << "===================================================================================\n\n";
}

int main() {
    run_allocation_verification();
    run_queue_microbenchmark();
    return 0;
}
