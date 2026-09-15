#pragma once

// bench_timer.hpp — High-resolution TSC benchmark timer with empirical frequency calibration.
//
// On x86/x86-64 with invariant TSC (CPUID 0x80000007 EDX[8]):
//   - Uses __rdtsc() + _mm_lfence() for sub-nanosecond timing overhead.
//   - Calibrates TSC frequency empirically against std::chrono::steady_clock
//     via a 50 ms sleep loop at startup. This avoids assuming the nominal CPU
//     clock speed equals the TSC frequency (they often differ by 1-5%).
//   - In virtualised environments (GitHub Actions, Docker) the TSC may be
//     scaled by the hypervisor; the calibration still produces the correct
//     effective ns_per_tsc for that host.
//
// On other architectures (ARM, non-x86):
//   - Falls back to std::chrono::steady_clock. The mean-from-same-samples
//     guarantee is preserved; only the timer resolution differs.
//
// Usage:
//   BenchTimer timer;
//   timer.calibrate();               // call once at program start
//
//   auto t0 = timer.start();
//   do_work();
//   uint64_t ns = timer.stop_ns(t0); // nanoseconds elapsed
//
//   LatencySampler sampler(N);
//   for (...) {
//       auto t0 = timer.start();
//       do_work();
//       sampler.record(timer.stop_ns(t0));
//   }
//   sampler.finish();  // sorts samples
//   // sampler.mean_ns(), .p50_ns(), .p95_ns(), .p99_ns(), .max_ns()
//   // — all derived from the same raw sample array.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <vector>
#include <chrono>
#include <thread>
#include <cassert>
#include <iostream>
#include <iomanip>

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  define HFT_BENCH_X86 1
#  include <immintrin.h>   // _mm_lfence
#  if defined(_MSC_VER)
#    include <intrin.h>    // __rdtsc on MSVC
#  else
#    include <x86intrin.h> // __rdtsc on GCC/Clang
#  endif
#else
#  define HFT_BENCH_X86 0
#endif

// ---------------------------------------------------------------------------
// BenchTimer
// ---------------------------------------------------------------------------
class BenchTimer {
public:
    // Calibrate TSC frequency by measuring TSC ticks over a known wall-clock
    // interval. Call once before benchmarking.
    void calibrate(int iterations = 3, int sleep_ms = 50) {
#if HFT_BENCH_X86
        double best_ns_per_tsc = 1.0; // fallback: assume 1 GHz
        // Run calibration several times and take the median to reduce noise.
        std::vector<double> results;
        results.reserve(iterations);
        for (int k = 0; k < iterations; ++k) {
            _mm_lfence();
            uint64_t t0_tsc = __rdtsc();
            _mm_lfence();
            auto t0_wall = std::chrono::steady_clock::now();

            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

            _mm_lfence();
            uint64_t t1_tsc = __rdtsc();
            _mm_lfence();
            auto t1_wall = std::chrono::steady_clock::now();

            double elapsed_ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1_wall - t0_wall).count());
            uint64_t elapsed_tsc = t1_tsc - t0_tsc;

            if (elapsed_tsc > 0 && elapsed_ns > 0.0) {
                results.push_back(elapsed_ns / static_cast<double>(elapsed_tsc));
            }
        }
        if (!results.empty()) {
            std::sort(results.begin(), results.end());
            best_ns_per_tsc = results[results.size() / 2]; // median
        }
        ns_per_tsc_ = best_ns_per_tsc;
        tsc_per_ns_ = 1.0 / ns_per_tsc_;
        calibrated_ = true;
#else
        ns_per_tsc_ = 1.0;
        tsc_per_ns_ = 1.0;
        calibrated_ = true;
