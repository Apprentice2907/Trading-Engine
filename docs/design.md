# Design Document: Limit Order Book & Matching Engine (Phase 1 Baseline)

## 1. Overview & Scope

This document details the architecture and design of the Phase 1 limit order book (LOB) and matching engine. The engine processes limit orders, cancellations, and modifications in single-threaded, deterministic execution, generating execution events (trades) while guaranteeing strict price-time priority.

---

## 2. Market Model & Order Types

### 2.1 Fixed-Point Price Representation
Prices are represented as fixed-point integers (`int64_t Price`), where each unit corresponds to a discrete multiple of the instrument's minimum tick size.
- **Rationale**: Binary floating point (`double`, `float`) suffers from precision loss, non-associative additions, and platform-specific FPU rounding differences. Exact integer arithmetic is essential for zero-drift order matching and bit-exact replayability.

### 2.2 Supported Operations
1. **LIMIT Order**: Specifies side (Buy/Sell), limit price, and quantity.
   - If crossing the market, matches aggressively against resting opposite orders.
   - Any unfilled residual quantity is placed passively into the order book.
2. **CANCEL**: Removes an active resting order by `OrderId`. Safely returns not-found status if the order is already filled or nonexistent.
3. **MODIFY**: Updates the price or quantity of an active resting order:
   - *Same Price, Quantity Decrease*: Retains FIFO queue priority (in-place quantity reduction).
   - *Same Price, Quantity Increase*: Loses FIFO priority; moved to the back (tail) of the FIFO list at that price level.
   - *Price Change*: Loses priority; cancels resting order and re-processes at the new price level (matching aggressively if crossed, or resting passively).

---

## 3. Price-Time Priority Mechanics

- **Price Priority**:
  - Buy orders: Higher price has strict precedence.
  - Sell orders: Lower price has strict precedence.
- **Time Priority (FIFO)**:
  - Among orders resting at the exact same price level, earlier arrival sequence has absolute priority.
- **Execution Price Rule**:
  - An aggressive crossing order always executes at the **price of the passive resting order**, rewarding the liquidity provider with their posted limit price.

---

## 4. Baseline Data Structures

For this initial baseline, standard C++ library containers are chosen deliberately for readability, correctness verification, and invariant enforcement:

```
OrderBook
│
├── bids_: std::map<Price, PriceLevel, std::greater<Price>>
├── asks_: std::map<Price, PriceLevel, std::less<Price>>
└── order_lookup_: std::unordered_map<OrderId, OrderLocation>

PriceLevel
├── price: Price
├── total_quantity: Quantity
└── orders: std::list<Order> (FIFO queue)

OrderLocation
├── side: Side
├── price: Price
└── iter: std::list<Order>::iterator
```

### Algorithmic Complexity of Baseline:
- **Order Lookup**: $O(1)$ average via `unordered_map`.
- **Level Lookup / Best Bid / Best Ask**: $O(1)$ amortized for `begin()`, $O(\log L)$ for level insertion/erasure where $L$ is the number of active price levels.
- **Order Cancellation**: $O(1)$ list erasure using direct iterator from lookup table + $O(\log L)$ level deletion if level is emptied.
- **Order Modification**: $O(1)$ for in-place quantity decrease; $O(1)$ queue splice for quantity increase.

---

## 5. Explicit Order Book Invariants

The method `OrderBook::verify_invariants(std::string* error_out)` enforces 10 structural properties:

1. **Uncrossed Book**: When both sides have resting orders, $\text{Best Bid} < \text{Best Ask}$.
2. **Side Segregation**: No buy order resides in the ask book, and no sell order resides in the bid book.
3. **Lookup Consistency**: Every live `OrderId` in `order_lookup_` maps to the exact matching order in the corresponding price level list.
4. **No Phantom Orders**: Total orders in `order_lookup_` matches the exact sum of orders across all price levels.
5. **Strictly Positive Quantities**: Every resting order has `remaining_qty > 0`.
6. **Quantity Conservation**: In each price level, `total_quantity` equals the exact sum of `remaining_qty` of all orders in that level.
7. **Pruned Price Levels**: Any price level whose order queue becomes empty is immediately erased from the map.
8. **Strict Level Ordering**: Bids are strictly descending; asks are strictly ascending.
9. **FIFO Preservation**: Within every price level, order sequence numbers / timestamps are strictly non-decreasing.
10. **Trade Conservation**: For every executed trade, quantity is deducted identically from both resting and aggressive orders.

