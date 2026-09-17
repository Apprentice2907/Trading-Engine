# Low-Latency C++ Trading Engine

A C++20 low-latency electronic trading engine with a deterministic limit order book, lock-free event pipelines, real-time market-data ingestion, pre-trade risk checks, binary event recording/replay, and simulated order execution.

---

## Architecture Overview

The system strictly decouples inbound real-time market-data observation from order matching and simulated execution:

```
                         LIVE MARKET DATA (READ ONLY)
                                       │
                                       ▼
                     Market Data Provider (Yahoo / Mock / Replay)
                                       │
                                       ▼
                                 Feed Handler
                                       │
                                       ▼
                             MarketEvent (128B)
                                       │
                                       ▼
                                SPSC Lock-Free
                                       │
                                       ▼
                               Market Processing
                                       │
                          ┌────────────┴────────────┐
                          │                         │
                          ▼                         ▼
                       Recording                 Processing
                          │
                          ▼
                       .mktlog
                          │
                          ▼
                        Replay


                         SIMULATED ORDER EXECUTION PATH
                                  Order Source
                                       │
                                       ▼
                              OrderCommand (64B)
                                       │
                                       ▼
                                Ingress SPSC Queue
                                       │
                                       ▼
                                 Pre-Trade Risk
                                       │
                                       ▼
                                 Order Gateway
                                       │
                                       ▼
                      MATCHING ENGINE (SIMULATED EXCHANGE)
                                       │
                                       ▼
                            ExecutionReport (64B)
                                       │
                                       ▼
                                Egress SPSC Queue
                                       │
                                       ▼
                               Execution Consumer
```

> [!IMPORTANT]
> **Strict Separation of Concerns:**
> - **LIVE MARKET DATA = READ ONLY**: The Yahoo Finance market-data adapter serves exclusively as a read-only observation feed. It makes **zero** order-placement, order-modification, or live trading API calls. No API keys or credentials are required.
> - **MATCHING ENGINE = SIMULATED EXCHANGE**: The Limit Order Book and Matching Engine execute in-memory against deterministic local books. It is not an order-routing gateway to a live brokerage or exchange.

---

## Repository Structure

The codebase is organized into ~25 purposeful files with no fragmented abstractions or code bloat:

