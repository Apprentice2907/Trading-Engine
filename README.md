# Low-Latency C++ Trading Engine

A C++20 low-latency electronic trading engine with a deterministic limit order book, lock-free event pipelines, real-time market-data ingestion, pre-trade risk checks, binary event recording/replay, and simulated order execution.

---

## Architecture Overview

The system strictly decouples inbound real-time market-data observation from order matching and simulated execution:

```
                         LIVE MARKET DATA (READ ONLY)
                                       │
                                       ▼
                             Angel One SmartStream
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
> - **LIVE MARKET DATA = READ ONLY**: The Angel One SmartAPI integration serves exclusively as a live market-data observation feed. It makes **zero** order-placement, order-modification, or live trading API calls. Credentials are read strictly from environment variables.
> - **MATCHING ENGINE = SIMULATED EXCHANGE**: The Limit Order Book and Matching Engine execute in-memory against deterministic local books. It is not an order-routing gateway to a live brokerage or exchange.

---

## Repository Structure

The codebase is organized into ~25 purposeful files with no fragmented abstractions or code bloat:

```text
Stocks/
├── .github/workflows/
│   └── ci.yml                  # 2x2 Matrix CI: Windows (MSVC) + Ubuntu Linux (GCC 13)
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
│   ├── market_data.hpp         # MarketEvent (128B), CRC32, .mktlog format, Angel decoder & feed
│   └── trading_pipeline.hpp    # PreTradeRiskEngine, OrderGateway, and OrderExecutionPipeline
│
├── src/
│   ├── order_book.cpp          # Map-based LOB implementation + OrderPool recycling
│   ├── flat_order_book.cpp     # Dense vector order book + FlatMatchingEngine
│   ├── matching_engine.cpp     # Top-level matching engine & order routing
│   ├── market_data.cpp         # SmartStream packet decoding, mock feed, .mktlog recorder & replayer
│   ├── angel_client.cpp        # WinHTTP WebSocket transport for live broker data (WIN32 guarded)
│   └── trading_pipeline.cpp    # Risk validation, gateway report emission, threaded execution loop
│
├── tests/
│   ├── test_framework.hpp      # Zero-dependency header-only test harness
│   ├── main.cpp                # Test runner entry point
│   ├── test_engine.cpp         # 26 tests: OrderBook, MatchingEngine, OrderPool, FlatBook differential
│   ├── test_market_data.cpp    # 21 tests: Wire decoders, SPSC drops, .mktlog CRC32 corruption tests
│   ├── test_pipeline.cpp       # 26 tests: 2M SPSC stress test, risk rules, gateway lifecycle, equivalence
│   ├── test_alloc.cpp          # 8 tests: Zero-allocation regression tests across all hot paths
│   ├── fuzz_decoder.cpp        # libFuzzer target for AngelDecoder (Clang -DHFT_ENABLE_FUZZING=ON)
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
    ├── market_data_benchmark.cpp       # Wire packet decoding and .mktlog serialization throughput
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
- **Real Market-Data Ingestion**: High-throughput parser for Angel One SmartStream WebSocket 2.0 binary protocol (Mode 1 LTP, Mode 2 Quote, Mode 3 SnapQuote) with integer overflow and malformed packet bounds checks.

---

## Performance Benchmarks

*Hardware: 13th Gen Intel Core i5-13420H @ 2.10 GHz, 16 GB RAM, Windows 11 x64*  
*Compiler: MSVC 19.50 (Visual Studio 2026), C++20, `/O2 /permissive-` Release build*  
*Timing Methodology: Per-operation latencies are sampled directly with hardware timestamp counters using `_mm_lfence() + __rdtsc()` serialized probes, calibrated against a high-resolution timer. Both mean and percentiles (`p50`, `p95`, `p99`, `max`) are derived from the exact same raw sample array after sorting. Throughput (M ops/sec) is measured independently in uninstrumented bulk execution loops to eliminate probe overhead.*

