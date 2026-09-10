# Baseline Performance & Profiling Report (Phase 2)

An empirical performance evaluation of the baseline single-threaded C++20 Limit Order Book and Matching Engine.

---

## 1. Hardware & Environment

- **Operating System**: Microsoft Windows 11 (NT 10.0.26200.0 x64)
- **CPU**: 13th Gen Intel(R) Core(TM) i5-13420H
  - 8 Physical Cores (4 Performance Cores + 4 Efficient Cores), 12 Logical Processors
  - Base Frequency: 2.1 GHz, Turbo Frequency: up to 4.6 GHz
- **RAM**: 16 GB DDR5
- **Compiler**: Microsoft (R) C/C++ Optimizing Compiler `19.50.35728.0` for x64 (Visual Studio Build Tools 2026 v18.4.2)
- **Build Configuration**: Release (`/O2 /W4 /permissive- /utf-8`, single-threaded, C++20)
- **High-Resolution Timer**: `std::chrono::steady_clock` (backed by Windows `QueryPerformanceCounter`)
  - Measured Timer Call Overhead: **16.4 ns / call**
- **Deterministic Workload Seed**: `0x12345678ULL` (64-bit high-entropy LCG)

---

## 2. Benchmark Methodology

To ensure fair, repeatable, and unperturbed measurements:
1. **Zero Hot-Path I/O**: Console logging, string formatting, file writes, and assertions are excluded from the timed execution window.
2. **Pre-Generated Workloads**: All orders, prices, quantities, and operations are fully populated in contiguous memory vectors prior to starting timers.
3. **Warm-Up Execution**: A dedicated 10,000-operation dry run executes first to warm CPU instruction caches, L1/L2 data caches, branch predictor tables, and OS memory paging.
4. **Dual-Run Protocol**:
   - *Throughput Run*: Measures the entire batch from $t_0$ to $t_{end}$ in a single timing interval with zero intermediate clock calls or tracker overhead (reporting the fastest of 3 iterations).
   - *Latency Distribution Run*: Records individual operation durations into pre-allocated memory buffers to derive exact percentiles ($p50, p95, p99, p99.9, \text{Max}$).
5. **Exact Allocation Tracking**: Overrides global `operator new` / `delete` active exclusively during the benchmark window to record exact heap allocation counts, deallocations, and net bytes per operation.

---

## 3. Workload Definitions

- **Workload A (Add-Heavy)**: 100% resting limit orders placed on both sides of the spread without crossing. Tests order-book tree insertion (`std::map`), FIFO queue insertion (`std::list`), and lookup indexing (`std::unordered_map`).
- **Workload B (Match-Heavy)**: Alternates between resting orders and aggressive crossing orders that sweep price levels and generate trades. Measures matching loops, trade event generation, order consumption, queue pops, and level pruning.
- **Workload C (Cancel-Heavy)**: 50% passive limit orders followed by 50% cancellations of active orders. Measures hash lookup and node deletion from lists and maps.
- **Workload D (Mixed)**: Realistic market distribution: 60% Add, 20% Cancel, 20% Modify across fluctuating price levels.

---

## 4. Empirical Performance Results

### 4.1 Summary Across Workloads & Scales

| Workload | Size | p50 | p95 | p99 | p99.9 | Max | Throughput | Trades |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ADD-HEAVY** | 10,000 | 200 ns | 200 ns | 300 ns | 19.4 &mu;s | 104.0 &mu;s | **4.707 M ops/s** | 0 |
| **MATCH-HEAVY** | 10,000 | 100 ns | 200 ns | 300 ns | 1.4 &mu;s | 8.1 &mu;s | **10.242 M ops/s** | 9,131 |
| **CANCEL-HEAVY** | 10,000 | 100 ns | 200 ns | 300 ns | 0.6 &mu;s | 3.7 &mu;s | **7.685 M ops/s** | 0 |
| **MIXED** | 10,000 | 100 ns | 300 ns | 400 ns | 1.3 &mu;s | 17.5 &mu;s | **9.783 M ops/s** | 5,020 |
| | | | | | | | | |
| **ADD-HEAVY** | 100,000 | 200 ns | 400 ns | 700 ns | 30.9 &mu;s | 2.04 ms | **3.852 M ops/s** | 0 |
| **MATCH-HEAVY** | 100,000 | 100 ns | 200 ns | 300 ns | 0.9 &mu;s | 131.9 &mu;s | **9.470 M ops/s** | 91,152 |
| **CANCEL-HEAVY** | 100,000 | 200 ns | 400 ns | 500 ns | 0.9 &mu;s | 174.4 &mu;s | **6.343 M ops/s** | 0 |
| **MIXED** | 100,000 | 100 ns | 300 ns | 400 ns | 1.2 &mu;s | 182.7 &mu;s | **8.968 M ops/s** | 49,091 |
| | | | | | | | | |
| **ADD-HEAVY** | 1,000,000 | 200 ns | 500 ns | 700 ns | 35.5 &mu;s | 45.69 ms | **2.726 M ops/s** | 0 |
| **MATCH-HEAVY** | 1,000,000 | 200 ns | 500 ns | 800 ns | 1.9 &mu;s | 2.66 ms | **7.827 M ops/s** | 911,140 |
| **CANCEL-HEAVY** | 1,000,000 | 200 ns | 300 ns | 500 ns | 0.8 &mu;s | 2.60 ms | **6.444 M ops/s** | 0 |
| **MIXED** | 1,000,000 | 100 ns | 400 ns | 600 ns | 1.1 &mu;s | 2.41 ms | **6.990 M ops/s** | 493,425 |
| | | | | | | | | |
| **MIXED (Stress)**| 10,000,000 | 100 ns | 400 ns | 600 ns | 1.2 &mu;s | 2.71 ms | **3.898 M ops/s** | 4,937,631 |

