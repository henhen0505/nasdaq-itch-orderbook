#pragma once

#include "parser/itch_messages.h"
#include "persistence/db_connection.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mde {

// Default number of messages processed before an automatic flush.
constexpr size_t kDefaultBatchSize = 5000;

// One row of the wide `messages` table (sql/001_schema.sql). Optional
// fields are NULL in the database for message types that don't use them --
// see to_message_row() for the exact per-type mapping.
struct MessageRow {
    char msg_type;
    uint64_t timestamp_ns;
    uint16_t stock_locate;
    std::optional<uint64_t> order_ref;
    std::optional<char> side;
    std::optional<uint32_t> shares;
    std::optional<uint32_t> price_raw;
    std::optional<uint64_t> new_order_ref;
    std::optional<uint64_t> match_number;

    // Set only for Stock Directory ('R') rows. Not a `messages` column --
    // used to upsert `symbols` before this row is inserted, since
    // messages.stock_locate carries a foreign key to symbols(stock_locate).
    std::optional<std::string> ticker;
};

// Maps one of the 9 ITCH message types to its `messages` row. Populates
// new_order_ref only for Order Replace, match_number only for Order
// Executed / Executed-with-Price, side only for Add Order / Add Order
// MPID -- everything else stays NULL, per the wide-table design in
// sql/001_schema.sql.
MessageRow to_message_row(const ItchMessage& msg);

// One row of the `trades` table (sql/001_schema.sql). Order Executed ('E')
// and Order Executed With Price ('C') messages are what constitute a trade
// -- a resting order got filled.
struct TradeRow {
    uint64_t timestamp_ns;
    uint16_t stock_locate;
    uint64_t order_ref;
    uint32_t price_raw;
    uint32_t shares;
    uint64_t match_number;
};

// Resolves the fill price for Order Executed / Executed-with-Price
// messages.
//
// NOTE on scope: ITCH's plain Order Executed ('E') message carries no price
// field at all -- the fill happens at the resting order's originally
// posted price, which only appears on the earlier Add Order ('A'/'F')
// message for that order_reference_number. This resolver tracks a small
// order_reference_number -> price_raw map from 'A'/'F' (updated on
// 'D'/'U' replace) purely to resolve that price. This is *not* an order
// book -- no side/depth/quantity state, just enough to answer "what price
// was this order resting at". See src/orderbook/ for the real order book.
class TradePriceResolver {
public:
    // Updates internal price-resolution state for every message type (Add
    // Order / Delete / Replace) and returns a TradeRow for Order Executed /
    // Executed-with-Price if (and only if) the fill price could be
    // resolved; returns std::nullopt for every other message type, and for
    // an Order Executed whose resting price is unknown (see
    // unresolved_execution_count()).
    std::optional<TradeRow> resolve(const ItchMessage& msg);

    // Count of Order Executed ('E') messages seen whose originating price
    // could not be resolved (e.g. the Add Order predates the capture
    // window) and were therefore skipped rather than produced with a
    // fabricated price. Exposed for diagnostics.
    size_t unresolved_execution_count() const;

private:
    std::unordered_map<uint64_t, uint32_t> resting_order_price_;
    size_t unresolved_execution_count_ = 0;
};

// Buffers incoming messages -- mapping each to a MessageRow (always,
// via to_message_row()) and, via TradePriceResolver, optionally a TradeRow
// -- and writes both buffers in batches. Each batch is wrapped in exactly
// ONE shared transaction spanning both `messages` and `trades`:
// start_transaction(), every buffered message row's insert (including the
// 'R'-row `symbols` upsert), every buffered trade row's insert, commit() --
// or rollback() and rethrow if any insert in the batch fails. This means
// messages and trades produced from the same batch always commit or roll
// back together, never independently on separate schedules.
class IngestWriter {
public:
    explicit IngestWriter(DbConnection& conn, size_t batch_size = kDefaultBatchSize);

    // Buffers a MessageRow for `msg` (always) and, if resolvable, a
    // TradeRow; auto-flushes once the number of messages processed (not
    // trades produced -- most messages don't produce a trade row) reaches
    // batch_size.
    void write(const ItchMessage& msg);

    // Writes any buffered rows now, regardless of batch size. Call at
    // end-of-stream so nothing is left unwritten.
    void flush();

    // Delegates to the internal TradePriceResolver.
    size_t unresolved_execution_count() const;

private:
    void flush_batch();

    DbConnection& conn_;
    size_t batch_size_;
    std::vector<MessageRow> message_buffer_;
    std::vector<TradeRow> trade_buffer_;
    TradePriceResolver resolver_;
};

} // namespace mde