#endif
    }

    // Returns the measured TSC frequency in GHz (informational).
    [[nodiscard]] double tsc_ghz() const noexcept {
        return tsc_per_ns_;
    }

    // Read the start timestamp. Use before the measured operation.
    // On x86: lfence serialises prior loads before the RDTSC.
    [[nodiscard]] inline uint64_t start() const noexcept {
#if HFT_BENCH_X86
        _mm_lfence();
        return __rdtsc();
#else
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Read the stop timestamp and convert to nanoseconds.
    // On x86: RDTSC then lfence serialises subsequent loads after the RDTSC.
    [[nodiscard]] inline uint64_t stop_ns(uint64_t t0) const noexcept {
#if HFT_BENCH_X86
        uint64_t t1 = __rdtsc();
        _mm_lfence();
        uint64_t cycles = t1 - t0;
        return static_cast<uint64_t>(static_cast<double>(cycles) * ns_per_tsc_);
#else
        uint64_t t1 = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return t1 - t0;
#endif
    }

    [[nodiscard]] bool is_calibrated() const noexcept { return calibrated_; }

private:
    double ns_per_tsc_{1.0};
    double tsc_per_ns_{1.0};
    bool   calibrated_{false};
};

// ---------------------------------------------------------------------------
// LatencySampler — Collects raw nanosecond samples and computes statistics.
//
// Mean, p50, p95, p99, and max are ALL derived from the same raw sample array
// after a single sort. This prevents the mean < p50 inconsistency that arises
// when mean is derived from a bulk-throughput measurement while percentiles
// come from a per-operation instrumented loop.
// ---------------------------------------------------------------------------
class LatencySampler {
public:
    explicit LatencySampler(size_t capacity) {
        samples_.reserve(capacity);
    }

    // Record one nanosecond measurement.
    inline void record(uint64_t ns) {
        samples_.push_back(static_cast<uint32_t>(ns < 0xFFFFFFFFULL ? ns : 0xFFFFFFFF));
    }

    // Sort samples and compute statistics. Call once after all records.
    void finish() {
        if (samples_.empty()) return;
        std::sort(samples_.begin(), samples_.end());

        uint64_t sum = 0;
        for (uint32_t v : samples_) sum += v;
        mean_ns_  = static_cast<double>(sum) / static_cast<double>(samples_.size());

        auto pct = [this](double p) -> double {
            size_t idx = static_cast<size_t>(p * static_cast<double>(samples_.size() - 1));
            return static_cast<double>(samples_[idx]);
        };
        p50_ns_  = pct(0.50);
        p95_ns_  = pct(0.95);
        p99_ns_  = pct(0.99);
        p999_ns_ = pct(0.999);
        max_ns_  = static_cast<double>(samples_.back());
        finished_ = true;
    }

    [[nodiscard]] size_t  count()    const noexcept { return samples_.size(); }
    [[nodiscard]] double  mean_ns()  const noexcept { return mean_ns_;  }
    [[nodiscard]] double  p50_ns()   const noexcept { return p50_ns_;   }
    [[nodiscard]] double  p95_ns()   const noexcept { return p95_ns_;   }
    [[nodiscard]] double  p99_ns()   const noexcept { return p99_ns_;   }
    [[nodiscard]] double  p999_ns()  const noexcept { return p999_ns_;  }
    [[nodiscard]] double  max_ns()   const noexcept { return max_ns_;   }
    [[nodiscard]] bool    finished() const noexcept { return finished_;  }

    void print_summary(const char* label, double throughput_mops = 0.0) const {
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  " << label << "\n";
        if (throughput_mops > 0.0) {
            std::cout << "    Throughput : " << std::setprecision(2) << throughput_mops << " M ops/s\n";
        }
        std::cout << std::setprecision(1);
        std::cout << "    Samples    : " << samples_.size() << "\n"
                  << "    Mean       : " << mean_ns_  << " ns\n"
                  << "    p50        : " << p50_ns_   << " ns\n"
                  << "    p95        : " << p95_ns_   << " ns\n"
                  << "    p99        : " << p99_ns_   << " ns\n"
                  << "    p99.9      : " << p999_ns_  << " ns\n"
                  << "    Max        : " << max_ns_   << " ns\n";
        // Sanity check: mean must not be less than p50 for a non-negative distribution.
        if (mean_ns_ < p50_ns_ * 0.90) {
            std::cout << "    [WARNING] mean (" << mean_ns_ << " ns) < p50 (" << p50_ns_
                      << " ns): check timer resolution or sample count\n";
        }
    }

private:
    std::vector<uint32_t> samples_;
    double mean_ns_{0.0};
    double p50_ns_{0.0};
    double p95_ns_{0.0};
    double p99_ns_{0.0};
    double p999_ns_{0.0};
    double max_ns_{0.0};
    bool   finished_{false};
};