| Component / Subsystem | Workload Description | Throughput | Mean Latency | p50 | p99 | Hot-Path Allocs |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Pre-Trade Risk Engine** | Single-Order Ingress Validation (100K) | **200.84 M checks/s** | 5.0 ns | 5.0 ns | 15.0 ns | **0.00 allocs** |
| **Market Data Decoder** | SmartStream Binary $\to$ `MarketEvent` (1M) | **11.72 M pkts/s** | 49.4 ns | 36.0 ns | 183.0 ns | **0.00 allocs** |
| **SPSC Queue Transfer** | 1P / 1C Lock-Free Ring Buffer (Push+Pop) | **33.11 M ev/s** | 25.5 ns | 26.0 ns | 29.0 ns | **0.00 allocs** |
| **Market Data Transit** | `MarketEvent` (128B) $\to$ Ingress SPSC | **69.22 M ev/s** | 14.4 ns | 14.0 ns | 27.0 ns | **0.00 allocs** |
| **Order Book (Flat)** | Dense Price Spread (100K Mixed ops) | **7.25 M ops/s** | 179.0 ns | 143.0 ns | 627.0 ns | **0.00 allocs\*** |
| **Matching Engine (Map)** | Reference Engine (100K Mixed ops) | **6.92 M ops/s** | 223.0 ns | 196.0 ns | 696.0 ns | **0.00 allocs\*** |
| **Binary Log Replay** | Raw `.hftlog` / `.mktlog` Stream Parse | **10.82 M ev/s** | 92.4 ns | 80.0 ns | 220.0 ns | **0.00 allocs** |
| **Execution Pipeline** | Ingress $\to$ Risk $\to$ Gateway $\to$ Engine | **4.13 M ops/s** | 364.8 ns | 235.0 ns | 903.0 ns | **0.00 allocs\*** |

*\* In OrderBook implementations, preallocated `OrderPool` recycling eliminates order heap allocation. The aggressive match, order cancellation, pre-trade risk validation, and SPSC ring buffers are verified 100% zero-allocation hot paths. In reference books, node allocations for resting limit orders are bounded strictly to std::unordered_map order lookup.*

---

## Verification & Testing

```
==================================================
 TEST SUMMARY: 81 / 81 PASS (0 Failures, ~680 ms)
==================================================
```

The test suite validates:
- **Order Book Mechanics**: Price-time FIFO priority, limit additions, cancellations, modifications, spread invariants.
- **Matching Engine**: Immediate crossing fills, partial fills, multi-level sweeps, resting residual liquidity.
- **Differential Testing**: MapOrderBook reference implementation vs. FlatOrderBook across deterministic pseudo-random workloads.
- **Lock-Free Concurrency**: 2,000,000-event concurrent 1P/1C SPSC ring buffer stress test, backpressure drop tracking.
- **Binary Replay Fidelity**: Bit-exact state reproduction, corrupted payload rejection, invalid magic/version detection.
- **Wire Protocols**: Little-endian byte conversion, token parsing, price/timestamp extraction, overflow protection for Angel One packets.
- **Pre-Trade Risk**: Price band enforcement, maximum quantity/notional limits, exposure tracking, rejection emission.
- **Execution Pipeline**: Threaded order gateway lifecycle, duplicate ID rejection, zero-drop dual-SPSC transport.
- **Zero-Allocation Regression Suite**: Verification that cancellations, crossing fills, risk checks, and queue operations incur 0 dynamic heap allocations, and layout/alignment assertions match cache lines.

---

## Cross-Platform Support & CI

A GitHub Actions CI workflow runs on every push and pull request across a 2×2 matrix:
- **Windows** (MSVC, `windows-latest`): Debug and Release. Full platform support including `AngelClient` (WinHTTP WebSocket).
- **Linux** (GCC 13, `ubuntu-latest`): Debug and Release. Full core platform support (order books, matching engines, SPSC queues, pre-trade risk, binary logger/replayer, test suite, and microbenchmarks). Platform-specific broker network client is cleanly guarded behind CMake `WIN32`.

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
./build-fuzz/fuzz_decoder tests/corpus/ -max_len=512 -runs=1000000
```

### Run Unified Pipeline Demo
```bash
# 1. Deterministic Execution Pipeline Walkthrough (Risk -> Resting -> Fills -> Cancels)
./build/Release/hft_pipeline_demo --execution

# 2. Integrated Market Data Ingestion + Simulated Execution Demo (Offline Mock)
./build/Release/hft_pipeline_demo --integrated --mock --seconds 3
```

### Run Market Data Tool
```bash
# Stream live market data (requires ANGEL_* environment variables)
./build/Release/hft_market_data live 3045

# Stream offline synthetic mock ticks
./build/Release/hft_market_data live --mock 3045

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
- **Credential Isolation**: No credentials, API tokens, passwords, or keys are stored in the codebase. All connection parameters must be provided via standard environment variables (`ANGEL_API_KEY`, `ANGEL_CLIENT_CODE`, `ANGEL_FEED_TOKEN`).
- **Binary Log Sanitization**: Binary log records (`.hftlog`, `.mktlog`) store strictly normalized market events and order identifiers; zero authentication data is ever written to disk.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
