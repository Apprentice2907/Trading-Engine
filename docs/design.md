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

1. **Benchmarking Harness**: Measure microsecond/nanosecond distributions (p50, p99, p99.9) using CPU cycle counters (`__rdtsc` / `std::chrono::high_resolution_clock`).
2. **Allocation Profiling**: Count dynamic heap allocations (`new` / `delete`) during high-frequency order churn.
3. **Cache-Locality Analysis**: Evaluate L1/L2 data cache hit rates with flat contiguously allocated price level rings.
4. **Intrusive Containers**: Replace `std::list` with intrusive double-linked lists to eliminate node allocations.