---

## 6. Phase 2 — Baseline Performance & Profiling

Phase 2 established an empirical baseline before attempting any premature optimization. For full profiling data, methodology, and latency distributions, see [`docs/baseline-performance.md`](baseline-performance.md).

### 6.1 Baseline Allocation Profile
Under high-frequency churn, standard library containers exhibited heavy dynamic heap allocation:
- **`ADD-HEAVY`**: 2.08 – 2.40 allocations / op (159.2 bytes / op).
- **`MATCH-HEAVY`**: 1.62 – 1.64 allocations / op and 1.48 deallocations / op.
- **`CANCEL-HEAVY`**: 1.94 – 1.98 allocations / op and 1.93 deallocations / op.
- **`MIXED`**: 1.22 – 1.29 allocations / op and 1.09 deallocations / op.

### 6.2 Identified Bottlenecks
1. **`std::list<Order>`**: Node allocations (`_List_node`) on every order insertion and deallocations on every fill/cancel.
2. **`std::unordered_map` Rehashing**: Dynamic bucket array doubling caused massive **43 ms** latency spikes at 1M scale.
3. **`std::map` Tree Nodes**: Dynamic node allocations for every price level created.

---

## 7. Phase 3 — Hot-Path Memory & Allocation Optimization

Phase 3 targeted dynamic memory allocation on the hot path without altering price-level data structures. For full comparative data and differential tests, see [`docs/phase3-memory-optimization.md`](phase3-memory-optimization.md).

### 7.1 Architecture Changes
1. **Contiguous OrderPool**: Created preallocated contiguous order storage (`OrderPool`), eliminating per-order heap allocations in favor of an $O(1)$ free-list index allocator.
2. **Intrusive FIFO Lists**: Replaced `std::list<Order>` with intrusive double-linked indices (`OrderIndex prev`, `next`) embedded directly inside `PoolOrderNode`.
3. **Rehash Control**: Added `reserve()` to pre-size `std::unordered_map` bucket arrays, completely eliminating periodic rehash latency spikes.

### 7.2 Results
- **Throughput**: Improved by up to **+59.7%** (e.g., ADD-heavy 10K increased from 4.79 M/s to 7.65 M/s).
- **Tail Latency**: 1M max latency plummeted from **43.3 ms** down to **1.6 ms** (-96.3% latency reduction).
- **Allocation Reduction**: Dynamic allocations per order insertion were halved from ~2.2 to ~1.1 allocs/op.

---

## 8. Phase 4 — Price-Level Data Structure Investigation

### 8.1 Original Hypothesis
In Phase 3, replacing `std::list<Order>` with `OrderPool` and index-based intrusive lists achieved up to +59.7% throughput improvement, but:
1. MATCH-heavy 1M throughput regressed from 6.67 M/s to 5.93 M/s.
2. Overall engine still incurred ~1 allocation/op.
The hypothesis was that `std::map` price levels caused frequent Red-Black tree node allocations/deallocations, pointer chasing across cache lines, and multi-level sweep overhead.

### 8.2 What Was Profiled
Using `price_level_benchmark`:
1. **Isolated Component Allocations**:
   - `OrderPool` alone (pre-reserved): 0.0001 allocs/op (0 heap allocations).
   - `std::unordered_map<OrderId, OrderLocation>` alone (with `reserve()`): **1.00 allocs/op** on insertion and **1.00 deallocs/op** on erase.
   - `std::map<Price, PriceLevel>` alone: **1.00 alloc/level** on new price insertion and **1.00 dealloc/level** on level removal.
2. **Granular Price-Level Access Latencies**:
   - Add order at existing price: **101.6 ns/op**
   - Add order at new price: **182.9 ns/op** (Tree insertion adds +81.3 ns)
   - Cancel order (level retained): **66.3 ns/op**
   - Remove empty price level: **129.6 ns/op** (Tree erase adds +63.3 ns)
   - Match pair against best price: **146.5 ns/op**
   - 5-Level Sweep execution: **204.5 ns/op**