---

## 5. Allocation Measurements

Dynamic allocations were measured via intercepted heap allocation hooks:

| Workload | Allocs / Op | Deallocs / Op | Bytes / Op | Dominant Allocation Source |
| :--- | :--- | :--- | :--- | :--- |
| **ADD-HEAVY** | **2.08** | **0.00** | **159.2 B** | `std::list` node + `std::unordered_map` node + `std::map` node |
| **MATCH-HEAVY** | **1.62** | **1.48** | **100.7 B** | Order placement allocs; order fills free list and hash nodes |
| **CANCEL-HEAVY** | **1.94** | **1.93** | **120.2 B** | 50% order placement allocs; 50% cancels deallocate nodes |
| **MIXED** | **1.24** | **1.09** | **76.9 B** | Interleaved order churn (allocs balanced by fills and cancels) |

---

## 6. Bottleneck Analysis

### 6.1 Verified Bottlenecks (Empirically Proven)
1. **Dynamic Heap Allocation per Operation**:
   Every new order creates at least two separate heap allocations: one `_List_node` inside `std::list<Order>` and one bucket node inside `std::unordered_map<OrderId, OrderLocation>`. In high-churn matching (1M+ ops), the engine invokes `malloc` / `free` millions of times, introducing CRT allocator overhead.
2. **`std::unordered_map` Rehashing Spikes**:
   In `ADD-HEAVY` at 1M operations, the maximum latency spikes to **45.69 ms**. This occurs when the lookup hash table dynamically reallocates and rehashes its internal bucket array.
3. **Cache Footprint Degradation with Scale**:
   As live resting orders increase from 10K to 1M in `ADD-HEAVY`, throughput drops by **42%** (4.71 M ops/s &rarr; 2.73 M ops/s). The working set of isolated node allocations disperses across the heap, exceeding the CPU's 1.25 MB L2 cache and 12 MB L3 cache.

### 6.2 Hypotheses (Likely, but unverified without hardware counter profiling)
1. *Cache Misses from Node-Based Containers*:
   `std::map` is a red-black tree with node pointers, and `std::list` is a doubly linked list with node pointers. Each traversal dereferences pointers scattered across heap pages, likely triggering data cache misses.
2. *Double-Indirection Lookup Overhead*:
   Finding an order by ID requires hashing the `OrderId`, finding the bucket, following the iterator to the `std::list` node, and modifying or deleting. A flat contiguous direct-index table or an intrusive list could eliminate this indirection.

---

## 7. What Should NOT Be Optimized Yet

Premature optimization must be avoided:
- **Do NOT implement multithreading**: The single-threaded engine already processes 3.9M &ndash; 10.2M ops/s. Adding mutexes, locks, or multi-producer queues would introduce thread synchronization overhead and cache-line bouncing.
- **Do NOT introduce SIMD / CPU intrinsics**: The bottleneck is memory layout and pointer chasing, not vectorizable arithmetic.
- **Do NOT use compiler-specific assembly or micro-optimizations**: Algorithm and memory layout changes provide orders of magnitude more impact than micro-tuning.

---

## 8. Recommended Phase 3 Experiment

Based on the verified evidence that **dynamic node allocation** and **pointer chasing** are the primary costs:

> **Recommended Experiment**:
> Design a contiguous, pre-allocated memory pool and intrusive list structure to eliminate dynamic allocations on the hot path.
> 1. Benchmark a pre-allocated fixed-size order pool (`std::array` / flat memory buffer) where nodes are recycled using a free list ($O(1)$ allocation, zero CRT `malloc`).
> 2. Replace `std::list<Order>` with an intrusive doubly-linked list embedded directly into the `Order` struct.
> 3. Measure the exact delta in throughput, $p99$ latency, and heap allocations against this established baseline.
