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

## 6. Why a Baseline First?

Premature optimization obscures invariants, complicates debugging, and introduces speculative complexity without hard numbers. By first implementing a simple, correct, and extensively tested baseline:
1. We establish a verified **golden model** for correctness testing.
2. We can profile and measure where time is actually spent (e.g., node allocations, cache misses, branch mispredictions).
3. Any subsequent low-latency data structure (custom memory pools, intrusive lists, flat ring-buffers) can be benchmarked against this exact control implementation.

---

## 7. Roadmap for Phase 2 & Beyond

1. **Benchmarking Harness**: Measure microsecond/nanosecond distributions (p50, p99, p99.9) using high-resolution clocks.
2. **Allocation Profiling**: Count dynamic heap allocations (`new` / `delete`) during high-frequency order churn.
3. **Cache-Locality Analysis**: Evaluate data cache hit rates with flat contiguously allocated price level structures.
4. **Intrusive Containers**: Replace `std::list` with intrusive double-linked lists to eliminate node allocations.

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

