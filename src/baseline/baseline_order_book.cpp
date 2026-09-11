#include "hft/baseline/baseline_order_book.hpp"
#include <algorithm>
#include <sstream>

namespace hft::baseline {

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

    if (order.side == Side::Buy) {
        auto& level = bids_[order.price];
        level.price = order.price;
        level.total_quantity += order.remaining_qty;
        level.orders.push_back(order);
        auto iter = std::prev(level.orders.end());
        order_lookup_[order.id] = OrderLocation{Side::Buy, order.price, iter};
    } else {
        auto& level = asks_[order.price];
        level.price = order.price;
        level.total_quantity += order.remaining_qty;
        level.orders.push_back(order);
        auto iter = std::prev(level.orders.end());
        order_lookup_[order.id] = OrderLocation{Side::Sell, order.price, iter};
    }

    return OrderResult::Accepted;
}

size_t OrderBook::match(Order& incoming, std::vector<Trade>& trades, uint64_t& trade_seq) {
    if (incoming.remaining_qty == 0 || incoming.price <= 0) {
        return 0;
    }

    const size_t initial_trade_count = trades.size();

    if (incoming.side == Side::Buy) {
        while (!asks_.empty() && incoming.remaining_qty > 0) {
            auto ask_it = asks_.begin();
            if (ask_it->first > incoming.price) {
                break;
            }

            PriceLevel& level = ask_it->second;
            while (!level.orders.empty() && incoming.remaining_qty > 0) {
                Order& resting = level.orders.front();
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
                    level.orders.pop_front();
                }
            }

            if (level.orders.empty()) {
                asks_.erase(ask_it);
            }
        }
    } else {
        while (!bids_.empty() && incoming.remaining_qty > 0) {
            auto bid_it = bids_.begin();
            if (bid_it->first < incoming.price) {
                break;
            }

            PriceLevel& level = bid_it->second;
            while (!level.orders.empty() && incoming.remaining_qty > 0) {
                Order& resting = level.orders.front();
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
                    level.orders.pop_front();
                }
            }

            if (level.orders.empty()) {
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

    const OrderLocation& loc = it->second;
    if (loc.side == Side::Buy) {
        auto bid_it = bids_.find(loc.price);
        if (bid_it != bids_.end()) {
            bid_it->second.total_quantity -= loc.iter->remaining_qty;
            bid_it->second.orders.erase(loc.iter);
            if (bid_it->second.orders.empty()) {
                bids_.erase(bid_it);
            }
        }
    } else {
        auto ask_it = asks_.find(loc.price);
        if (ask_it != asks_.end()) {
            ask_it->second.total_quantity -= loc.iter->remaining_qty;
            ask_it->second.orders.erase(loc.iter);
            if (ask_it->second.orders.empty()) {
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

    OrderLocation& loc = it->second;
    Order& existing = *(loc.iter);

    if (new_price == existing.price && new_qty == existing.remaining_qty) {
        return OrderResult::RejectedUnchanged;
    }

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

    if (new_price == existing.price && new_qty > existing.remaining_qty) {
        const Quantity diff = new_qty - existing.remaining_qty;
        existing.remaining_qty = new_qty;
        existing.initial_qty += diff;

        if (loc.side == Side::Buy) {
            auto& level = bids_[loc.price];
            level.total_quantity += diff;
            level.orders.splice(level.orders.end(), level.orders, loc.iter);
            loc.iter = std::prev(level.orders.end());
        } else {
            auto& level = asks_[loc.price];
            level.total_quantity += diff;
            level.orders.splice(level.orders.end(), level.orders, loc.iter);
            loc.iter = std::prev(level.orders.end());
        }
        return OrderResult::Accepted;
    }

    Order replacement = existing;
    replacement.price = new_price;
    replacement.remaining_qty = new_qty;
    replacement.initial_qty = new_qty;

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
    return *(it->second.iter);
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
        views.push_back(LevelView{price, level.total_quantity, level.orders.size()});
    }
    return views;
}

std::vector<LevelView> OrderBook::get_ask_levels() const {
    std::vector<LevelView> views;
    views.reserve(asks_.size());
    for (const auto& [price, level] : asks_) {
        views.push_back(LevelView{price, level.total_quantity, level.orders.size()});
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

    if (!bids_.empty() && !asks_.empty()) {
        const Price bb = bids_.begin()->first;
        const Price ba = asks_.begin()->first;
        if (bb >= ba) {
            return report_err("Crossed book");
        }
    }

    size_t order_count_in_queues = 0;
    for (const auto& [price, level] : bids_) {
        Quantity level_sum = 0;
        for (const auto& order : level.orders) {
            if (order.remaining_qty == 0 || order.side != Side::Buy) {
                return report_err("Invalid order in bid level");
            }
            level_sum += order.remaining_qty;
            order_count_in_queues++;
        }
        if (level.total_quantity != level_sum) {
            return report_err("Bid level quantity mismatch");
        }
    }

    for (const auto& [price, level] : asks_) {
        Quantity level_sum = 0;
        for (const auto& order : level.orders) {
            if (order.remaining_qty == 0 || order.side != Side::Sell) {
                return report_err("Invalid order in ask level");
            }
            level_sum += order.remaining_qty;
            order_count_in_queues++;
        }
        if (level.total_quantity != level_sum) {
            return report_err("Ask level quantity mismatch");
        }
    }

    if (order_lookup_.size() != order_count_in_queues) {
        return report_err("Order count mismatch");
    }

    return true;
}

} // namespace hft::baseline
