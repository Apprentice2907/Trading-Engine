#include "hft/order_book.hpp"
#include <algorithm>
#include <sstream>

namespace hft {

void OrderBook::append_order_to_level(PriceLevel& level, OrderIndex idx) noexcept {
    auto& node = order_pool_.get(idx);
    node.prev = level.tail;
    node.next = INVALID_INDEX;

    if (level.tail != INVALID_INDEX) {
        order_pool_.get(level.tail).next = idx;
    } else {
        level.head = idx;
    }
    level.tail = idx;
    level.count++;
}

void OrderBook::detach_order_from_level(PriceLevel& level, OrderIndex idx) noexcept {
    auto& node = order_pool_.get(idx);
    const OrderIndex p = node.prev;
    const OrderIndex n = node.next;

    if (p != INVALID_INDEX) {
        order_pool_.get(p).next = n;
    } else {
        level.head = n;
    }

    if (n != INVALID_INDEX) {
        order_pool_.get(n).prev = p;
    } else {
        level.tail = p;
    }

    node.prev = INVALID_INDEX;
    node.next = INVALID_INDEX;
    level.count--;
}

OrderResult OrderBook::add_resting_order(const Order& order) {
    if (order.id == 0 || order.remaining_qty == 0) {
        return OrderResult::RejectedInvalidQuantity;
    }
    if (order.price <= 0) {
        return OrderResult::RejectedInvalidPrice;
    }
    if (has_order(order.id)) {
        return OrderResult::RejectedDuplicateId;
    }

    const OrderIndex idx = order_pool_.allocate(order);

    if (order.side == Side::Buy) {
        auto& level = bids_[order.price];
        level.price = order.price;
        level.total_quantity += order.remaining_qty;
        append_order_to_level(level, idx);
        order_lookup_[order.id] = OrderLocation{Side::Buy, order.price, idx};
    } else {
        auto& level = asks_[order.price];
        level.price = order.price;
        level.total_quantity += order.remaining_qty;
        append_order_to_level(level, idx);
        order_lookup_[order.id] = OrderLocation{Side::Sell, order.price, idx};
    }

    return OrderResult::Accepted;
}

size_t OrderBook::match(Order& incoming, std::vector<Trade>& trades, uint64_t& trade_seq) {
    if (incoming.remaining_qty == 0 || incoming.price <= 0) {
        return 0;
    }

    const size_t initial_trade_count = trades.size();

    if (incoming.side == Side::Buy) {
        // Match against resting asks (lowest ask first)
        while (!asks_.empty() && incoming.remaining_qty > 0) {
            auto ask_it = asks_.begin();
            if (ask_it->first > incoming.price) {
                break;
            }

            PriceLevel& level = ask_it->second;
            while (level.head != INVALID_INDEX && incoming.remaining_qty > 0) {
                const OrderIndex resting_idx = level.head;
                Order& resting = order_pool_.order(resting_idx);
                const Quantity trade_qty = std::min(incoming.remaining_qty, resting.remaining_qty);
                const Price exec_price = resting.price; // Resting order price sets execution price

                trades.push_back(Trade{
                    ++trade_seq,
                    resting.id,
                    incoming.id,
                    exec_price,
                    trade_qty,
                    incoming.side,
                    incoming.timestamp
                });

                incoming.remaining_qty -= trade_qty;
                resting.remaining_qty -= trade_qty;
                level.total_quantity -= trade_qty;

                if (resting.is_filled()) {
                    order_lookup_.erase(resting.id);
                    detach_order_from_level(level, resting_idx);
                    order_pool_.deallocate(resting_idx);
                }
            }

            if (level.count == 0) {
                asks_.erase(ask_it);
            }
        }
    } else {
        // Match against resting bids (highest bid first)
        while (!bids_.empty() && incoming.remaining_qty > 0) {
            auto bid_it = bids_.begin();
            if (bid_it->first < incoming.price) {
                break;
            }

            PriceLevel& level = bid_it->second;
            while (level.head != INVALID_INDEX && incoming.remaining_qty > 0) {
                const OrderIndex resting_idx = level.head;
                Order& resting = order_pool_.order(resting_idx);
                const Quantity trade_qty = std::min(incoming.remaining_qty, resting.remaining_qty);
                const Price exec_price = resting.price;

                trades.push_back(Trade{
                    ++trade_seq,
                    resting.id,
                    incoming.id,
                    exec_price,
                    trade_qty,
                    incoming.side,
                    incoming.timestamp
                });

                incoming.remaining_qty -= trade_qty;
                resting.remaining_qty -= trade_qty;
                level.total_quantity -= trade_qty;

                if (resting.is_filled()) {
                    order_lookup_.erase(resting.id);
                    detach_order_from_level(level, resting_idx);
                    order_pool_.deallocate(resting_idx);
                }
            }

            if (level.count == 0) {
                bids_.erase(bid_it);
            }
        }
    }

    return trades.size() - initial_trade_count;
}

OrderResult OrderBook::process_order(Order incoming, std::vector<Trade>& trades, uint64_t& trade_seq) {
    if (incoming.id == 0 || incoming.remaining_qty == 0) {
        return OrderResult::RejectedInvalidQuantity;
    }
    if (incoming.price <= 0) {
        return OrderResult::RejectedInvalidPrice;
    }
    if (has_order(incoming.id)) {
        return OrderResult::RejectedDuplicateId;
    }

    match(incoming, trades, trade_seq);

    if (incoming.remaining_qty > 0) {
        return add_resting_order(incoming);
    }

    return OrderResult::Accepted;
}

OrderResult OrderBook::cancel(OrderId id) {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) {
        return OrderResult::RejectedOrderNotFound;
    }