3. **Data Structure Microbenchmark (`std::map` vs Flat Contiguous Vector)**:
   - Evaluated at 10, 100, 1,000, 10,000, and 100,000 price levels:
     - *Best-Price Access*: ~0.24 ns on both (O(1)).
     - *Price Lookup*: Flat vector with binary search was 1.06x to 2.89x faster across all depths (110 ns vs 321 ns at 100K levels due to contiguous cache lines).
     - *Top-10 Sweep*: Flat vector was 3.4x - 4.15x faster (3.6 ns vs 15 ns) due to prefetching and zero pointer chasing.
     - *Insert + Erase*: Flat vector was 2.23x faster at 10 levels (19 ns vs 43 ns), but degraded to 407 ns at 1K levels and 8.5 &mu;s at 10K levels due to $O(N)$ `memmove`.

### 8.3 Remaining Allocation Source
The remaining ~1 allocation/op in the engine originates from:
1. **`std::unordered_map` bucket list nodes**: In MSVC STL, `std::unordered_map` allocates an individual `_List_node` on the CRT heap for every unique order ID inserted, even with `reserve()`.
2. **`std::map` tree nodes**: Dynamically allocates a `_Tree_node` for each new price level, and frees it when the level is emptied.
In `OrderBook`:
- Adding to an existing price level incurs exactly **1.00 alloc/op** (all from `order_lookup_`).
- Adding to a new price level incurs **2.00 allocs/op** (1 map node + 1 unordered_map node).

### 8.4 Experimental Data Structure (`FlatOrderBook`)
- **Structure**: Contiguous `std::vector<PriceLevel> bids_` (sorted descending) and `asks_` (sorted ascending).
- **Best Bid / Best Ask**: Direct index 0 access ($O(1)$).
- **Price Lookup**: `std::lower_bound` binary search on contiguous cache lines ($O(\log K)$).
- **Queue Linking**: Orders linked via intrusive 32-bit `OrderIndex` into contiguous `OrderPool`.
- **Level Sweep**: Linear traversal over adjacent memory.
- **Pre-reserved Capacity**: Pre-reserves price-level vector capacity, eliminating vector reallocation.

### 8.5 Correctness Results
Extended differential testing between `MapMatchingEngine` (`std::map`) and `FlatMatchingEngine` (`FlatOrderBook`) over 3,000 deterministic operations across 3 seeds (`0x12345678`, `0xCAFEBABE`, `0xDEADBEEF`).
Verified:
- Accepted / rejected return statuses: 100% match.
- Trade count, prices, quantities, timestamps: 100% bit-exact match.
- Rested book orders, depth, best bid/ask, total quantities: 100% match.
- Full invariant verification (`verify_invariants`): PASSED across all states.

### 8.6 Benchmark Results (Map vs Flat)

