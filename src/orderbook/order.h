#pragma once

#include <cstdint>

namespace mde {

// One resting order on one side of a book. Populated on Add Order, mutated
// in place by Execute/Cancel (remaining_qty shrinks), removed entirely by
// Delete or by Execute/Cancel consuming the last share. `side` mirrors
// ITCH's buy_sell_indicator convention ('B' or 'S', see AddOrderMessage in
// src/parser/itch_messages.h).
struct Order {
    uint64_t order_ref;
    char side;
    uint32_t price_raw;
    uint32_t remaining_qty;
};

} // namespace mde
