# Low-Latency C++ Trading Engine

A C++20 low-latency limit order book and matching engine exploring data structures, memory allocation, lock-free event pipelines, binary event recording, and deterministic replay.

---

## Current Architecture

### Real-Time Event Pipeline
```
Event Source
    ↓
OrderEvent (32B)
    ↓
SPSC Queue (Lock-Free)
    ↓
Matching Engine (Single-Threaded)
    ↓
Trade Events
```

### Event Logging & Deterministic Replay
```
Event Source
    ↓
Binary Recorder
    ↓
.hftlog (CRC32, 64B Header)
    ↓
Replayer
    ↓
Matching Engine
```

---

## Features

- **Price-Time Priority**: Deterministic FIFO execution with discrete tick pricing (`int64_t`).
- **Core Order Operations**: Full support for `LIMIT`, `CANCEL`, and `MODIFY` order types.
- **Execution Engine**: Partial fills, multi-level sweeps, and passive resting price execution.
- **Preallocated OrderPool**: Contiguous pool allocator eliminating per-order dynamic heap allocations.
- **Intrusive FIFO Queues**: Index-based intrusive order chaining per price level.
- **Price-Level Implementations**:
  - `MapOrderBook`: Balanced tree (`std::map`) reference implementation.
  - `FlatOrderBook`: Contiguous sorted array with binary search for cache-friendly access.
- **Bounded SPSC Lock-Free Queue**: Fixed-capacity ring buffer with C++20 acquire-release memory ordering, index caching, and cache-line separation (`alignas(64)`).
- **Binary Event Recording**: Compact `.hftlog` binary logging with 64-byte cache-aligned file header.
- **Deterministic Replay**: Bit-exact reproducing of trade streams and order book states from disk.
- **CRC32 Integrity Validation**: Compile-time `constexpr` IEEE 802.3 checksums guarding file headers and payload data.

---

## Engineering Work

- **Allocation Reduction**: Replaced dynamic order allocation with a preallocated `OrderPool`, achieving 0 allocations per event during core matching and binary recording/replay.
- **Cache Locality Investigation**: Evaluated `std::map` node allocation against contiguous `FlatOrderBook` storage, measuring throughput and tail latency across varying price spreads.
- **SPSC Pipeline**: Built a cache-aligned lock-free queue, measuring the throughput impact of false-sharing elimination and quantifying the cross-thread pipeline boundary cost.
- **Binary Replay**: Designed a compact 32-byte event record format with buffered I/O, achieving multi-million event/second recording and replay rates.
- **Differential Testing**: Maintained the baseline reference implementation to run continuous differential tests against optimized variants.
- **Deterministic Benchmarks**: Validated reproducibility across identical random seeds and fixed-size microbenchmarks.

---

## Verification

```
46 / 46 tests passing
```

The test suite covers order book invariants, matching logic, intrusive pool recycling, differential equivalence (baseline vs. optimized, map vs. flat), concurrent SPSC queue stress testing, pipeline determinism, binary format corruption rejection, and end-to-end replay fidelity.

---

## Representative Performance

> **Note**: The following measurements were collected on a single development machine (13th Gen Intel Core i5-13420H, Windows 11, MSVC 19.44 `/O2` Release). They reflect local empirical observations for comparing algorithmic and architectural trade-offs, not universal hardware-independent guarantees.

| Category | Component / Workload | Throughput | Avg Latency | Dynamic Allocs |
| :--- | :--- | :--- | :--- | :--- |
| **Matching Engine** | Core Matching (Mixed Workload) | ~7.23 M events/sec | ~138 ns / event | 0 allocs/event* |
| **Price Levels** | FlatOrderBook (Dense Spread, 100K) | ~11.83 M events/sec | ~84 ns / event | 0 allocs/event* |
| **SPSC Queue** | Concurrent 1P / 1C (Capacity 16K) | ~48.02 M events/sec | ~20.8 ns / event | 0 allocs/event |
| **Binary Logging** | Disk Recording (Buffered + CRC32, 10M) | ~9.76 M events/sec | ~102.5 ns / event | 0 allocs/event |
| **Binary Replay** | Raw Event Parsing (10M Events) | ~16.13 M events/sec | ~62.0 ns / event | 0 allocs/event |
| **Full Replay** | Replay $\to$ Matching Engine (10M) | ~3.74 M events/sec | ~267.7 ns / event | 0 allocs/event* |

*\* Excluding unordered_map node allocations for order ID lookup where applicable.*

For detailed technical analysis, memory profiles, cache-locality measurements, and architectural history, see [`docs/design.md`](docs/design.md).

---

## Building and Running

### Prerequisites
- C++20 compliant compiler (MSVC 19.30+, GCC 11+, or Clang 13+)
- CMake 3.20+

### Build
```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build --config Release
```

### Run Tests
```bash
# Run the automated test suite
./build/Release/hft_tests
```

### Run Log Tool
```bash
# Record synthetic events to binary log
./build/Release/hft_log_tool record events.hftlog 100000

# Verify file integrity and CRC32 checksums
./build/Release/hft_log_tool verify events.hftlog

# Replay events through the matching engine
./build/Release/hft_log_tool replay events.hftlog
```
