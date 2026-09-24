#pragma once

#include "persistence/db_connection.h"

#include <mysqlx/xdevapi.h>

#include <chrono>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace mde {

// Column names, row data, and wall-clock timing for one executed query.
// wall_time is used by the benchmarking harness (bench/bench_queries.cpp)
// for EXPLAIN/before-after-indexing comparisons.
struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<std::vector<mysqlx::Value>> rows;
    std::chrono::microseconds wall_time{0};
};

namespace detail {

// Pulls column names and all rows out of an already-executed SqlResult and
// stamps wall_time as elapsed time since `start`. Not a template -- how the
// rows/columns are collected doesn't depend on what parameter types
// run_query() was instantiated with, so this is implemented once in
// query_runner.cpp rather than re-emitted per instantiation.
QueryResult collect_result(mysqlx::SqlResult& sql_result, std::chrono::steady_clock::time_point start);

} // namespace detail

// Executes `sql_text` against `conn`, binding `params_to_bind` in order (the
// same `.bind()` pattern used throughout src/persistence/) if any are
// given, and materializes every row into a QueryResult. wall_time spans
// from just before execute() to just after the last row has been pulled
// out of the result -- X DevAPI results are lazily streamed, so the real
// cost of "running this query" isn't fully paid until rows are actually
// fetched.
template <typename... Params>
QueryResult run_query(DbConnection& conn, const std::string& sql_text, Params&&... params_to_bind) {
    auto start = std::chrono::steady_clock::now();
    mysqlx::SqlStatement stmt = conn.sql(sql_text);

    if constexpr (sizeof...(Params) > 0) {
        mysqlx::SqlResult sql_result = stmt.bind(std::forward<Params>(params_to_bind)...).execute();
        return detail::collect_result(sql_result, start);
    } else {
        mysqlx::SqlResult sql_result = stmt.execute();
        return detail::collect_result(sql_result, start);
    }
}

// Minimal readable-table formatter: column names on the first line, then
// one tab-separated line per row, then a row-count/timing summary line.
// Not a general-purpose formatter -- good enough for eyeballing analytics
// output during verification, not a replacement for a real reporting tool.
void print_query_result(const QueryResult& result, std::ostream& out);

} // namespace mde