```text
Stocks/
├── .github/workflows/
│   ├── ci.yml                  # 2x2 Matrix CI: Windows (MSVC) + Ubuntu Linux (GCC 13)
│   └── fuzz.yml                # Clang libFuzzer mutation campaign (ASan + UBSan)
├── CMakeLists.txt              # Unified C++20 build configuration (with optional -DHFT_ENABLE_FUZZING)
├── README.md                   # System design, benchmarks, and documentation
│
├── include/hft/
│   ├── types.hpp               # Numerical primitives, Price/Quantity types, core domain enums
│   ├── order.hpp               # Order, Trade, OrderCommand (64B), and ExecutionReport (64B)
│   ├── order_book.hpp          # LimitOrderBook + embedded contiguous OrderPool
│   ├── flat_order_book.hpp     # Cache-friendly vector LOB + FlatMatchingEngine comparison
│   ├── matching_engine.hpp     # Deterministic single-threaded Matching Engine (MapMatchingEngine)
│   ├── spsc_queue.hpp          # Lock-free cacheline-padded SPSC ring buffer
│   ├── market_data.hpp         # MarketEvent (128B), CRC32, .mktlog format, IMarketDataSource, Yahoo parser & mock feed
│   └── trading_pipeline.hpp    # PreTradeRiskEngine, OrderGateway, and OrderExecutionPipeline
│
├── src/
│   ├── order_book.cpp          # Map-based LOB implementation + OrderPool recycling
│   ├── flat_order_book.cpp     # Dense vector order book + FlatMatchingEngine
│   ├── matching_engine.cpp     # Top-level matching engine & order routing
│   ├── market_data.cpp         # Mock feed, replay source, .mktlog recorder & replayer
│   ├── yahoo_market_data.cpp   # Yahoo Finance HTTP/JSON market-data adapter and zero-alloc parser
│   └── trading_pipeline.cpp    # Risk validation, gateway report emission, threaded execution loop
│
├── tests/
│   ├── test_framework.hpp      # Zero-dependency header-only test harness
│   ├── main.cpp                # Test runner entry point
│   ├── test_engine.cpp         # 26 tests: OrderBook, MatchingEngine, OrderPool, FlatBook differential
│   ├── test_market_data.cpp    # 21 tests: Yahoo parser, Mock feed, SPSC drops, .mktlog CRC32 corruption tests
│   ├── test_pipeline.cpp       # 26 tests: 2M SPSC stress test, risk rules, gateway lifecycle, equivalence
│   ├── test_alloc.cpp          # 8 tests: Zero-allocation regression tests across all hot paths
│   ├── fuzz_decoder.cpp        # libFuzzer target for YahooParser (Clang -DHFT_ENABLE_FUZZING=ON)
│   └── corpus/                 # Seed corpus generator and adversarial inputs for fuzz testing
│
├── examples/
│   ├── basic_simulation.cpp    # Step-by-step console demonstration of matching and order book invariants
│   ├── hft_market_data.cpp     # CLI tool for streaming, recording, and replaying real/mock market data
│   └── hft_pipeline_demo.cpp   # End-to-end multi-threaded market data & order execution demonstration
│
└── benchmarks/
    ├── bench_timer.hpp                 # Serialized invariant TSC hardware timer + LatencySampler
    ├── benchmark.cpp                   # Comprehensive matching engine benchmark & alloc tracker
    ├── price_level_benchmark.cpp       # Price-level lookup comparison (std::map vs. alternatives)
    ├── queue_benchmark.cpp             # SPSC lock-free queue throughput vs. std::mutex queue
    ├── market_data_benchmark.cpp       # Market data parsing and .mktlog serialization throughput
    └── execution_pipeline_benchmark.cpp# End-to-end threaded pipeline throughput and latency profiling
```

---

## Core Engineering

- **Zero-Allocation Hot Path**: Preallocated `OrderPool` and intrusive FIFO order chaining eliminate per-order dynamic heap allocations during core matching, cancellations, risk checks, and queue transit.
- **Cache-Aligned Data Structures**:
  - `OrderCommand` (64 bytes, `alignas(64)`): Ingress order request fitting one cache line.
  - `ExecutionReport` (64 bytes, `alignas(64)`): Egress execution report fitting one cache line.
  - `MarketEvent` (128 bytes, `alignas(64)`): Normalized market tick fitting two cache lines.
- **Lock-Free SPSC Queues**: Fixed-capacity ring buffers utilizing C++20 acquire-release atomic memory ordering, cached head/tail indexes, and false-sharing prevention (`alignas(64)`).
- **Pre-Trade Risk Engine**: Wire-speed order validation enforcing maximum order quantity, maximum order notional, price bands, and cumulative exposure limits in ~5 ns.
- **Order Gateway**: Translates order commands into atomic limit order book operations, producing monotonic execution reports (`New`, `Trade`, `Cancelled`, `RiskRejected`, `EngineRejected`).
- **Binary Event Logging & Replay**: Compact `.hftlog` (32B records) and `.mktlog` (128B records) binary formats guarded by compile-time `constexpr` IEEE 802.3 CRC32 checksums for bit-exact deterministic replay.
- **Real Market-Data Ingestion**: High-throughput zero-allocation parser for Yahoo Finance JSON market data and synthetic tick feeds with strict non-fabrication of quote depth and malformed input resilience.

---

## Performance Benchmarks

### Benchmark Methodology

