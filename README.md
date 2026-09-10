# Low-Latency C++ Exchange & Trading Engine

An experimental low-latency C++ exchange and trading engine focused on order-book design, deterministic matching, and performance engineering.

> **Note**: This is a learning and portfolio systems project inspired by electronic exchange architectures (e.g. CME, NASDAQ, LSE). It is **not** an institutional HFT system, does not claim unmeasured "nanosecond" performance, and is not production trading infrastructure. Future optimizations will strictly follow a **Correctness &rarr; Baseline &rarr; Measurement &rarr; Optimization** workflow.

---

## Current Status: Phase 0 & Phase 1 Complete

- **Phase 0 (Foundation)**: Clean C++20 project structure, CMake build setup, strong compiler warnings, and git repository integration.
- **Phase 1 (Baseline Matching Engine)**: Single-threaded limit order book supporting standard price-time priority (FIFO), limit orders, cancellations, priority-preserving/losing modifications, and full/partial fills with bit-exact deterministic replay.

---

## Key Architectural Decisions

1. **Fixed-Point Integer Prices**:
   Floating-point types (`float`, `double`) are avoided entirely in the matching core. Exact discrete tick pricing (`Price = int64_t`) eliminates IEEE-754 rounding inaccuracies, equality ambiguities, and cross-platform non-determinism.
2. **Price-Time Priority (FIFO)**:
   Resting orders at the same price level are executed in strict arrival sequence. Aggressor trades execute at the price of the resting passive order.
3. **Explicit Invariant Verification**:
   The engine enforces 10 formal order book invariants (e.g. uncrossed book, strict FIFO preservation, order quantity conservation, and 1-to-1 lookup table synchronization).
4. **Baseline Control Data Structures**:
   The initial implementation intentionally uses standard C++ containers (`std::map`, `std::list`, `std::unordered_map`) to establish a verified correct control baseline against which future low-latency container designs (e.g., flat contiguous arrays, ring buffers, intrusive lists, cache-conscious pools) will be benchmarked.

---

## Project Structure

```
.
├── CMakeLists.txt              # Top-level build configuration (C++20, MSVC / GCC / Clang)
├── README.md                   # Project overview & architectural baseline
├── .gitignore                  # Git ignore rules for build & IDE artifacts
├── docs/
│   └── design.md               # Detailed market microstructure & design specification
├── include/
│   └── hft/
│       ├── types.hpp           # Fixed-point Price, Quantity, OrderId, strongly-typed enums
│       ├── order.hpp           # Order struct, Trade execution event struct
│       ├── order_book.hpp      # LimitOrderBook interface & invariant verification
│       └── matching_engine.hpp # MatchingEngine processing submit, cancel, modify
├── src/
│   ├── order_book.cpp          # LOB execution, level pruning, matching logic
│   └── matching_engine.cpp     # MatchingEngine sequence tracking & trade dispatch
├── tests/
│   ├── test_framework.hpp      # Lightweight, zero-dependency C++20 test runner
│   ├── main.cpp                # Test runner entry point
│   ├── test_order_book.cpp     # Order book unit tests (add, cancel, modify, invariants)
│   ├── test_matching_engine.cpp# Matching engine execution & fill tests
│   └── test_determinism.cpp    # Deterministic replay with identical event sequences
└── examples/
    └── basic_simulation.cpp    # CLI demonstration of order book depth & trades
```

---

## Building and Running

### Prerequisites
- C++20 compliant compiler (MSVC 19.30+, GCC 11+, or Clang 13+)
- CMake 3.20+
- (Optional) Ninja

### Build Instructions

```bash
# Configure the project
cmake -S . -B build

# Build in Release configuration
cmake --build build --config Release
```

### Run Automated Tests

```bash
# Run the test executable directly
./build/Release/hft_tests

# Or via CTest
ctest --test-dir build -C Release --output-on-failure
```

### Run the Simulation CLI

```bash
./build/Release/hft_sim
```

---

## Roadmap

- **Phase 0 & 1** (Complete): Foundation, baseline limit order book, deterministic matching engine, and test harness.
- **Phase 2**: Microbenchmarking harness, latency measurement (TSC / `std::chrono`), memory allocation profiling, and cache-locality analysis.
- **Phase 3**: Custom low-latency data structures (intrusive double-linked lists, flat ring-buffers, memory pools).
- **Phase 4**: Market event journal, zero-allocation deterministic replay engine.
- **Phase 5**: Real-world external market data adapter & gateway integration.