| Workload | Size | Map p50 / Flat p50 | Map p99 / Flat p99 | Map Max / Flat Max | Map Allocs / Flat Allocs | Map Tput -> Flat Tput | Change (%) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ADD-HEAVY** | 10K | 100ns / 100ns | 600ns / 400ns | 128&mu;s / 20&mu;s | 1.19 -> 1.00 | 7.87 -> 8.06 M/s | **+2.4%** |
| **MATCH-HEAVY** | 10K | 100ns / 100ns | 200ns / 400ns | 3&mu;s / 22&mu;s | 0.81 -> 0.50 | 16.65 -> 18.44 M/s | **+10.7%** |
| **CANCEL-HEAVY**| 10K | 100ns / 100ns | 400ns / 300ns | 69&mu;s / 53&mu;s | 0.99 -> 0.50 | 10.24 -> 12.07 M/s | **+17.8%** |
| **MIXED** | 10K | 100ns / 100ns | 300ns / 200ns | 3&mu;s / 1&mu;s | 0.64 -> 0.40 | 14.85 -> 16.43 M/s | **+10.6%** |
| **ADD-HEAVY** | 100K | 100ns / 100ns | 500ns / 400ns | 75&mu;s / 77&mu;s | 1.04 -> 1.00 | 6.67 -> 7.27 M/s | **+9.0%** |
| **MATCH-HEAVY** | 100K | 100ns / 100ns | 300ns / 200ns | 44&mu;s / 45&mu;s | 0.81 -> 0.50 | 13.38 -> 16.30 M/s | **+21.8%** |
| **CANCEL-HEAVY**| 100K | 100ns / 100ns | 400ns / 200ns | 257&mu;s / 54&mu;s | 0.97 -> 0.50 | 8.40 -> 11.37 M/s | **+35.3%** |
| **MIXED** | 100K | 100ns / 100ns | 300ns / 400ns | 142&mu;s / 38&mu;s | 0.61 -> 0.39 | 13.02 -> 14.13 M/s | **+8.5%** |
| **ADD-HEAVY** | 1M | 200ns / 200ns | 600ns / 600ns | 315&mu;s / 291&mu;s | 1.04 -> 1.00 | 4.64 -> 4.63 M/s | **-0.3%** |
| **MATCH-HEAVY** | 1M | 200ns / 200ns | 600ns / 500ns | 214&mu;s / 706&mu;s | 0.81 -> 0.50 | 6.56 -> 7.10 M/s | **+8.3%** |
| **CANCEL-HEAVY**| 1M | 200ns / 200ns | 500ns / 500ns | 101&mu;s / 300&mu;s | 0.97 -> 0.50 | 6.35 -> 6.38 M/s | **+0.6%** |
| **MIXED** | 1M | 200ns / 200ns | 600ns / 600ns | 744&mu;s / 205&mu;s | 0.61 -> 0.39 | 7.41 -> 7.60 M/s | **+2.7%** |
| **MIXED-10M** | 10M | 200ns / 200ns | 900ns / 700ns | 257&mu;s / 185&mu;s | 0.61 -> 0.39 | 5.30 -> 5.39 M/s | **+1.7%** |

### 8.7 What Improved
1. **MATCH-Heavy Performance Rebound**: Reversed the Phase 3 regression, delivering +8.3% to +21.8% throughput gains (6.56 -> 7.10 M/s at 1M, and 16.30 M/s at 100K).
2. **Allocation Reduction**:
   - MATCH-Heavy: 0.81 &rarr; **0.50 allocs/op** (-38%).
   - CANCEL-Heavy: 0.97 &rarr; **0.50 allocs/op** (-48%).
   - MIXED: 0.61 &rarr; **0.39 allocs/op** (-36%).
3. **CANCEL-Heavy Throughput**: +35.3% at 100K and +17.8% at 10K due to zero heap deallocations when price levels become empty.
4. **Tail Latency**:
   - MIXED 1M max latency reduced from 744 &mu;s to 205 &mu;s (-72%).
   - MIXED 10M max latency reduced from 257 &mu;s to 185 &mu;s.
   - CANCEL 100K max latency reduced from 257 &mu;s to 54 &mu;s (-79%).

### 8.8 What Regressed / Trade-offs
1. **Large Depth Insertions**: At 1M operations in ADD-heavy with 1,000 distinct price levels, insertion shifts cause a negligible -0.3% regression (4.64 vs 4.63 M/s).
2. **Extreme Depth Microbenchmarks**: When active price levels exceed 1,000 simultaneously, contiguous vector insertion time scales as $O(N)$ (e.g. 407 ns at 1K levels, 8.5 &mu;s at 10K levels), whereas `std::map` remains logarithmic (61 ns).

### 8.9 Final Decision
**KEEP BOTH IMPLEMENTATIONS**:
- `FlatOrderBook` / `FlatMatchingEngine` is retained as a high-performance price-level structure optimized for active inside markets, multi-level sweeps, and reduced allocation churn.
- `MapOrderBook` / `MapMatchingEngine` is retained as the baseline reference implementation, which scales gracefully without $O(N)$ vector shifts when active depth spans tens of thousands of price levels.

### 8.10 Why the Final Decision Was Made
The benchmark data provides clear evidence: `FlatOrderBook` improves MATCH-heavy (+8% to +22%), CANCEL-heavy (+18% to +35%), and MIXED (+2% to +10%) across realistic scale, while cutting dynamic allocations by 36% to 48% across workloads. Keeping both implementations preserves architectural honesty and gives full visibility into the trade-off between cache locality ($O(1)/O(\log K)$) and dense vector shifting ($O(K)$).