- **Hardware & Environment**: 13th Gen Intel Core i5-13420H @ 2.10 GHz, 16 GB RAM, Windows 11 x64.
- **Compiler**: MSVC 19.50 (Visual Studio 2026), C++20, `/O2 /permissive-` Release build.
- **Invariant Hardware Timing**: Release builds were benchmarked using invariant hardware TSC timing via serialized `_mm_lfence() + __rdtsc()` instruction pairs.
- **Empirical Calibration**: The TSC was empirically calibrated against the CPU timer at approximately 2.611 GHz on the benchmark machine.
- **Throughput Measurement**: Throughput is measured from the complete uninstrumented bulk workload using wall-clock time (`total_ops / wall_clock_seconds`).
- **Latency Measurement**: Latency distributions are measured separately using TSC samples.
- **Fixed-Size Sampling Window**: For large workloads (such as 1,000,000 operations), latency sampling uses a fixed-size sample window (e.g. 100,000 samples) rather than instrumenting every single operation, keeping measurement probe overhead bounded and comparable across scales.
- **Single-Source Distribution**: Percentiles (`p50`, `p95`, `p99`, `p99.9`, `max`) and `mean` are calculated from the exact same sampled latency array after sorting.
- **OS Scheduling Noise**: Maximum latency can contain normal OS scheduling/interruption noise and should not be interpreted as steady-state engine latency.
- **Environment Dependency**: Results are machine- and run-dependent and are not claims about production exchange performance.

---

### 1. Order Book & Matching Engine (`hft_benchmark.exe`)

*Comparing Reference `MatchingEngine` (std::map) vs `FlatMatchingEngine` (Contiguous Sorted Vector)*  
*Format: [Map] / [Flat] — Derived from the same raw TSC sample array*

| Workload | Scale | p50 | p95 | p99 | p99.9 | Mean | Allocs/Op | Throughput (M ops/s) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ADD-HEAVY** | 10,000 | 110 / 94 ns | 163 / 219 ns | 206 / 330 ns | 4689 / 5169 ns | 121 / 115 ns | 1.13 -> 1.00 | 4.40 -> 8.53 (+93.7%) |
| **MATCH-HEAVY** | 10,000 | 65 / 55 ns | 127 / 121 ns | 170 / 192 ns | 497 / 687 ns | 70 / 64 ns | 0.81 -> 0.50 | 16.58 -> 19.29 (+16.4%) |
| **CANCEL-HEAVY**| 10,000 | 114 / 88 ns | 185 / 124 ns | 266 / 166 ns | 1078 / 417 ns | 126 / 91 ns | 0.98 -> 0.50 | 10.14 -> 12.87 (+26.9%) |
| **MIXED** | 10,000 | 75 / 70 ns | 183 / 173 ns | 247 / 238 ns | 602 / 553 ns | 79 / 75 ns | 0.63 -> 0.40 | 14.53 -> 17.19 (+18.3%) |
| **ADD-HEAVY** | 100,000 | 126 / 244 ns | 249 / 641 ns | 389 / 864 ns | 847 / 4790 ns | 162 / 356 ns | 1.00 -> 1.00 | 6.74 -> 7.21 (+7.0%) |
| **MATCH-HEAVY** | 100,000 | 92 / 59 ns | 298 / 143 ns | 496 / 213 ns | 883 / 513 ns | 129 / 74 ns | 0.81 -> 0.50 | 9.91 -> 17.83 (+80.0%) |
| **CANCEL-HEAVY**| 100,000 | 117 / 101 ns | 186 / 176 ns | 274 / 314 ns | 576 / 569 ns | 129 / 117 ns | 0.96 -> 0.50 | 9.34 -> 11.08 (+18.7%) |
| **MIXED** | 100,000 | 76 / 73 ns | 190 / 205 ns | 275 / 336 ns | 546 / 624 ns | 84 / 89 ns | 0.61 -> 0.39 | 12.52 -> 14.55 (+16.2%) |
| **ADD-HEAVY** | 1,000,000 | 153 / 149 ns | 327 / 228 ns | 536 / 490 ns | 965 / 4044 ns | 193 / 185 ns | 1.00 -> 1.00 | 3.81 -> 4.50 (+18.3%) |
| **MATCH-HEAVY** | 1,000,000 | 150 / 140 ns | 253 / 235 ns | 492 / 479 ns | 1155 / 1091 ns | 158 / 148 ns | 0.81 -> 0.50 | 6.63 -> 6.60 (-0.4%) |
| **CANCEL-HEAVY**| 1,000,000 | 149 / 121 ns | 232 / 199 ns | 452 / 424 ns | 847 / 791 ns | 158 / 133 ns | 0.96 -> 0.50 | 6.22 -> 6.66 (+7.1%) |
| **MIXED** | 1,000,000 | 143 / 138 ns | 318 / 296 ns | 510 / 490 ns | 1157 / 1254 ns | 140 / 135 ns | 0.61 -> 0.39 | 7.26 -> 8.05 (+10.9%) |

