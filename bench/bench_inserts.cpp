// Insert throughput comparison -- unbatched (autocommit) vs.
// batched-transaction vs. multi-row INSERT, all against the same real
// `messages` table (same schema, same FK to `symbols` as production) under a
// dedicated benchmark stock_locate. All three methods still use `.bind()`
// parameterized statements; this
// benchmark is about transaction/batching strategy, not about regressing
// that practice.
//
// kBenchStockLocate is out of range of both the real 5-ticker allowlist's
// resolved locates (13/5291/3459/393/5708) and test_analytics.cpp's
// reserved synthetic range (60000-60999).
//
// !!! MUST stay within SMALLINT UNSIGNED (0-65535) -- stock_locate's actual
// column type (sql/001_schema.sql), which is exactly `uint16_t`'s range.
// An earlier version of this file set kBenchStockLocate = 70000. Even
// though the variable's type is `uint16_t`, plain (non-list-initialized)
// assignment of an out-of-range literal to an unsigned type doesn't error
// in C++ -- it silently truncates modulo 2^16 at compile time
// (70000 mod 65536 = 4464), so kBenchStockLocate was actually 4464 the
// entire time, a REAL symbol's locate. insert_bench_symbol()'s
// ON DUPLICATE KEY UPDATE then collided on that real row's primary key and
// real symbol's only message (its Stock Directory row). Caught by this
// benchmark's own post-cleanup row-count check, not by inspection or a
// compiler warning -- the real data had to be reloaded from
// data/itch_sample_12302019_100mb.bin to recover. Any future change to
// this constant must stay under 65536, and ideally should be checked
// against the real data's actual max locate (currently 8906 -- query
// `SELECT MAX(stock_locate) FROM symbols`) rather than assumed safe.
//
// Every benchmark-inserted row is deleted after every individual run (not
// just after each method finishes all its runs), so no run's timing is
// skewed by rows a prior run left behind, and the real loaded data's row
// counts are verified unchanged (not just assumed) after every cleanup and
// again at the very end. Requires MDE_DB_PASSWORD -- see DbConfig::from_env().

#include "bench_util.h"
#include "persistence/db_config.h"
#include "persistence/db_connection.h"

#include <mysqlx/xdevapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace mde;