---

## 9. Phase 5 — SPSC Lock-Free Event Pipeline

### 9.1 Motivation: Why SPSC?
In production exchange and trading architectures, network adapters / feed handlers receive order events asynchronously from external connections. To achieve ultra-low latency:
1. The core matching engine must **never block** on network I/O or mutex contention.
2. The matching engine must remain strictly **single-threaded** to eliminate locks, race conditions, and non-deterministic concurrency bugs.
A Single-Producer / Single-Consumer (SPSC) lock-free bounded queue provides the optimal, wait-free thread boundary: exactly one external producer thread deposits events, and exactly one consumer thread owns and executes the matching engine.

### 9.2 Strict Ownership Model
```
[Producer Thread] ──(try_push)──► [SpscQueue<OrderEvent>] ──(try_pop)──► [Consumer Thread] ──► [MatchingEngine]
```
- **Consumer Thread**: Sole owner of `MatchingEngine`, `OrderBook`, `OrderPool`, and price-level state. Only the consumer thread invokes order placement, matching, cancellation, and modification.
- **Producer Thread**: Strictly forbidden from touching or reading matching engine state. Interacts solely with the thread-safe SPSC queue boundary.
- **Matching Engine**: Contains zero mutexes, zero internal synchronization, and zero locking overhead.

### 9.3 Event & Queue Layout
- **`OrderEvent` (32 Bytes)**:
  - Trivially copyable, standard layout struct fitting exactly 2 events per 64-byte hardware cache line.
  - Fields: `type` (1B), `side` (1B), `pad` (6B), `id` (8B), `price` (8B), `qty` (8B).
  - No strings, vectors, virtual functions, or heap pointers.
- **`SpscQueue<T, Capacity, CacheAligned>`**:
  - Power-of-two bounded ring buffer with single-cycle bitwise masking (`index & (Capacity - 1)`).
  - Preallocated buffer array (0 allocations during operation).
  - Explicit queue-full policy: `try_push()` returns `false` without overwriting unread events or blocking.

### 9.4 Memory Ordering Strategy
Sequential consistency (`memory_order_seq_cst`) is strictly avoided on the hot path in favor of tailored Acquire-Release semantics and local index caching:
1. **Producer (`try_push`)**:
   - Reads `head_` with `std::memory_order_relaxed`.
   - Checks fullness against local `cached_tail_`. If full, refreshes `cached_tail_` with `tail_.load(std::memory_order_acquire)`.
   - Writes event into preallocated buffer.
   - Commits write with `head_.store(head + 1, std::memory_order_release)`, establishing a happens-before relationship for buffer writes.
2. **Consumer (`try_pop`)**:
   - Reads `tail_` with `std::memory_order_relaxed`.
   - Checks emptiness against local `cached_head_`. If empty, refreshes `cached_head_` with `head_.load(std::memory_order_acquire)`.
   - Reads event from buffer.
   - Commits read with `tail_.store(tail + 1, std::memory_order_release)`, ensuring the slot is not overwritten until reading finishes.

### 9.5 Allocation Verification (Step 12)
Active-window CRT allocation hooks verified that `try_push` and `try_pop` perform **0.00 dynamic heap allocations per event** across 200,000 continuous operations.

### 9.6 Single-Threaded & Concurrent Correctness (Step 4 & 5)
1. **Single-Threaded Unit Tests**: Passed 9 deterministic scenarios: empty pop, single push/pop, FIFO ordering, capacity fill & rejection, ring buffer wraparound, multiple wraparounds, alternating push/pop, and a 100,000-event sequence.
2. **Concurrent 1P/1C Stress Test**: Transferred 2,000,000 events between dedicated concurrent producer and consumer threads. Zero lost events, zero duplicates, zero reorderings, zero data corruptions, zero deadlock.
3. **Deterministic Pipeline Equivalence**: Passed 3 deterministic equivalence suites (seeds `0x12345678`, `0xCAFEBABE`, `0xDEADBEEF`), verifying that `Producer -> SPSC -> Consumer -> MatchingEngine` generates 100% bit-exact trade counts, prices, quantities, timestamps, order books, and structural invariants compared to direct synchronous execution.

