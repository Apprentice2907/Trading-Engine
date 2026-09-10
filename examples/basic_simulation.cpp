#include "hft/matching_engine.hpp"

#include <iostream>
#include <iomanip>
#include <vector>

void print_order_book(const hft::OrderBook& book) {
    std::cout << "\n========================================\n";
    std::cout << "               ORDER BOOK\n";
    std::cout << "========================================\n";

    auto asks = book.get_ask_levels();
    auto bids = book.get_bid_levels();

    std::cout << "ASKS (Sell Side):\n";
    if (asks.empty()) {
        std::cout << "  (Empty)\n";
    } else {
        // Display asks from highest down to lowest (towards the spread)
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
            std::cout << "  " << std::setw(8) << it->price
                      << "  x  " << std::setw(6) << it->total_quantity
                      << "  (" << it->order_count << " order"
                      << (it->order_count > 1 ? "s" : "") << ")\n";
        }
    }

    std::cout << "----------------------------------------\n";
    if (book.best_bid().has_value() && book.best_ask().has_value()) {
        const hft::Price spread = *book.best_ask() - *book.best_bid();
        std::cout << "  SPREAD: " << spread
                  << " (Bid: " << *book.best_bid()
                  << " | Ask: " << *book.best_ask() << ")\n";
    } else {
        std::cout << "  SPREAD: N/A\n";
    }
    std::cout << "----------------------------------------\n";

    std::cout << "BIDS (Buy Side):\n";
    if (bids.empty()) {
        std::cout << "  (Empty)\n";
    } else {
        // Display bids from highest (best bid) down
        for (const auto& level : bids) {
            std::cout << "  " << std::setw(8) << level.price
                      << "  x  " << std::setw(6) << level.total_quantity
                      << "  (" << level.order_count << " order"
                      << (level.order_count > 1 ? "s" : "") << ")\n";
        }
    }
    std::cout << "========================================\n\n";
}

void print_trades(const std::vector<hft::Trade>& trades) {
    if (trades.empty()) {
        std::cout << "  No trades executed.\n";
        return;
    }

    std::cout << ">>> EXECUTED TRADES (" << trades.size() << "):\n";
    for (const auto& t : trades) {
        std::cout << "  [Trade #" << t.trade_id << "] "
                  << "Price: " << t.price
                  << " | Qty: " << t.quantity
                  << " | Resting Order #" << t.resting_order_id
                  << " <-> Aggressor Order #" << t.incoming_order_id
                  << " (" << hft::to_string(t.aggressor_side) << ")\n";
    }
    std::cout << "\n";
}

int main() {
    std::cout << "==================================================\n";
    std::cout << " LOW-LATENCY C++ EXCHANGE ENGINE: BASIC SIMULATION\n";
    std::cout << " Phase 1: Baseline LOB & Deterministic Matching\n";
    std::cout << "==================================================\n\n";

    hft::MatchingEngine engine;
    std::vector<hft::Trade> trades;

    std::cout << "[Step 1] Seeding Initial Bids into Order Book...\n";
    engine.submit_limit_order(101, hft::Side::Buy, 10000, 200, trades); // 100.00 x 200
    engine.submit_limit_order(102, hft::Side::Buy, 10005, 100, trades); // 100.05 x 100
    engine.submit_limit_order(103, hft::Side::Buy, 9995,  150, trades); //  99.95 x 150
    engine.submit_limit_order(104, hft::Side::Buy, 10005,  50, trades); // 100.05 x  50 (FIFO queue priority after 102)

    std::cout << "[Step 2] Seeding Initial Asks into Order Book...\n";
    engine.submit_limit_order(201, hft::Side::Sell, 10010, 150, trades); // 100.10 x 150
    engine.submit_limit_order(202, hft::Side::Sell, 10015, 100, trades); // 100.15 x 100
    engine.submit_limit_order(203, hft::Side::Sell, 10020, 250, trades); // 100.20 x 250

    print_order_book(engine.book());

    std::cout << "[Step 3] Submitting Aggressive Buy Order #301:\n";
    std::cout << "  BUY 200 shares @ limit price 10015\n";
    std::cout << "  (Should sweep Ask level 10010 for 150, and take 50 from Ask level 10015)\n\n";

    trades.clear();
    engine.submit_limit_order(301, hft::Side::Buy, 10015, 200, trades);

    print_trades(trades);
    print_order_book(engine.book());

    std::cout << "[Step 4] Submitting Non-Crossing Sell Order #204:\n";
    std::cout << "  SELL 80 shares @ limit price 10008 (New Best Ask)\n\n";

    trades.clear();
    engine.submit_limit_order(204, hft::Side::Sell, 10008, 80, trades);
    print_order_book(engine.book());

    std::cout << "[Step 5] Modifying Order #102:\n";
    std::cout << "  Reduce remaining quantity from 100 to 40 (In-place reduction, retains FIFO priority)\n\n";

    trades.clear();
    engine.modify_order(102, 10005, 40, trades);
    print_order_book(engine.book());

    std::cout << "[Step 6] Cancelling Order #104 (50 shares @ 10005)...\n";
    engine.cancel_order(104);
    print_order_book(engine.book());

    std::string err;
    if (engine.verify_invariants(&err)) {
        std::cout << "[Invariant Check] ALL 10 ORDER BOOK INVARIANTS SATISFIED!\n";
    } else {
        std::cerr << "[Invariant Check] FAILED: " << err << "\n";
        return 1;
    }

    std::cout << "\nSimulation completed successfully.\n";
    return 0;
}
