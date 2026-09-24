#pragma once

#include "persistence/db_connection.h"

#include <string>

namespace mde {

// Parses `path` (a decompressed ITCH 5.0 binary file -- see parse_file()
// in src/parser/itch_parser.h) and drives three consumers off the same
// message stream, in this order per message:
//   1. IngestWriter::write() -- always produces a `messages` row, and
//      additionally a `trades` row when an Order Executed / Executed-with-
//      Price message's fill price is resolvable.
//   2. BookManager::process() -- reconstructs per-symbol order-book state
//      in memory (no database writes of its own) and reports which
//      stock_locate it just mutated, if any.
//   3. Snapshot cadence policy (lives here, not inside BookManager or
//      SnapshotWriter, so each of those stays single-purpose): every 50
//      book-mutating messages for a given stock_locate, writes one
//      `book_snapshots` row for that symbol's current book state via
//      SnapshotWriter::write().
// IngestWriter and SnapshotWriter each batch/transact independently and are
// both flushed at end-of-stream so nothing is left buffered -- see
// SnapshotWriter's class comment (src/persistence/snapshot_writer.h) for why
// it does not share IngestWriter's transaction.
void load_itch_file(const std::string& path, DbConnection& conn);

} // namespace mde