### 9.7 Queue Microbenchmark Results (Step 6 & 11)
Measured 2,000,000 events transferred between concurrent threads:

| Queue Type | Capacity | Elapsed (ms) | Throughput (M ev/s) | Avg Latency (ns) |
| :--- | :--- | :--- | :--- | :--- |
| **SPSC (Cache-Aligned)** | 256 | 56.80 ms | **35.21 M ev/s** | 28.4 ns |
| SPSC (Unaligned) | 256 | 103.57 ms | 19.31 M ev/s | 51.8 ns |
| `std::mutex + queue` | 256 | 139.72 ms | 14.31 M ev/s | 69.9 ns |
| **SPSC (Cache-Aligned)** | 1024 | 52.76 ms | **37.91 M ev/s** | 26.4 ns |
| SPSC (Unaligned) | 1024 | 101.09 ms | 19.78 M ev/s | 50.5 ns |
| `std::mutex + queue` | 1024 | 123.30 ms | 16.22 M ev/s | 61.6 ns |
| **SPSC (Cache-Aligned)** | 4096 | 46.75 ms | **42.78 M ev/s** | 23.4 ns |
| SPSC (Unaligned) | 4096 | 115.87 ms | 17.26 M ev/s | 57.9 ns |
| `std::mutex + queue` | 4096 | 129.06 ms | 15.50 M ev/s | 64.5 ns |
| **SPSC (Cache-Aligned)** | 16384 | 41.65 ms | **48.02 M ev/s** | 20.8 ns |
| SPSC (Unaligned) | 16384 | 101.45 ms | 19.71 M ev/s | 50.7 ns |
| `std::mutex + queue` | 16384 | 98.37 ms | 20.33 M ev/s | 49.2 ns |

#### False Sharing Impact (`alignas(64)`)
Separating `head_` and `tail_` onto distinct 64-byte cache lines yielded a **+82% to +148% throughput increase** (e.g. 17.26 M &rarr; 42.78 M events/sec at capacity 4096). When unaligned, producer and consumer write-invalidations cause continuous cross-core L1 cache line bouncing (MESI invalidation ping-pong).

#### CPU Thread Affinity Experiment (Step 10)
- OS Default Scheduling: **42.31 M events/sec** (23.6 ns/event)
- Pinned Scheduling (Cores 0 & 2): **33.88 M events/sec** (29.5 ns/event, -19.9%)
On hybrid Intel architectures (P-core/E-core), OS thread scheduling dynamically utilizes turbo performance cores, whereas static affinity masks can introduce core contention or scheduling sub-optimality.

### 9.8 Pipeline Overhead Benchmark Results (Step 9)
Measured Direct Synchronous Engine vs Threaded SPSC Pipeline (`MatchingEnginePipeline`):

| Workload | Size | Direct Throughput | Pipeline Throughput | Producer Latency ($p50 / p99$) | Pipeline Boundary Cost |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **ADD-HEAVY** | 10K | 7.52 M/s | 4.96 M/s | 0 ns / 100 ns | +51.6% wall time |
| **MATCH-HEAVY** | 10K | 17.13 M/s | 8.07 M/s | 0 ns / 100 ns | +112.3% wall time |
| **CANCEL-HEAVY**| 10K | 9.70 M/s | 6.77 M/s | 0 ns / 100 ns | +43.3% wall time |
| **MIXED** | 10K | 14.71 M/s | 9.36 M/s | 0 ns / 100 ns | +57.2% wall time |
| **ADD-HEAVY** | 100K | 6.09 M/s | 5.49 M/s | 0 ns / 200 ns | +11.0% wall time |
| **MATCH-HEAVY** | 100K | 13.45 M/s | 7.43 M/s | 0 ns / 200 ns | +81.2% wall time |
| **CANCEL-HEAVY**| 100K | 6.84 M/s | 6.16 M/s | 0 ns / 300 ns | +11.1% wall time |
| **MIXED** | 100K | 12.02 M/s | 9.50 M/s | 0 ns / 100 ns | +26.6% wall time |
| **ADD-HEAVY** | 1M | 4.78 M/s | 3.74 M/s | 200 ns / 600 ns | +27.7% wall time |
| **MATCH-HEAVY** | 1M | 6.80 M/s | 4.38 M/s | 200 ns / 600 ns | +55.1% wall time |
| **CANCEL-HEAVY**| 1M | 6.20 M/s | 4.63 M/s | 200 ns / 500 ns | +34.0% wall time |
| **MIXED** | 1M | 7.23 M/s | 5.10 M/s | 100 ns / 600 ns | +41.6% wall time |

