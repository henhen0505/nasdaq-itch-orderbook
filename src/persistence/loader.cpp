#include "persistence/loader.h"

#include "orderbook/book_manager.h"
#include "orderbook/order_book.h"
#include "parser/itch_parser.h"
#include "persistence/ingest_writer.h"
#include "persistence/snapshot_writer.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <variant>

namespace mde {

namespace {

// Every this many book-mutating messages for a given stock_locate, emit one
// snapshot for that symbol. See loader.h for why this cadence policy lives
// here rather than inside BookManager or SnapshotWriter.
constexpr size_t kSnapshotCadence = 50;

// ItchMessage is a std::variant over all 9 message structs; every one of
// them carries a `header.timestamp_ns` field, so this works uniformly
// without needing per-type dispatch.
uint64_t message_timestamp_ns(const ItchMessage& msg) {
    return std::visit([](auto&& m) { return m.header.timestamp_ns; }, msg);
}

} // namespace

void load_itch_file(const std::string& path, DbConnection& conn) {
    IngestWriter ingest_writer(conn);
    BookManager book_manager;
    SnapshotWriter snapshot_writer(conn);
    std::unordered_map<uint16_t, size_t> mutation_counts;

    // parse_file() throws BufferUnderrunError on a truncated final frame --
    // an expected condition for a file that was deliberately range-fetched
    // (e.g. this project's own smoke-test samples), not just a hypothetical.
    // Both writers must still be flushed on that path: IngestWriter
    // auto-flushes every 5000 rows and usually has little left buffered by
    // the time a truncation hits, but SnapshotWriter's buffer fills far more
    // slowly (one row per kSnapshotCadence messages per symbol) and can
    // easily still hold everything it has ever produced when the exception
    // arrives -- without this, a truncated file silently discards the
    // entire snapshot run while messages/trades mostly survive. Flush both,
    // then rethrow so the caller still learns the load was incomplete.
    try {
        parse_file(path, [&](const ItchMessage& msg) {
            ingest_writer.write(msg);

            std::optional<uint16_t> mutated_locate = book_manager.process(msg);
            if (!mutated_locate.has_value()) {
                return;
            }

            size_t& count = mutation_counts[*mutated_locate];
            ++count;
            if (count < kSnapshotCadence) {
                return;
            }

            const OrderBook* book = book_manager.get_book(*mutated_locate);
            if (book == nullptr) {
                // Invariant: BookManager::process() just reported mutating this
                // exact stock_locate, so its book must exist. A nullptr here
                // means BookManager's own bookkeeping is broken, not a data
                // issue -- fail loudly rather than silently skipping a snapshot.
                throw std::logic_error(
                    "load_itch_file: BookManager::get_book returned nullptr for a stock_locate "
                    "it just reported mutating");
            }

            snapshot_writer.write(message_timestamp_ns(msg), *mutated_locate, *book);
            count = 0;
        });
    } catch (...) {
        ingest_writer.flush();
        snapshot_writer.flush();
        throw;
    }

    ingest_writer.flush();
    snapshot_writer.flush();

    if (ingest_writer.unresolved_execution_count() > 0) {
        std::cerr << "[WARN] load_itch_file: " << ingest_writer.unresolved_execution_count()
                  << " Order Executed message(s) had no resolvable resting price "
                     "(originating Add Order predates the capture window) -- skipped, not written to trades\n";
    }
}

} // namespace mde
