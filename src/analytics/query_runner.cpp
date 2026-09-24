#include "analytics/query_runner.h"

namespace mde {

namespace detail {

QueryResult collect_result(mysqlx::SqlResult& sql_result, std::chrono::steady_clock::time_point start) {
    QueryResult result;

    mysqlx::col_count_t col_count = sql_result.getColumnCount();
    result.column_names.reserve(col_count);
    for (mysqlx::col_count_t i = 0; i < col_count; ++i) {
        // getColumnLabel(), not getColumnName(): the latter is the
        // underlying source column (meaningless -- often empty -- for a
        // computed/aggregate expression like `SUM(...) AS vwap`), while
        // getColumnLabel() is the actual AS-alias / display name, which is
        // what every query in sql/003_analytics.sql relies on for its
        // result columns.
        result.column_names.push_back(std::string(sql_result.getColumn(i).getColumnLabel()));
    }

    for (mysqlx::Row row : sql_result) {
        std::vector<mysqlx::Value> row_values;
        row_values.reserve(col_count);
        for (mysqlx::col_count_t i = 0; i < col_count; ++i) {
            row_values.push_back(row[i]);
        }
        result.rows.push_back(std::move(row_values));
    }

    result.wall_time =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

    return result;
}

} // namespace detail

void print_query_result(const QueryResult& result, std::ostream& out) {
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (i > 0) {
            out << "\t";
        }
        out << result.column_names[i];
    }
    out << "\n";

    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) {
                out << "\t";
            }
            out << row[i];
        }
        out << "\n";
    }

    out << "(" << result.rows.size() << " row(s), " << result.wall_time.count() << " us)\n";
}

} // namespace mde
