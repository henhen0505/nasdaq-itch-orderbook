#include "persistence/bind_helpers.h"
#include "persistence/snapshot_writer.h"

#include <mysqlx/xdevapi.h>

namespace mde {

namespace {

const char* kInsertSnapshotSql =
    "INSERT INTO book_snapshots "
    "(timestamp_ns, stock_locate, best_bid_raw, best_ask_raw, bid_qty_at_best, ask_qty_at_best) "
    "VALUES (?, ?, ?, ?, ?, ?)";

} // namespace

SnapshotWriter::SnapshotWriter(DbConnection& conn, size_t batch_size) : conn_(conn), batch_size_(batch_size) {
}

void SnapshotWriter::write(uint64_t timestamp_ns, uint16_t stock_locate, const OrderBook& book) {
    SnapshotRow row{};
    row.timestamp_ns = timestamp_ns;
    row.stock_locate = stock_locate;
    row.best_bid_raw = book.best_bid();
    row.best_ask_raw = book.best_ask();
    row.bid_qty_at_best = book.bid_qty_at_best();
    row.ask_qty_at_best = book.ask_qty_at_best();

    buffer_.push_back(row);

    if (buffer_.size() >= batch_size_) {
        flush_batch();
    }
}

void SnapshotWriter::flush() {
    flush_batch();
}

void SnapshotWriter::flush_batch() {
    if (buffer_.empty()) {
        return;
    }

    conn_.start_transaction();
    try {
        for (const auto& row : buffer_) {
            conn_.sql(kInsertSnapshotSql)
                .bind(mysqlx::Value(row.timestamp_ns),
                      mysqlx::Value(static_cast<uint64_t>(row.stock_locate)),
                      to_value(row.best_bid_raw),
                      to_value(row.best_ask_raw),
                      mysqlx::Value(row.bid_qty_at_best),
                      mysqlx::Value(row.ask_qty_at_best))
                .execute();
        }

        conn_.commit();
    } catch (...) {
        conn_.rollback();
        throw;
    }

    buffer_.clear();
}

} // namespace mde