namespace {

// Configurable, not hardcoded inline below.
constexpr size_t kRowsPerMethod = 100000;
constexpr size_t kRunsPerMethod = 3;

// Rows per transaction (Method 2) / rows per multi-row INSERT batch
// (Method 3). Matches IngestWriter::kDefaultBatchSize
// (src/persistence/ingest_writer.h) so Method 2 mirrors IngestWriter's
// actual batching discipline exactly, and Method 3 uses the same batch
// boundary so the two "further tuned" comparisons share a batch size.
constexpr size_t kTransactionBatchSize = 5000;

constexpr uint16_t kBenchStockLocate = 65000;
constexpr const char* kBenchTicker = "BENCH";

// Same column set to_message_row() (src/persistence/ingest_writer.cpp)
// populates for an Add Order ('A') message -- a realistic production row
// shape, not an arbitrary bench-only schema.
const char* kInsertMessageSql =
    "INSERT INTO messages (msg_type, timestamp_ns, stock_locate, order_ref, side, shares, price_raw) "
    "VALUES (?, ?, ?, ?, ?, ?, ?)";

struct BenchRow {
    uint64_t timestamp_ns;
    uint64_t order_ref;
    char side;
    uint32_t shares;
    uint32_t price_raw;
};

std::vector<BenchRow> make_rows(size_t n, uint64_t order_ref_base) {
    std::vector<BenchRow> rows;
    rows.reserve(n);
    constexpr uint64_t kBaseTs = 34200000000000ull; // 09:30 ET, matches the real data's session start
    for (size_t i = 0; i < n; ++i) {
        BenchRow row{};
        row.timestamp_ns = kBaseTs + i;
        row.order_ref = order_ref_base + i;
        row.side = (i % 2 == 0) ? 'B' : 'S';
        row.shares = 100;
        row.price_raw = 1000000 + static_cast<uint32_t>(i % 1000);
        rows.push_back(row);
    }
    return rows;
}

mysqlx::Value bind_field(const BenchRow& row, int field) {
    switch (field) {
        case 0: return mysqlx::Value(std::string("A"));
        case 1: return mysqlx::Value(row.timestamp_ns);
        case 2: return mysqlx::Value(static_cast<uint64_t>(kBenchStockLocate));
        case 3: return mysqlx::Value(row.order_ref);
        case 4: return mysqlx::Value(std::string(1, row.side));
        case 5: return mysqlx::Value(row.shares);
        case 6: return mysqlx::Value(row.price_raw);
        default: throw std::logic_error("bind_field: bad field index");
    }
}

uint64_t count_rows(DbConnection& conn, const std::string& table) {
    static const std::unordered_set<std::string> kAllowed = {
        "symbols", "messages", "trades", "book_snapshots"
    };
    if (kAllowed.find(table) == kAllowed.end()) {
        throw std::invalid_argument("count_rows: table name not in allowlist: " + table);
    }
    mysqlx::SqlResult result = conn.sql("SELECT COUNT(*) FROM " + table).execute();
    mysqlx::Row row = result.fetchOne();
    return row[0].get<uint64_t>();
}

void insert_bench_symbol(DbConnection& conn) {
    conn.sql("INSERT INTO symbols (stock_locate, ticker) VALUES (?, ?) "
             "ON DUPLICATE KEY UPDATE ticker = VALUES(ticker)")
        .bind(mysqlx::Value(static_cast<uint64_t>(kBenchStockLocate)), mysqlx::Value(std::string(kBenchTicker)))
        .execute();
}

void delete_bench_symbol(DbConnection& conn) {
    conn.sql("DELETE FROM symbols WHERE stock_locate = ?")
        .bind(mysqlx::Value(static_cast<uint64_t>(kBenchStockLocate)))
        .execute();
}

void delete_bench_messages(DbConnection& conn) {
    conn.sql("DELETE FROM messages WHERE stock_locate = ?")
        .bind(mysqlx::Value(static_cast<uint64_t>(kBenchStockLocate)))
        .execute();
}

// --- Method 1: unbatched, one execute() per row, autocommit --------------
// No start_transaction()/commit() wraps any group of rows -- each INSERT is
// its own implicit transaction under MySQL's default autocommit=1, which
// this codebase never disables.
std::chrono::duration<double> run_unbatched(DbConnection& conn, const std::vector<BenchRow>& rows) {
    auto start = std::chrono::steady_clock::now();
    for (const auto& row : rows) {
        conn.sql(kInsertMessageSql)
            .bind(bind_field(row, 0), bind_field(row, 1), bind_field(row, 2), bind_field(row, 3),
                  bind_field(row, 4), bind_field(row, 5), bind_field(row, 6))
            .execute();
    }
    return std::chrono::steady_clock::now() - start;
}

// --- Method 2: batched (transaction), one execute() per row ---------------
// Identical per-row INSERT/bind/execute() to Method 1, but kTransactionBatchSize
// rows share one start_transaction()/commit() -- exactly IngestWriter's
// flush_batch() pattern (src/persistence/ingest_writer.cpp), including its
// rollback()-and-rethrow-on-failure shape.
std::chrono::duration<double> run_batched_transaction(DbConnection& conn, const std::vector<BenchRow>& rows) {
    auto start = std::chrono::steady_clock::now();
    for (size_t offset = 0; offset < rows.size(); offset += kTransactionBatchSize) {
        size_t end = std::min(offset + kTransactionBatchSize, rows.size());

        conn.start_transaction();
        try {
            for (size_t i = offset; i < end; ++i) {
                const BenchRow& row = rows[i];
                conn.sql(kInsertMessageSql)
                    .bind(bind_field(row, 0), bind_field(row, 1), bind_field(row, 2), bind_field(row, 3),
                          bind_field(row, 4), bind_field(row, 5), bind_field(row, 6))
                    .execute();
            }
            conn.commit();
        } catch (...) {
            conn.rollback();
            throw;
        }
    }
    return std::chrono::steady_clock::now() - start;
}

// --- Method 3: multi-row INSERT, one execute() per batch -------------------
// Builds ONE INSERT statement with `n` value-tuples and binds all of that
// batch's parameters via a single .bind(vector<Value>) call (X DevAPI's
// bind() accepts a container and iterates it -- see
// devapi/detail/crud.h's Args_processor -- so this is still genuinely
// parameterized, not string concatenation), still wrapped in one
// start_transaction()/commit() per batch.
std::string build_multi_row_insert_sql(size_t n) {
    std::string sql = "INSERT INTO messages (msg_type, timestamp_ns, stock_locate, order_ref, side, shares, price_raw) VALUES ";
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += "(?, ?, ?, ?, ?, ?, ?)";
    }
    return sql;
}