    const OrderLocation loc = it->second;
    if (loc.side == Side::Buy) {
        auto bid_it = bids_.find(loc.price);
        if (bid_it != bids_.end()) {
            PriceLevel& level = bid_it->second;
            level.total_quantity -= order_pool_.order(loc.pool_index).remaining_qty;
            detach_order_from_level(level, loc.pool_index);
            order_pool_.deallocate(loc.pool_index);
            if (level.count == 0) {
                bids_.erase(bid_it);
            }
        }
    } else {
        auto ask_it = asks_.find(loc.price);
        if (ask_it != asks_.end()) {
            PriceLevel& level = ask_it->second;
            level.total_quantity -= order_pool_.order(loc.pool_index).remaining_qty;
            detach_order_from_level(level, loc.pool_index);
            order_pool_.deallocate(loc.pool_index);
            if (level.count == 0) {
                asks_.erase(ask_it);
            }
        }
    }

    order_lookup_.erase(it);
    return OrderResult::Accepted;
}

OrderResult OrderBook::modify(OrderId id, Price new_price, Quantity new_qty,
                              std::vector<Trade>& trades, uint64_t& trade_seq) {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) {
        return OrderResult::RejectedOrderNotFound;
    }
    if (new_price <= 0) {
        return OrderResult::RejectedInvalidPrice;
    }
    if (new_qty == 0) {
        return OrderResult::RejectedInvalidQuantity;
    }

    const OrderLocation loc = it->second;
    Order& existing = order_pool_.order(loc.pool_index);

    if (new_price == existing.price && new_qty == existing.remaining_qty) {
        return OrderResult::RejectedUnchanged;
    }

    // Case 1: Same price, quantity reduction -> Retains FIFO queue priority
    if (new_price == existing.price && new_qty < existing.remaining_qty) {
        const Quantity diff = existing.remaining_qty - new_qty;
        existing.remaining_qty = new_qty;
        if (loc.side == Side::Buy) {
            bids_[loc.price].total_quantity -= diff;
        } else {
            asks_[loc.price].total_quantity -= diff;
        }
        return OrderResult::Accepted;
    }

    // Case 2: Same price, quantity increase -> Loses priority, moved to tail of queue
    if (new_price == existing.price && new_qty > existing.remaining_qty) {
        const Quantity diff = new_qty - existing.remaining_qty;
        existing.remaining_qty = new_qty;
        existing.initial_qty += diff;
        existing.timestamp = trade_seq;

        if (loc.side == Side::Buy) {
            auto& level = bids_[loc.price];
            level.total_quantity += diff;
            detach_order_from_level(level, loc.pool_index);
            append_order_to_level(level, loc.pool_index);
        } else {
            auto& level = asks_[loc.price];
            level.total_quantity += diff;
            detach_order_from_level(level, loc.pool_index);
            append_order_to_level(level, loc.pool_index);
        }
        return OrderResult::Accepted;
    }

    // Case 3: Price change -> Cancel & Replace
    Order replacement = existing;
    replacement.price = new_price;
    replacement.remaining_qty = new_qty;
    replacement.initial_qty = new_qty;
    replacement.timestamp = trade_seq;

    cancel(id);
    return process_order(replacement, trades, trade_seq);
}