---

### 2. Market Data Decoder & SPSC Benchmark (`market_data_benchmark.exe`)

*Protocol: Angel One SmartStream Binary | MarketEvent: 128 bytes (2 cache lines)*

#### Hot-Path Allocation Verification
- **Decode + SPSC push/pop (50,000 ops)**: **0 heap allocations** (Verified Zero Dynamic Allocations).

#### Scale: 100,000 Packets
- **Decoder Throughput**: **19.21 M packets/s**
- **Decoder Latency (TSC, per-op samples)**:
  - Mean: **47.0 ns** | p50: **22.0 ns** | p95: **127.0 ns** | p99: **189.0 ns** | p99.9: **480.0 ns** | Max: **87790.0 ns**
- **SPSC Push Throughput**: **80.34 M events/s**
- **SPSC Push+Pop Latency**:
  - Mean: **8.5 ns** | p50: **9.0 ns** | p95: **9.0 ns** | p99: **9.0 ns** | p99.9: **17.0 ns** | Max: **265.0 ns**
- **Binary Logging (.mktlog)**:
  - Record: **3.58 M ev/s**
  - Replay: **18.93 M ev/s**

#### Scale: 1,000,000 Packets
- **Decoder Throughput**: **19.25 M packets/s**
- **Decoder Latency (TSC, 100,000-sample window)**:
  - Mean: **44.7 ns** | p50: **21.0 ns** | p95: **127.0 ns** | p99: **202.0 ns** | p99.9: **485.0 ns** | Max: **27505.0 ns**
- **SPSC Push Throughput**: **88.61 M events/s**
- **SPSC Push+Pop Latency**:
  - Mean: **8.2 ns** | p50: **8.0 ns** | p95: **9.0 ns** | p99: **9.0 ns** | p99.9: **16.0 ns** | Max: **12694.0 ns**
- **Binary Logging (.mktlog)**:
  - Record: **3.14 M ev/s**
  - Replay: **28.37 M ev/s**

> [!NOTE]
> **Fixed-Window Sampling Disclosure**: The 1,000,000-packet decoder benchmark intentionally reports latency from a fixed 100,000-sample measurement window rather than instrumenting all 1,000,000 operations. Throughput measures the complete uninstrumented workload, while latency sampling uses a fixed-size sample window to keep measurement overhead bounded and comparable across scales. 1,000,000 latency samples were not collected.

---

### 3. SPSC Lock-Free Queue Benchmark (`queue_benchmark.exe`)

*Concurrent 1 Producer / 1 Consumer Transfer (2,000,000 Events)*

#### Allocation Verification
- **Operations Tested**: 200,000 (100k push + 100k pop)
- **Dynamic Allocations**: **0** | **Dynamic Deallocations**: **0** (0.00 allocs/event).

#### Throughput & Amortized Period by Capacity
| Queue Type | Capacity | Throughput (M ev/s) | Amortized Period (ns) |
| :--- | :--- | :--- | :--- |
| **SPSC (Cache-Aligned)** | 256 | **40.61 M ev/s** | 24.6 ns |
| **SPSC (Unaligned)** | 256 | **31.80 M ev/s** | 31.4 ns |
| **std::mutex + queue** | 256 | **15.00 M ev/s** | 66.7 ns |
| **SPSC (Cache-Aligned)** | 1,024 | **41.12 M ev/s** | 24.3 ns |
| **SPSC (Unaligned)** | 1,024 | **26.18 M ev/s** | 38.2 ns |
| **std::mutex + queue** | 1,024 | **15.47 M ev/s** | 64.7 ns |
| **SPSC (Cache-Aligned)** | 4,096 | **36.96 M ev/s** | 27.1 ns |
| **SPSC (Unaligned)** | 4,096 | **24.58 M ev/s** | 40.7 ns |
| **std::mutex + queue** | 4,096 | **13.69 M ev/s** | 73.0 ns |
| **SPSC (Cache-Aligned)** | 16,384 | **46.01 M ev/s** | 21.7 ns |
| **SPSC (Unaligned)** | 16,384 | **28.92 M ev/s** | 34.6 ns |
| **std::mutex + queue** | 16,384 | **13.42 M ev/s** | 74.5 ns |

