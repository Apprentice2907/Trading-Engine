#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/baseline/baseline_order_book.hpp"

#include <vector>
#include <string>

namespace hft::baseline {

class MatchingEngine {
public:
    MatchingEngine() = default;
    ~MatchingEngine() = default;

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&) noexcept = default;
    MatchingEngine& operator=(MatchingEngine&&) noexcept = default;

    OrderResult submit_limit_order(OrderId id, Side side, Price price, Quantity qty,
                                  std::vector<Trade>& trades);
    OrderResult cancel_order(OrderId id);
    OrderResult modify_order(OrderId id, Price new_price, Quantity new_qty,
                            std::vector<Trade>& trades);

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] uint64_t total_trades_generated() const noexcept { return trade_sequence_; }
    [[nodiscard]] Timestamp current_sequence() const noexcept { return sequence_number_; }

    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const {
        return book_.verify_invariants(error_out);
    }

    void reset();

private:
    OrderBook book_;
    uint64_t trade_sequence_{0};
    Timestamp sequence_number_{0};
};

} // namespace hft::baseline