bool OrderBook::has_order(OrderId id) const noexcept {
    return order_lookup_.find(id) != order_lookup_.end();
}

std::optional<Order> OrderBook::get_order(OrderId id) const {
    auto it = order_lookup_.find(id);
    if (it == order_lookup_.end()) {
        return std::nullopt;
    }
    return order_pool_.order(it->second.pool_index);
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity OrderBook::best_bid_qty() const noexcept {
    if (bids_.empty()) {
        return 0;
    }
    return bids_.begin()->second.total_quantity;
}

Quantity OrderBook::best_ask_qty() const noexcept {
    if (asks_.empty()) {
        return 0;
    }
    return asks_.begin()->second.total_quantity;
}

size_t OrderBook::bid_depth() const noexcept {
    return bids_.size();
}

size_t OrderBook::ask_depth() const noexcept {
    return asks_.size();
}

size_t OrderBook::total_orders() const noexcept {
    return order_lookup_.size();
}

Quantity OrderBook::total_bid_qty() const noexcept {
    Quantity sum = 0;
    for (const auto& [price, level] : bids_) {
        sum += level.total_quantity;
    }
    return sum;
}

Quantity OrderBook::total_ask_qty() const noexcept {
    Quantity sum = 0;
    for (const auto& [price, level] : asks_) {
        sum += level.total_quantity;
    }
    return sum;
}

std::vector<LevelView> OrderBook::get_bid_levels() const {
    std::vector<LevelView> views;
    views.reserve(bids_.size());
    for (const auto& [price, level] : bids_) {
        views.push_back(LevelView{price, level.total_quantity, level.count});
    }
    return views;
}

std::vector<LevelView> OrderBook::get_ask_levels() const {
    std::vector<LevelView> views;
    views.reserve(asks_.size());
    for (const auto& [price, level] : asks_) {
        views.push_back(LevelView{price, level.total_quantity, level.count});
    }
    return views;
}

bool OrderBook::verify_invariants(std::string* error_out) const {
    auto report_err = [&](const std::string& msg) {
        if (error_out) {
            *error_out = msg;
        }
        return false;
    };

    // Invariant 1: Uncrossed book
    if (!bids_.empty() && !asks_.empty()) {
        const Price bb = bids_.begin()->first;
        const Price ba = asks_.begin()->first;
        if (bb >= ba) {
            return report_err("Invariant 1 Violation: Crossed book! Best Bid (" +
                              std::to_string(bb) + ") >= Best Ask (" +
                              std::to_string(ba) + ")");
        }
    }

    size_t order_count_in_queues = 0;

    // Check Bids
    Price prev_bid = 0;
    bool first_bid = true;
    for (const auto& [price, level] : bids_) {
        if (!first_bid && price >= prev_bid) {
            return report_err("Invariant 9 Violation: Bids not strictly descending");
        }
        prev_bid = price;
        first_bid = false;

        if (level.count == 0 || level.head == INVALID_INDEX) {
            return report_err("Invariant 9 Violation: Empty bid level exists at price " +
                              std::to_string(price));
        }

        Quantity level_sum = 0;
        size_t traversed = 0;
        Timestamp prev_ts = 0;
        bool first_order = true;

        OrderIndex curr = level.head;
        OrderIndex last_seen = INVALID_INDEX;

        while (curr != INVALID_INDEX) {
            const auto& node = order_pool_.get(curr);
            if (!node.in_use) {
                return report_err("Invariant Violation: Slot not in use in live level");
            }
            const Order& order = node.order;

            if (order.remaining_qty == 0) {
                return report_err("Invariant 5 Violation: Zero remaining qty for order " +
                                  std::to_string(order.id));
            }
            if (order.price != price) {
                return report_err("Invariant Violation: Order price does not match level");
            }
            if (order.side != Side::Buy) {
                return report_err("Invariant 2 Violation: Sell order in Bid book");
            }

            if (!first_order && order.timestamp < prev_ts) {
                return report_err("Invariant 7 Violation: FIFO priority inverted");
            }
            prev_ts = order.timestamp;
            first_order = false;

            level_sum += order.remaining_qty;
            traversed++;
            order_count_in_queues++;

            auto it = order_lookup_.find(order.id);
            if (it == order_lookup_.end()) {
                return report_err("Invariant 3 Violation: Order ID " + std::to_string(order.id) +
                                  " missing in lookup");
            }
            if (it->second.pool_index != curr) {
                return report_err("Invariant 3 Violation: Lookup index mismatch for order " +
                                  std::to_string(order.id));
            }

            last_seen = curr;
            curr = node.next;
        }

        if (traversed != level.count) {
            return report_err("Invariant Violation: Traversed order count != level.count");
        }
        if (last_seen != level.tail) {
            return report_err("Invariant Violation: Last traversed order != level.tail");
        }
        if (level.total_quantity != level_sum) {
            return report_err("Invariant Violation: Bid level total_quantity != sum of orders");
        }
    }

    // Check Asks
    Price prev_ask = 0;
    bool first_ask = true;
    for (const auto& [price, level] : asks_) {
        if (!first_ask && price <= prev_ask) {
            return report_err("Invariant 9 Violation: Asks not strictly ascending");
        }
        prev_ask = price;
        first_ask = false;

        if (level.count == 0 || level.head == INVALID_INDEX) {
            return report_err("Invariant 9 Violation: Empty ask level exists at price " +
                              std::to_string(price));
        }

        Quantity level_sum = 0;
        size_t traversed = 0;
        Timestamp prev_ts = 0;
        bool first_order = true;

        OrderIndex curr = level.head;
        OrderIndex last_seen = INVALID_INDEX;

        while (curr != INVALID_INDEX) {
            const auto& node = order_pool_.get(curr);
            if (!node.in_use) {
                return report_err("Invariant Violation: Slot not in use in live level");
            }
            const Order& order = node.order;

            if (order.remaining_qty == 0) {
                return report_err("Invariant 5 Violation: Zero remaining qty for order " +
                                  std::to_string(order.id));
            }
            if (order.price != price) {
                return report_err("Invariant Violation: Order price does not match level");
            }
            if (order.side != Side::Sell) {
                return report_err("Invariant 2 Violation: Buy order in Ask book");
            }

            if (!first_order && order.timestamp < prev_ts) {
                return report_err("Invariant 7 Violation: FIFO priority inverted");
            }
            prev_ts = order.timestamp;
            first_order = false;

            level_sum += order.remaining_qty;
            traversed++;
            order_count_in_queues++;

            auto it = order_lookup_.find(order.id);
            if (it == order_lookup_.end()) {
                return report_err("Invariant 3 Violation: Order ID " + std::to_string(order.id) +
                                  " missing in lookup");
            }
            if (it->second.pool_index != curr) {
                return report_err("Invariant 3 Violation: Lookup index mismatch for order " +
                                  std::to_string(order.id));
            }

            last_seen = curr;
            curr = node.next;
        }

        if (traversed != level.count) {
            return report_err("Invariant Violation: Traversed order count != level.count");
        }
        if (last_seen != level.tail) {
            return report_err("Invariant Violation: Last traversed order != level.tail");
        }
        if (level.total_quantity != level_sum) {
            return report_err("Invariant Violation: Ask level total_quantity != sum of orders");
        }
    }

    // Invariant 3 & 4: Lookup size matches queues
    if (order_lookup_.size() != order_count_in_queues) {
        return report_err("Invariant 3/4 Violation: order_lookup_ size != total orders in queues");
    }

    // Pool size matches live orders
    if (order_pool_.size() != order_count_in_queues) {
        return report_err("Invariant Violation: order_pool_.size() != live orders count");
    }

    return true;
}

} // namespace hft