#### CPU Thread Affinity Experiment (Capacity 4,096, 2M Events)
- **Unpinned Scheduling**: **40.45 M events/s**
- **Pinned Scheduling**: **42.15 M events/s**
- **Observed Affinity Impact**: **+4.2%** *(Note: This is an observed result from this benchmark run, not a universal guarantee across all environments).*

---

### 4. End-to-End Execution Pipeline Benchmark (`execution_pipeline_benchmark.exe`)

*Pipeline: OrderCommand (64B) -> PreTradeRisk -> OrderGateway -> MatchingEngine -> ExecutionReport (64B)*

#### Dynamic Heap Allocation Audit Across Pipeline Stages
- **[1. Pre-Trade Risk Engine Hot Path]**: **0 allocations** (0.000 allocs/check) -> **Verified Zero Heap Allocations**
- **[2. Risk-Rejected Orders (Gateway -> ExecutionReport)]**: **0 allocations** (0.000 allocs/order) -> **Verified Zero Heap Allocations**
- **[3. SPSC Queue Ingress & Egress Transfers]**: **0 allocations** (0.000 allocs/op) -> **Verified Zero Heap Allocations**
- **[4. Order Cancellation Hot Path]**: **0 allocations** (0.000 allocs/cancel) -> **Verified Zero Heap Allocations**
- **[5. Aggressive Crossing Matches (Immediate Fills)]**: **2 total allocations**
- **[6. Reference Engine Book Insertion (order_lookup_)]**: **50,100 allocations / 50,000 orders** = **1.002 allocs/order** *(due to `std::unordered_map` node allocation in the reference OrderBook)*

> [!WARNING]
> The entire MatchingEngine pipeline is **not universally zero-allocation**: while risk checking, risk rejection, SPSC queue transport, and order cancellations are strictly 100% zero-allocation, resting limit order insertions in the reference OrderBook allocate `std::unordered_map` bucket nodes (1 node per resting order).

#### Scale: 100,000 Orders / Events
- **Benchmark A (Pre-Trade Risk Check Only)**: **215.29 M checks/s** (4.6 ns/check amortized)
- **Benchmark B (Resting Orders: Risk + Gateway + Matching)**: **5.91 M orders/s** (169.2 ns/order amortized)
- **Benchmark C (Match-Heavy Execution: Crossing Orders)**: **10.96 M orders/s** (91.2 ns/order amortized, 50,000 trades)
- **Benchmark D (Mixed Workload: 60% Adds, 25% Cancels, 15% Crosses)**: **9.55 M ops/s** (104.7 ns/op amortized)
- **Benchmark E (TSC-Sampled Latency Distribution, 100,000 samples)**:
  - Mean: **123.1 ns** | p50: **64.0 ns** | p95: **196.0 ns** | p99: **350.0 ns** | p99.9: **826.0 ns** | Max: **381725.0 ns**

#### Scale: 1,000,000 Orders / Events
- **Benchmark A (Pre-Trade Risk Check Only)**: **227.50 M checks/s** (4.4 ns/check amortized)
- **Benchmark B (Resting Orders: Risk + Gateway + Matching)**: **3.83 M orders/s** (261.4 ns/order amortized)
- **Benchmark C (Match-Heavy Execution: Crossing Orders)**: **4.67 M orders/s** (214.1 ns/order amortized)
- **Benchmark D (Mixed Workload: 60% Adds, 25% Cancels, 15% Crosses)**: **3.72 M ops/s** (269.0 ns/op amortized)

---

## Verification & Testing

```
==================================================
 TEST SUMMARY: 83 / 83 PASS (0 Failures, ~460 ms)
==================================================
```

