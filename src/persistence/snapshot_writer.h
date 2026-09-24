#pragma once

#include "orderbook/order_book.h"
#include "persistence/db_connection.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mde {

// Default number of snapshot rows buffered before an automatic flush.
constexpr size_t kDefaultSnapshotBatchSize = 5000;

// One row of the `book_snapshots` table (sql/001_schema.sql).
struct SnapshotRow {
    uint64_t timestamp_ns;
    uint16_t stock_locate;
    std::optional<uint32_t> best_bid_raw;
    std::optional<uint32_t> best_ask_raw;
    uint32_t bid_qty_at_best;
    uint32_t ask_qty_at_best;
};

// Writes periodic order-book snapshots to `book_snapshots`, following the
// same shape as IngestWriter (src/persistence/ingest_writer.h): batched,
// parameterized `.bind()` inserts, one start_transaction()/commit() per
// batch, rollback() + rethrow on failure, flush() for end-of-stream.
//
// Design note -- why this is a SEPARATE writer/transaction from
// IngestWriter, not merged into it the way the earlier audit pass merged
// MessageWriter/TradeWriter into IngestWriter: that merge was necessary
// because a `messages` row and the `trades` row it produces are two rows
// *derived from the same event* (one execution message) with a genuine
// atomicity requirement -- either both commit or neither does, because
// they represent one fact about the world (a fill happened). A
// book_snapshots row has no such relationship to the message stream. It's
// a periodic *sample* of aggregate book state (see the cadence policy in
// src/persistence/loader.cpp), not something derived 1:1 from a single
// message. There is no equivalent expectation that "the exact set of
// messages committed so far" and "the latest snapshot" must be atomic with
// each other -- a snapshot slightly behind or ahead of the last committed
// message batch is expected and harmless. Keeping SnapshotWriter's
// batch/transaction independent of IngestWriter's is therefore the correct
// design here, not a regression of that earlier fix.
class SnapshotWriter {
public:
    explicit SnapshotWriter(DbConnection& conn, size_t batch_size = kDefaultSnapshotBatchSize);

    // Buffers one snapshot row for `stock_locate` at `timestamp_ns`, pulled
    // from `book`'s current best_bid()/best_ask()/bid_qty_at_best()/
    // ask_qty_at_best(). best_bid()/best_ask() empty (that side has no
    // resting orders) -> SQL NULL for best_bid_raw/best_ask_raw, matching
    // the nullable columns in sql/001_schema.sql. Auto-flushes once the
    // buffered row count reaches batch_size.
    void write(uint64_t timestamp_ns, uint16_t stock_locate, const OrderBook& book);

    // Writes any buffered rows now, regardless of batch size. Call at
    // end-of-stream so nothing is left unwritten.
    void flush();

private:
    void flush_batch();

    DbConnection& conn_;
    size_t batch_size_;
    std::vector<SnapshotRow> buffer_;
};

} // namespace mde
