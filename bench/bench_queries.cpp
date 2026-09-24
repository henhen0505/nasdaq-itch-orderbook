// Query latency benchmark, before and after indexing.
//
// Runs all 6 analytics queries (src/analytics/queries.h) via run_query()
// (src/analytics/query_runner.h), capturing wall_time and EXPLAIN output
// against the current (no secondary index) schema; applies
// sql/002_indexes.sql; re-runs the same 6 queries and EXPLAINs; prints a
// before/after comparison (wall-clock time, speedup factor, and what
// changed in EXPLAIN's type/key/rows/Extra columns). Safe to re-run: if the
// indexes already exist (e.g. a prior run already applied them), the
// "Duplicate key name" error from CREATE INDEX is caught and treated as
// "already applied", not a failure. Requires MDE_DB_PASSWORD -- see
// DbConfig::from_env(). Runs against the real market_data_engine database;
// read-only except for the index creation in step 3 -- no rows are
// inserted, modified, or deleted by this program.

#include "analytics/queries.h"
#include "analytics/query_runner.h"
#include "persistence/db_config.h"
#include "persistence/db_connection.h"
#include "util/sql_file.h"

#include <mysqlx/xdevapi.h>

#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef MDE_PROJECT_ROOT
#error "MDE_PROJECT_ROOT must be defined by the build -- see CMakeLists.txt"
#endif

using namespace mde;

namespace {

struct NamedQuery {
    std::string name;
    std::string sql;
};

std::vector<NamedQuery> named_queries() {
    return {
        {"Q1_vwap_by_minute", queries::kVwapByMinute},
        {"Q2_rolling_midpoint_volatility", queries::kRollingMidpointVolatility},
        {"Q3_spread_depth_by_bucket", queries::kSpreadDepthByBucket},
        {"Q4_order_to_trade_ratio", queries::kOrderToTradeRatio},
        {"Q5_imbalance_regime_vs_forward_move", queries::kImbalanceRegimeVsForwardMove},
        {"Q6_intraday_volume_profile", queries::kIntradayVolumeProfile},
    };
}

void apply_index_sql(DbConnection& conn, const std::string& path) {
    for (const auto& statement : split_statements(read_file(path))) {
        try {
            conn.execute(statement);
            std::cout << "[OK] " << statement << "\n";
        } catch (const mysqlx::Error& e) {
            std::string msg = e.what();
            if (msg.find("Duplicate key name") != std::string::npos) {
                std::cout << "[SKIP] already applied: " << statement << "\n";
            } else {
                throw;
            }
        }
    }
}

// --- EXPLAIN summarization -------------------------------------------

size_t find_column(const QueryResult& r, const std::string& name) {
    for (size_t i = 0; i < r.column_names.size(); ++i) {
        if (r.column_names[i] == name) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

std::string value_to_string(const mysqlx::Value& v) {
    if (v.isNull()) {
        return "NULL";
    }
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

std::string column_or(const QueryResult& r, const std::vector<mysqlx::Value>& row, const std::string& name) {
    size_t idx = find_column(r, name);
    return idx == static_cast<size_t>(-1) ? "?" : value_to_string(row[idx]);
}

// One line per EXPLAIN result row (multiple rows are normal for a JOIN or a
// query with CTEs/derived tables) -- table/type/key/rows/Extra are the
// columns that matter for "did the index actually get used".
std::string summarize_explain(const QueryResult& explain_result) {
    std::ostringstream oss;
    for (size_t i = 0; i < explain_result.rows.size(); ++i) {
        if (i > 0) {
            oss << " | ";
        }
        const auto& row = explain_result.rows[i];
        oss << "table=" << column_or(explain_result, row, "table") << " type=" << column_or(explain_result, row, "type")
            << " key=" << column_or(explain_result, row, "key") << " rows=" << column_or(explain_result, row, "rows")
            << " Extra=" << column_or(explain_result, row, "Extra");
    }
    return oss.str();
}

struct Measurement {
    std::string name;
    std::chrono::microseconds wall_time{0};
    std::string explain_summary;
};

std::vector<Measurement> measure_all(DbConnection& conn) {
    std::vector<Measurement> out;
    for (const auto& nq : named_queries()) {
        QueryResult result = run_query(conn, nq.sql);
        QueryResult explain_result = run_query(conn, "EXPLAIN " + nq.sql);
        out.push_back({nq.name, result.wall_time, summarize_explain(explain_result)});
    }
    return out;
}

void print_measurements(const std::vector<Measurement>& measurements) {
    for (const auto& m : measurements) {
        std::cout << m.name << ": " << m.wall_time.count() << " us\n  EXPLAIN: " << m.explain_summary << "\n";
    }
}

} // namespace

int main() {
    try {
        DbConnection conn(DbConfig::from_env());

        std::cout << "=== Part B: query latency BEFORE indexing ===\n";
        std::vector<Measurement> before = measure_all(conn);
        print_measurements(before);

        std::cout << "\n=== Applying sql/002_indexes.sql ===\n";
        const std::string index_sql_path = std::string(MDE_PROJECT_ROOT) + "/sql/002_indexes.sql";
        apply_index_sql(conn, index_sql_path);

        std::cout << "\n=== Part B: query latency AFTER indexing ===\n";
        std::vector<Measurement> after = measure_all(conn);
        print_measurements(after);

        std::cout << "\n=== Comparison ===\n";
        std::cout << "query\tbefore_ms\tafter_ms\tspeedup\n";
        bool any_speedup = false;
        for (size_t i = 0; i < before.size(); ++i) {
            double before_ms = before[i].wall_time.count() / 1000.0;
            double after_ms = after[i].wall_time.count() / 1000.0;
            double speedup = after_ms > 0.0 ? before_ms / after_ms : 0.0;
            if (speedup > 1.05) { // more than a 5% improvement -- not noise
                any_speedup = true;
            }
            std::cout << before[i].name << "\t" << before_ms << "\t" << after_ms << "\t" << speedup << "x\n";
        }

        if (!any_speedup) {
            std::cout << "\n[NOTE] no query showed a >5% speedup after indexing -- see docs/performance_log.md "
                          "for the honest investigation (dataset size vs. MySQL's optimizer cost model), not a "
                          "misleading metric substituted in its place.\n";
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