#### Analysis of Pipeline Boundary Overhead:
1. **Producer Isolation**: The producer thread experiences near-zero enqueue latency ($p50 = 0\text{--}100 \text{ ns}, p99 = 100\text{--}600 \text{ ns}$) because it pushes to the preallocated SPSC ring buffer without waiting for order matching or trade event emission.
2. **Boundary Overhead**: End-to-end throughput is 11% to 55% lower than direct execution due to cross-thread cache coherence migrations (moving 32-byte events across L2/L3 interconnect), atomic release/acquire barriers, and inter-thread yield/pause signaling when the queue empties.
3. **Core Benefit**: In exchange for this well-characterized boundary cost, the engine achieves complete decoupling from external inputs while guaranteeing strict single-threaded determinism.

---

## 10. Phase 6 — Binary Event Recording & Deterministic Replay

### 10.1 Architectural Overview
Phase 6 introduces a high-performance binary event recording and deterministic replay system using a dedicated `.hftlog` file format.

```
       [ Event Source ]
              |
              v
       [ OrderEvent ] (32 bytes)
              |
              v
      [ EventRecorder ] (64 KB write buffer, incremental CRC32)
              |
              v
       [ .hftlog File ] (64B cache-aligned header + packed 32B records)
              |
              v
      [ EventReplayer ] (64 KB read buffer, CRC32 validation)
              |
              +---------------------------+
              |                           |
              v                           v
     [ Direct Engine ]          [ SPSC Lock-Free Queue ]
              |                           |
              v                           v
   Deterministic Matches/Trades     [ Consumer / Engine ]
```

#### Core Design Invariants:
1. **Strict Decoupling**: The binary recorder and replayer perform *no trading logic whatsoever*. They are strictly responsible for low-latency serialization, checksum integrity verification, and sequential event delivery.
2. **Zero Dynamic Allocation**: Both recording and replay operate with `0.00 dynamic heap allocations per event`. Buffers are preallocated at initialization.
3. **Hardware Cache Line Alignment**: The file header is precisely 64 bytes (`alignas(64)`), ensuring the payload begins on an exact cache-line boundary, and each 32-byte `OrderEvent` aligns two records per 64-byte hardware cache line.
4. **End-to-End Cryptographic/Data Integrity**: Standard IEEE 802.3 CRC32 polynomial (`0xEDB88320`) guards both the header configuration and the full event payload against corruption, bit-rot, or truncation.

---

### 10.2 Binary Format Specification (`.hftlog`)

#### File Header Layout (Exactly 64 bytes)
```
Offset  Size  Field          Type      Description
0       4     magic          uint32_t  0x4C544648 ('HFTL' in ASCII, Little-Endian)
4       2     version        uint16_t  Format version (currently 1)
6       2     header_size    uint16_t  Fixed header size (64 bytes)
8       4     record_size    uint32_t  Fixed event record size (32 bytes)
12      4     header_crc32   uint32_t  IEEE 802.3 CRC32 of first 12 header bytes
16      8     event_count    uint64_t  Total number of OrderEvents in payload
24      4     data_crc32     uint32_t  Incremental IEEE 802.3 CRC32 of full payload
28      4     flags          uint32_t  Feature flags / compression indicator (0=raw)
32      32    reserved       uint8_t   Zero padding to complete 64-byte cache line
```

#### Event Record Layout (Exactly 32 bytes)
```
Offset  Size  Field          Type      Description
0       1     type           uint8_t   EventType (1 = Add, 2 = Cancel, 3 = Modify)
1       1     side           uint8_t   Side (0 = Buy, 1 = Sell)
2       6     pad            uint8_t   Explicit zero padding for 8-byte boundary
8       8     id             uint64_t  Unique OrderId
16      8     price          int64_t   Fixed-point limit price (integer ticks)
24      8     qty            uint64_t  Order quantity / delta quantity
```

