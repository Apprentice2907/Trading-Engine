#include "hft/matching_engine.hpp"

namespace hft {

OrderResult MatchingEngine::submit_limit_order(OrderId id, Side side, Price price, Quantity qty,
                                              std::vector<Trade>& trades) {
    if (id == 0) {
        return OrderResult::RejectedDuplicateId;
    }
    if (price <= 0) {
        return OrderResult::RejectedInvalidPrice;
    }
    if (qty == 0) {
        return OrderResult::RejectedInvalidQuantity;
    }
    if (book_.has_order(id)) {
        return OrderResult::RejectedDuplicateId;
    }

    ++sequence_number_;
    Order order{
        id,
        price,
        qty,
        qty,
        side,
        OrderType::Limit,
        sequence_number_
    };

    return book_.process_order(order, trades, trade_sequence_);
}

OrderResult MatchingEngine::cancel_order(OrderId id) {
    if (id == 0) {
        return OrderResult::RejectedOrderNotFound;
    }
    return book_.cancel(id);
}

OrderResult MatchingEngine::modify_order(OrderId id, Price new_price, Quantity new_qty,
                                        std::vector<Trade>& trades) {
    if (id == 0) {
        return OrderResult::RejectedOrderNotFound;
    }
    ++sequence_number_;
    return book_.modify(id, new_price, new_qty, trades, sequence_number_);
}

void MatchingEngine::reset() {
    book_ = OrderBook();
    trade_sequence_ = 0;
    sequence_number_ = 0;
}

} // namespace hft
