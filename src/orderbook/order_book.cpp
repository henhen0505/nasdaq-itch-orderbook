#include "orderbook/order_book.h"

#include "parser/itch_messages.h"

#include <iterator>

namespace mde {

template <typename MapT>
void OrderBook::insert_order(MapT& side_map, char side, uint64_t order_ref, uint32_t price_raw, uint32_t shares) {
    PriceLevel& level = side_map[price_raw]; // creates the level if new
    level.price_raw = price_raw;
    level.orders.push_back(Order{order_ref, side, price_raw, shares});
    level.total_qty += shares;

    auto last = std::prev(level.orders.end());
    orders_by_ref_[order_ref] = OrderLocation{side, &level, last};
}

void OrderBook::add_order(uint64_t order_ref, char side, uint32_t price_raw, uint32_t shares) {
    if (side == 'B') {
        insert_order(bids_, side, order_ref, price_raw, shares);
    } else {
        insert_order(asks_, side, order_ref, price_raw, shares);
    }
}

void OrderBook::erase_order(std::unordered_map<uint64_t, OrderLocation>::iterator ref_it) {
    OrderLocation loc = ref_it->second; // copy: orders_by_ref_.erase() below invalidates ref_it
    orders_by_ref_.erase(ref_it);

    loc.level->orders.erase(loc.it);
    if (loc.level->orders.empty()) {
        uint32_t price_raw = loc.level->price_raw; // capture before the map erase destroys *loc.level
        if (loc.side == 'B') {
            bids_.erase(price_raw);
        } else {
            asks_.erase(price_raw);
        }
    }
}

void OrderBook::reduce_order(uint64_t order_ref, uint32_t qty) {
    auto ref_it = orders_by_ref_.find(order_ref);
    if (ref_it == orders_by_ref_.end()) {
        ++unresolved_reference_count_;
        return;
    }

    OrderLocation& loc = ref_it->second;
    Order& order = *loc.it;

    uint32_t applied_qty = qty;
    if (applied_qty > order.remaining_qty) {
        ++quantity_anomaly_count_;
        applied_qty = order.remaining_qty; // clamp: never underflow remaining_qty
    }

    order.remaining_qty -= applied_qty;
    loc.level->total_qty -= applied_qty;

    if (order.remaining_qty == 0) {
        erase_order(ref_it);
    }
}

void OrderBook::execute_order(uint64_t order_ref, uint32_t executed_shares) {
    reduce_order(order_ref, executed_shares);
}

void OrderBook::cancel_order(uint64_t order_ref, uint32_t cancelled_shares) {
    reduce_order(order_ref, cancelled_shares);
}

void OrderBook::delete_order(uint64_t order_ref) {
    auto ref_it = orders_by_ref_.find(order_ref);
    if (ref_it == orders_by_ref_.end()) {
        ++unresolved_reference_count_;
        return;
    }

    OrderLocation& loc = ref_it->second;
    loc.level->total_qty -= loc.it->remaining_qty; // full removal, regardless of remaining qty
    erase_order(ref_it);
}

void OrderBook::replace_order(uint64_t original_order_ref, uint64_t new_order_ref, uint32_t new_price_raw,
                               uint32_t new_shares) {
    auto ref_it = orders_by_ref_.find(original_order_ref);
    if (ref_it == orders_by_ref_.end()) {
        ++unresolved_reference_count_;
        return;
    }

    // OrderReplaceMessage carries no side field -- capture it from the
    // original order before deleting, so the replacement lands on the
    // same side.
    char side = ref_it->second.side;

    delete_order(original_order_ref);
    add_order(new_order_ref, side, new_price_raw, new_shares);
}

std::optional<uint32_t> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<uint32_t> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<uint32_t> OrderBook::spread() const {
    std::optional<uint32_t> bid = best_bid();
    std::optional<uint32_t> ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

std::optional<double> OrderBook::midpoint() const {
    std::optional<uint32_t> bid = best_bid();
    std::optional<uint32_t> ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return (itch_price_to_decimal(*bid) + itch_price_to_decimal(*ask)) / 2.0;
}

uint32_t OrderBook::bid_qty_at_best() const {
    if (bids_.empty()) {
        return 0;
    }
    return bids_.begin()->second.total_qty;
}

uint32_t OrderBook::ask_qty_at_best() const {
    if (asks_.empty()) {
        return 0;
    }
    return asks_.begin()->second.total_qty;
}

std::optional<double> OrderBook::imbalance() const {
    double bid_qty = static_cast<double>(bid_qty_at_best());
    double ask_qty = static_cast<double>(ask_qty_at_best());
    double denom = bid_qty + ask_qty;
    if (denom == 0.0) {
        return std::nullopt; // both sides empty
    }
    return (bid_qty - ask_qty) / denom;
}

std::vector<std::pair<uint32_t, uint32_t>> OrderBook::depth(char side, size_t n_levels) const {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    result.reserve(n_levels);

    if (side == 'B') {
        for (auto it = bids_.begin(); it != bids_.end() && result.size() < n_levels; ++it) {
            result.emplace_back(it->first, it->second.total_qty);
        }
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && result.size() < n_levels; ++it) {
            result.emplace_back(it->first, it->second.total_qty);
        }
    }

    return result;
}

size_t OrderBook::unresolved_reference_count() const {
    return unresolved_reference_count_;
}

size_t OrderBook::quantity_anomaly_count() const {
    return quantity_anomaly_count_;
}

} // namespace mde