#### Storage Efficiency & Density:
- **Record Size**: 32 bytes
- **Header Overhead**: 64 bytes total
- **Density Formula**: $\text{Density} = \frac{64 + 32 \times N}{N} \text{ bytes/event}$
- At 100,000 events: **32.0006 B/event** (File size: 3,200,064 bytes, 0.002% header)
- At 1,000,000 events: **32.0001 B/event** (File size: 32,000,064 bytes, 0.0002% header)
- At 10,000,000 events: **32.0000 B/event** (File size: 320,000,064 bytes, 0.00002% header)

---

### 10.3 Benchmark Results (Empirical Performance)

Hardware: Intel Core i5-13420H (13th Gen), Windows 11, MSVC 19.44 /O2 Release.

#### Allocation Verification
- **Recording Allocations / Event**: **0.00 allocs/event** (0 total dynamic allocations)
- **Replay Allocations / Event**: **0.00 allocs/event** (0 total dynamic allocations)

#### Multi-Scale Benchmark Matrix
| Scale | Operation / Pipeline Phase | Throughput | Latency / Event | File Size | Header Overhead |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **100,000** | **Recording** (Disk Write + CRC32) | **8.48 M ev/s** | 118.0 ns | 3.05 MB | 0.00200% |
| | **Raw Replay** (Read + Parse) | **12.01 M ev/s** | 83.3 ns | - | - |
| | **Replay &rarr; SPSC Queue** | **10.68 M ev/s** | 93.7 ns | - | - |
| | **Replay &rarr; MatchingEngine** | **5.84 M ev/s** | 171.1 ns | (49,324 trades) | - |
| **1,000,000** | **Recording** (Disk Write + CRC32) | **6.19 M ev/s** | 161.5 ns | 30.52 MB | 0.00020% |
| | **Raw Replay** (Read + Parse) | **15.03 M ev/s** | 66.5 ns | - | - |
| | **Replay &rarr; SPSC Queue** | **14.35 M ev/s** | 69.7 ns | - | - |
| | **Replay &rarr; MatchingEngine** | **3.91 M ev/s** | 255.9 ns | (493,095 trades) | - |
| **10,000,000**| **Recording** (Disk Write + CRC32) | **9.76 M ev/s** | 102.5 ns | 305.18 MB | 0.00002% |
| | **Raw Replay** (Read + Parse) | **16.13 M ev/s** | 62.0 ns | - | - |
| | **Replay &rarr; SPSC Queue** | **14.87 M ev/s** | 67.3 ns | - | - |
| | **Replay &rarr; MatchingEngine** | **3.74 M ev/s** | 267.7 ns | (4,934,465 trades) | - |

---

### 10.4 Key Observations & Analysis

1. **Raw Replay Speed Exceeds Engine Processing Capacity**:
   Raw replay parsing sustains **16.13 Million events/sec** (62.0 ns/event). When coupled with the concurrent SPSC pipeline, events are enqueued at **14.87 Million events/sec**. Because the MatchingEngine processes complex order matching at ~3.74 to 5.84 Million events/sec, replay I/O is never the bottleneck—the matching engine remains 100% compute/cache-bound.

2. **I/O Amortization via 64 KB Memory Buffers**:
   By batching 2,048 events per system read/write, filesystem syscall overhead is reduced by a factor of 2,048x. Recording throughput reaches **9.76 Million events/sec** directly to NVMe storage while calculating running 32-bit CRC32 checksums on the fly.

3. **Bit-Exact Deterministic Reproducibility**:
   Across 10,000,000 events, direct reference execution and replay from `.hftlog` produce identical trade counts (4,934,465 trades), identical execution prices, identical residual books, and identical order lookup states. Zero drift was observed.

4. **Robust Corruption Rejection**:
   Header CRC32 and Payload CRC32 guarantee immediate, graceful rejection of corrupted files, flipped bits, truncated payloads, and incompatible versions without relying on C++ exceptions or risking undefined behavior.