The test suite validates:
- **Order Book Mechanics**: Price-time FIFO priority, limit additions, cancellations, modifications, spread invariants.
- **Matching Engine**: Immediate crossing fills, partial fills, multi-level sweeps, resting residual liquidity.
- **Differential Testing**: MapOrderBook reference implementation vs. FlatOrderBook across deterministic pseudo-random workloads.
- **Lock-Free Concurrency**: 2,000,000-event concurrent 1P/1C SPSC ring buffer stress test, backpressure drop tracking.
- **Binary Replay Fidelity**: Bit-exact state reproduction, corrupted payload rejection, invalid magic/version detection.
- **Wire Protocols & Parsers**: JSON parsing, token hashing, price/timestamp extraction, malformed input rejection, and libFuzzer mutation testing.
- **Pre-Trade Risk**: Price band enforcement, maximum quantity/notional limits, exposure tracking, rejection emission.
- **Execution Pipeline**: Threaded order gateway lifecycle, duplicate ID rejection, zero-drop dual-SPSC transport.
- **Zero-Allocation Regression Suite**: Verification that cancellations, crossing fills, risk checks, and queue operations incur 0 dynamic heap allocations, and layout/alignment assertions match cache lines.

---

## Cross-Platform Support & CI

A GitHub Actions CI workflow runs on every push and pull request across a 2×2 matrix:
- **Windows** (MSVC, `windows-latest`): Debug and Release. Full platform support.
- **Linux** (GCC 13, `ubuntu-latest`): Debug and Release. Full platform support with 100% feature parity.

---

## Building and Running

### Prerequisites
- C++20 compliant compiler (MSVC 19.30+, GCC 11+, or Clang 13+)
- CMake 3.20+

### Build (Release Mode)
```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build all targets
cmake --build build --config Release
```

### Run Tests
```bash
./build/Release/hft_tests
```

### Optional: Decoder Fuzz Testing (Clang + libFuzzer)
```bash
# Configure with fuzzing enabled (requires Clang)
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DHFT_ENABLE_FUZZING=ON
cmake --build build-fuzz --target fuzz_decoder

# Run fuzzer with adversarial seed corpus
./build-fuzz/fuzz_decoder tests/corpus/ -max_len=1024 -runs=1000000
```

### Run Unified Pipeline Demo
```bash
# 1. Deterministic Execution Pipeline Walkthrough (Risk -> Resting -> Fills -> Cancels)
./build/Release/hft_pipeline_demo --execution

# 2. Integrated Market Data Ingestion + Simulated Execution Demo (Offline Mock)
./build/Release/hft_pipeline_demo --integrated --mock --seconds 3

# 3. Integrated Market Data Ingestion + Simulated Execution Demo (Live Yahoo Finance)
./build/Release/hft_pipeline_demo --integrated --yahoo AAPL --seconds 5
```

### Run Market Data Tool
```bash
# Stream live market data from Yahoo Finance
./build/Release/hft_market_data live --yahoo AAPL --seconds 5

# Stream offline synthetic mock ticks
./build/Release/hft_market_data live --mock 3045 --seconds 3

# Record market ticks to binary log
./build/Release/hft_market_data record session.mktlog --mock 50000

# Replay recorded log offline with CRC32 verification
./build/Release/hft_market_data replay session.mktlog
```

### Run Microbenchmarks
```bash
./build/Release/hft_benchmark
./build/Release/price_level_benchmark
./build/Release/queue_benchmark
./build/Release/market_data_benchmark
./build/Release/execution_pipeline_benchmark
```

---

## Security & Safety

- **No Real-Money Trading**: The project does not contain any trading strategies, order-placement endpoints, or automated broker execution.
- **Zero Credentials Required**: Live market data uses public read-only Yahoo Finance endpoints. No API keys, passwords, or broker credentials are stored or needed.
- **Binary Log Sanitization**: Binary log records (`.hftlog`, `.mktlog`) store strictly normalized market events and order identifiers; zero authentication data is ever written to disk.

---

## License

MIT License. See [LICENSE](LICENSE) for details.

