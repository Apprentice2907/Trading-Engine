#pragma once

#include "hft/types.hpp"
#include "hft/order.hpp"
#include "hft/flat_order_book.hpp"

#include <vector>
#include <optional>
#include <string>

namespace hft {

/**
 * @brief Matching Engine wrapping the experimental FlatOrderBook.
 */
class FlatMatchingEngine {
public:
    FlatMatchingEngine() = default;
    ~FlatMatchingEngine() = default;

    // Non-copyable, movable
    FlatMatchingEngine(const FlatMatchingEngine&) = delete;
    FlatMatchingEngine& operator=(const FlatMatchingEngine&) = delete;
    FlatMatchingEngine(FlatMatchingEngine&&) noexcept = default;
    FlatMatchingEngine& operator=(FlatMatchingEngine&&) noexcept = default;

    OrderResult submit_limit_order(OrderId id, Side side, Price price, Quantity qty,
                                  std::vector<Trade>& trades);

    OrderResult cancel_order(OrderId id);

    OrderResult modify_order(OrderId id, Price new_price, Quantity new_qty,
                            std::vector<Trade>& trades);

    void reserve(size_t order_capacity, size_t level_capacity = 1024) {
        book_.reserve(order_capacity, level_capacity);
    }

    [[nodiscard]] const FlatOrderBook& book() const noexcept { return book_; }
    [[nodiscard]] FlatOrderBook& book() noexcept { return book_; }
    [[nodiscard]] uint64_t total_trades_generated() const noexcept { return trade_sequence_; }
    [[nodiscard]] Timestamp current_sequence() const noexcept { return sequence_number_; }

    [[nodiscard]] bool verify_invariants(std::string* error_out = nullptr) const {
        return book_.verify_invariants(error_out);
    }

    void reset();

private:
    FlatOrderBook book_;
    uint64_t trade_sequence_{0};
    Timestamp sequence_number_{0};
};

} // namespace hft
