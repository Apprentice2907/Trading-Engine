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

## Core Engineering

- **Zero-Allocation Hot Path**: Preallocated `OrderPool` and intrusive FIFO order chaining eliminate per-order dynamic heap allocations during core matching, risk checks, and queue transit.
- **Cache-Aligned Data Structures**:
  - `OrderCommand` (64 bytes, `alignas(64)`): Ingress order request fitting one cache line.
  - `ExecutionReport` (64 bytes, `alignas(64)`): Egress execution report fitting one cache line.
  - `MarketEvent` (128 bytes, `alignas(64)`): Normalized market tick fitting two cache lines.
- **Lock-Free SPSC Queues**: Fixed-capacity ring buffers utilizing C++20 acquire-release atomic memory ordering, cached head/tail indexes, and false-sharing prevention (`alignas(64)`).
- **Pre-Trade Risk Engine**: Wire-speed order validation enforcing maximum order quantity, maximum order notional, price bands, and cumulative exposure limits in ~11 ns.
- **Order Gateway**: Translates order commands into atomic limit order book operations, producing monotonic execution reports (`New`, `Trade`, `Cancelled`, `RiskRejected`, `EngineRejected`).
- **Binary Event Logging & Replay**: Compact `.hftlog` (32B records) and `.mktlog` (128B records) binary formats guarded by compile-time `constexpr` IEEE 802.3 CRC32 checksums for bit-exact deterministic replay.
- **Real Market-Data Ingestion**: High-throughput parser for Angel One SmartStream WebSocket 2.0 binary protocol (Mode 1 LTP, Mode 2 Quote, Mode 3 SnapQuote).

---

## Performance Benchmarks

*Hardware: 13th Gen Intel Core i5-13420H @ 2.10 GHz, 16 GB RAM, Windows 11 x64*  
*Compiler: MSVC 19.50 (Visual Studio 2026), C++20, `/O2 /permissive-` Release build*

| Component / Subsystem | Workload Description | Throughput | Mean Latency | p50 | p99 | Hot-Path Allocs |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Pre-Trade Risk Engine** | Single-Order Ingress Validation | **90.22 M checks/s** | 11.1 ns | 5.0 ns | 15.0 ns | **0.00 allocs** |
| **Market Data Decoder** | SmartStream Binary $\to$ `MarketEvent` | **11.95 M pkts/s** | 83.7 ns | 100.0 ns | 300.0 ns | **0.00 allocs** |
| **SPSC Queue Transfer** | 1P / 1C Lock-Free Ring Buffer | **27.17 M ev/s** | 36.8 ns | 20.0 ns | 70.0 ns | **0.00 allocs** |
| **Market Data Transit** | `MarketEvent` (128B) $\to$ Ingress SPSC | **62.55 M ev/s** | 16.0 ns | 10.0 ns | 40.0 ns | **0.00 allocs** |
| **Order Book (Flat)** | Dense Price Spread (10K ops) | **9.53 M ops/s** | 104.9 ns | 200.0 ns | 700.0 ns | **0.00 allocs\*** |
| **Matching Engine (Map)** | Reference Engine (100K mixed ops) | **7.40 M ops/s** | 135.1 ns | 200.0 ns | 800.0 ns | **0.00 allocs\*** |
| **Binary Log Replay** | Raw `.hftlog` / `.mktlog` Stream Parse| **12.05 M ev/s** | 83.0 ns | 70.0 ns | 250.0 ns | **0.00 allocs** |
| **Execution Pipeline** | Ingress $\to$ Risk $\to$ Gateway $\to$ Engine | **1.88 M ops/s** | 533.0 ns | 400.0 ns | 1,500.0 ns | **0.00 allocs\*** |

*\* In reference OrderBook implementations, pool recycling eliminates order allocation; node allocations are confined to std::unordered_map order lookup.*

---

## Verification & Testing

```
==================================================
 TEST SUMMARY: 73 / 73 PASS (0 Failures, ~430 ms)
==================================================
```

The test suite validates:
- **Order Book Mechanics**: Price-time FIFO priority, limit additions, cancellations, modifications, spread invariants.
- **Matching Engine**: Immediate crossing fills, partial fills, multi-level sweeps, resting residual liquidity.
- **Differential Testing**: MapOrderBook reference implementation vs. FlatOrderBook across deterministic pseudo-random workloads.
- **Lock-Free Concurrency**: 2,000,000-event concurrent 1P/1C SPSC ring buffer stress test, backpressure drop tracking.
- **Binary Replay Fidelity**: Bit-exact state reproduction, corrupted payload rejection, invalid magic/version detection.
- **Wire Protocols**: Little-endian byte conversion, token parsing, price/timestamp extraction for Angel One packets.
- **Pre-Trade Risk**: Price band enforcement, maximum quantity/notional limits, exposure tracking, rejection emission.
- **Execution Pipeline**: Threaded order gateway lifecycle, duplicate ID rejection, zero-drop dual-SPSC transport.

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
./build/Release/replay_benchmark
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
