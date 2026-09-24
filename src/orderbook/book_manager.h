#pragma once

#include "orderbook/order_book.h"
#include "parser/itch_messages.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace mde {

// Owns one OrderBook per stock_locate and dispatches parsed ITCH messages
// to the right book's mutating operation. System Event ('S') and Stock
// Directory ('R') messages don't affect book state and are ignored --
// the parser already resolves ticker filtering, so BookManager only
// ever deals in stock_locate, never tickers.
class BookManager {
public:
    // Looks up (creating if needed) the OrderBook for msg's stock_locate
    // and applies the matching mutating operation: Add Order ('A'/'F') ->
    // add_order(), Order Executed ('E') / Executed with Price ('C') ->
    // execute_order(), Order Cancel ('X') -> cancel_order(), Order Delete
    // ('D') -> delete_order(), Order Replace ('U') -> replace_order().
    // System Event and Stock Directory messages are no-ops.
    //
    // Returns the stock_locate of the book that was just mutated, or
    // std::nullopt for System Event / Stock Directory messages (which
    // mutate no book). Lets callers (e.g. the loader's snapshot cadence
    // policy in src/persistence/loader.cpp) know what changed without
    // duplicating this message-type dispatch logic themselves.
    std::optional<uint16_t> process(const ItchMessage& msg);

    // Returns the book for stock_locate, or nullptr if no message for that
    // stock_locate has been processed yet.
    const OrderBook* get_book(uint16_t stock_locate) const;

private:
    std::unordered_map<uint16_t, OrderBook> books_;
};

} // namespace mde