std::chrono::duration<double> run_multi_row_insert(DbConnection& conn, const std::vector<BenchRow>& rows) {
    static const std::string kFullBatchSql = build_multi_row_insert_sql(kTransactionBatchSize);

    auto start = std::chrono::steady_clock::now();
    for (size_t offset = 0; offset < rows.size(); offset += kTransactionBatchSize) {
        size_t end = std::min(offset + kTransactionBatchSize, rows.size());
        size_t batch_n = end - offset;
        // kRowsPerMethod is evenly divisible by kTransactionBatchSize today,
        // so this only ever takes the pre-built kFullBatchSql path -- the
        // fallback keeps this correct if either constant is changed later.
        const std::string sql_text = (batch_n == kTransactionBatchSize) ? kFullBatchSql : build_multi_row_insert_sql(batch_n);

        std::vector<mysqlx::Value> params;
        params.reserve(batch_n * 7);
        for (size_t i = offset; i < end; ++i) {
            const BenchRow& row = rows[i];
            for (int field = 0; field < 7; ++field) {
                params.push_back(bind_field(row, field));
            }
        }

        conn.start_transaction();
        try {
            conn.sql(sql_text).bind(params).execute();
            conn.commit();
        } catch (...) {
            conn.rollback();
            throw;
        }
    }
    return std::chrono::steady_clock::now() - start;
}

// --- Orchestration ----------------------------------------------------

struct MethodResult {
    std::string name;
    std::vector<double> rows_per_sec;
};

using InsertFn = std::function<std::chrono::duration<double>(DbConnection&, const std::vector<BenchRow>&)>;

MethodResult run_method(DbConnection& conn, const std::string& name, const InsertFn& fn, uint64_t baseline_messages) {
    MethodResult result{name, {}};

    for (size_t run = 0; run < kRunsPerMethod; ++run) {
        // Distinct order_ref ranges per run -- not required for correctness
        // (no uniqueness constraint on messages.order_ref), just avoids any
        // accidental cross-run collision in case that ever matters later.
        uint64_t order_ref_base = 1000000000ull * static_cast<uint64_t>(run + 1);
        std::vector<BenchRow> rows = make_rows(kRowsPerMethod, order_ref_base);

        std::chrono::duration<double> elapsed = fn(conn, rows);
        if (elapsed.count() <= 0.0) {
            throw std::runtime_error(name + ": measured elapsed time was non-positive -- clock resolution issue");
        }
        double rate = static_cast<double>(kRowsPerMethod) / elapsed.count();
        result.rows_per_sec.push_back(rate);

        // Cleanup discipline mirrors AnalyticsExactTest (tests/test_analytics.cpp):
        // delete every benchmark-inserted row, then verify (not assume) the
        // real data's row count is exactly back to baseline before the next run.
        delete_bench_messages(conn);
        uint64_t after = count_rows(conn, "messages");
        if (after != baseline_messages) {
            throw std::runtime_error(name + ": messages row count did not return to baseline after cleanup (" +
                                      std::to_string(after) + " != " + std::to_string(baseline_messages) + ")");
        }
    }

    return result;
}

