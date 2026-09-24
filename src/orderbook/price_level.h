#pragma once

#include "orderbook/order.h"

#include <cstdint>
#include <list>

namespace mde {

// All resting orders at one price, on one side of one symbol's book.
// `orders` preserves FIFO (price-time) priority -- the order at the front
// was added first and is the one consumed first on execution, matching
// real exchange price-time priority matching. `total_qty` is a running sum
// of every order's remaining_qty at this price, kept in sync by
// OrderBook's mutating operations so query methods don't need to re-scan
// the list.
struct PriceLevel {
    uint32_t price_raw;
    uint32_t total_qty = 0;
    std::list<Order> orders;
};

} // namespace mde