void print_result(const MethodResult& r) {
    double m = bench::mean(r.rows_per_sec);
    double sd = bench::sample_stddev(r.rows_per_sec, m);
    std::cout << r.name << ": mean=" << m << " rows/sec, stddev=" << sd << ", runs=" << r.rows_per_sec.size()
              << "\n";
    for (size_t i = 0; i < r.rows_per_sec.size(); ++i) {
        std::cout << "  run " << (i + 1) << ": " << r.rows_per_sec[i] << " rows/sec\n";
    }
}

} // namespace

int main() {
    try {
        DbConnection conn(DbConfig::from_env());

        // Defensive pre-cleanup, run before baseline is captured: if a
        // prior run of this program was killed externally (process/service
        // interruption, not a normal exception this program's own
        // try/catch would have handled), it may have left stray rows at
        // kBenchStockLocate behind -- this happened for real (2026-08-06):
        // an interrupted run left one stray `symbols` row and a partial
        // batch of `messages` rows uncleaned, which the next run's naive
        // "capture baseline, then compare after cleanup" logic had no way
        // to distinguish from real data, producing a confusing false-alarm
        // failure even though the actual real data was fine. Cleaning up
        // any stray bench rows *before* reading the baseline makes this
        // program self-healing across an external interruption instead of
        // requiring a manual investigation each time one happens.
        delete_bench_messages(conn);
        delete_bench_symbol(conn);

        uint64_t baseline_symbols = count_rows(conn, "symbols");
        uint64_t baseline_messages = count_rows(conn, "messages");
        uint64_t baseline_trades = count_rows(conn, "trades");
        uint64_t baseline_snapshots = count_rows(conn, "book_snapshots");

        std::cout << "Baseline row counts -- symbols=" << baseline_symbols << " messages=" << baseline_messages
                  << " trades=" << baseline_trades << " book_snapshots=" << baseline_snapshots << "\n\n";

        insert_bench_symbol(conn);

        std::vector<MethodResult> results;
        results.push_back(
            run_method(conn, "1_unbatched_autocommit", run_unbatched, baseline_messages));
        results.push_back(
            run_method(conn, "2_batched_transaction", run_batched_transaction, baseline_messages));
        results.push_back(
            run_method(conn, "3_multi_row_insert", run_multi_row_insert, baseline_messages));

        delete_bench_symbol(conn);

        uint64_t final_symbols = count_rows(conn, "symbols");
        uint64_t final_messages = count_rows(conn, "messages");
        uint64_t final_trades = count_rows(conn, "trades");
        uint64_t final_snapshots = count_rows(conn, "book_snapshots");

        std::cout << "\n=== Results (rows/sec, " << kRowsPerMethod << " rows/run, " << kRunsPerMethod
                  << " runs/method) ===\n";
        for (const auto& r : results) {
            print_result(r);
        }

        std::cout << "\nFinal row counts (should equal baseline) -- symbols=" << final_symbols
                  << " messages=" << final_messages << " trades=" << final_trades
                  << " book_snapshots=" << final_snapshots << "\n";

        if (final_symbols != baseline_symbols || final_messages != baseline_messages ||
            final_trades != baseline_trades || final_snapshots != baseline_snapshots) {
            std::cerr << "[FAIL] real data row counts changed after the benchmark's cleanup\n";
            return 1;
        }

        std::cout << "[OK] real data row counts unchanged after the benchmark's cleanup\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
