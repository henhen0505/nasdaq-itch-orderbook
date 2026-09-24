// Analytics layer tests.
//
//  1. QueriesMatchSqlFile -- pure file-comparison test (no DB needed):
//     asserts src/analytics/queries.h's 6 string constants match
//     sql/003_analytics.sql so the two can never silently drift apart.
//  2. AnalyticsExactTest -- queries 1 (VWAP) and 4 (order-to-trade ratio),
//     exact hand-computed correctness against synthetic data in a dedicated
//     stock_locate range (60000-60999, disjoint from real data). Requires
//     MDE_DB_PASSWORD.
//  3. AnalyticsRealDataTest -- queries 2, 3, 5, 6, plausibility tests
//     against the real loaded data. Does NOT drop or recreate any table.
//     Also requires MDE_DB_PASSWORD.

#include "analytics/queries.h"
#include "analytics/query_runner.h"
#include "persistence/db_config.h"
#include "persistence/db_connection.h"
#include "util/sql_file.h"

#include <gtest/gtest.h>
#include <mysqlx/xdevapi.h>

#include <cmath>
#include <string>
#include <vector>

#ifndef MDE_PROJECT_ROOT
#error "MDE_PROJECT_ROOT must be defined by the build -- see CMakeLists.txt"
#endif

using namespace mde;

namespace {

// --- QueriesMatchSqlFile helpers (no DB needed) ---------------------------

// Collapses all whitespace runs to a single space and trims the ends, so
// two SQL bodies that differ only in indentation/line breaks compare equal.
std::string normalize_whitespace(const std::string& text) {
    std::string result;
    bool last_was_space = true; // true so leading whitespace is dropped
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!last_was_space) {
                result += ' ';
            }
            last_was_space = true;
        } else {
            result += c;
            last_was_space = false;
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

// --- Live-DB test helpers (mirror test_persistence.cpp's style) -----------

// Dedicated locate range for synthetic test data -- guaranteed disjoint
// from the real 5-ticker allowlist's resolved locates (13, 5291, 3459, 393,
// 5708), so exact-correctness tests never see real rows in their GROUP BY
// output, and TearDown's cleanup can never delete real data.
constexpr uint16_t kTestLocateRangeStart = 60000;
constexpr uint16_t kTestLocateRangeEnd = 60999;

DbConnection& test_connection() {
    static DbConnection conn(DbConfig::from_env());
    return conn;
}

void insert_symbol(DbConnection& conn, uint16_t stock_locate, const std::string& ticker) {
    conn.sql("INSERT INTO symbols (stock_locate, ticker) VALUES (?, ?)")
        .bind(mysqlx::Value(static_cast<uint64_t>(stock_locate)), mysqlx::Value(ticker))
        .execute();
}

void insert_trade(DbConnection& conn, uint16_t stock_locate, uint64_t order_ref, uint32_t price_raw, uint32_t shares,
                   uint64_t match_number, uint64_t timestamp_ns) {
    conn.sql("INSERT INTO trades (timestamp_ns, stock_locate, order_ref, price_raw, shares, match_number) "
             "VALUES (?, ?, ?, ?, ?, ?)")
        .bind(mysqlx::Value(timestamp_ns), mysqlx::Value(static_cast<uint64_t>(stock_locate)),
              mysqlx::Value(order_ref), mysqlx::Value(price_raw), mysqlx::Value(shares), mysqlx::Value(match_number))
        .execute();
}

void insert_message(DbConnection& conn, char msg_type, uint16_t stock_locate, uint64_t timestamp_ns) {
    conn.sql("INSERT INTO messages (msg_type, timestamp_ns, stock_locate) VALUES (?, ?, ?)")
        .bind(mysqlx::Value(std::string(1, msg_type)), mysqlx::Value(timestamp_ns),
              mysqlx::Value(static_cast<uint64_t>(stock_locate)))
        .execute();
}

// Removes every row this test file could have inserted, across all 4
// tables, regardless of which test (or assertion failure mid-test) put
// them there -- children before the `symbols` parent so foreign keys don't
// block the delete.
void cleanup_test_locate_range(DbConnection& conn) {
    conn.sql("DELETE FROM book_snapshots WHERE stock_locate BETWEEN ? AND ?")
        .bind(mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeStart)),
              mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeEnd)))
        .execute();
    conn.sql("DELETE FROM trades WHERE stock_locate BETWEEN ? AND ?")
        .bind(mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeStart)),
              mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeEnd)))
        .execute();
    conn.sql("DELETE FROM messages WHERE stock_locate BETWEEN ? AND ?")
        .bind(mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeStart)),
              mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeEnd)))
        .execute();
    conn.sql("DELETE FROM symbols WHERE stock_locate BETWEEN ? AND ?")
        .bind(mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeStart)),
              mysqlx::Value(static_cast<uint64_t>(kTestLocateRangeEnd)))
        .execute();
}

// Finds a column by name in a QueryResult, rather than hardcoding a
// positional index -- the query text is right above every test that uses
// this, but looking up by name is still cheap insurance against a silent
// column-order mismatch.
size_t column_index(const QueryResult& result, const std::string& name) {
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (result.column_names[i] == name) {
            return i;
        }
    }
    throw std::runtime_error("column_index: no column named '" + name + "' in result");
}

// Finds the row whose `ticker` column equals `ticker_value`. Returns
// nullptr if not found.
const std::vector<mysqlx::Value>* find_row_by_ticker(const QueryResult& result, const std::string& ticker_value) {
    size_t ticker_col = column_index(result, "ticker");
    for (const auto& row : result.rows) {
        if (row[ticker_col].get<std::string>() == ticker_value) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

// =============================================================================
// QueriesMatchSqlFile -- no DB needed, always runs.
// =============================================================================

TEST(AnalyticsQueriesConsistency, QueriesMatchSqlFile) {
    const std::string path = std::string(MDE_PROJECT_ROOT) + "/sql/003_analytics.sql";
    std::string sql_file_normalized = normalize_whitespace(strip_line_comments(read_file(path)));

    std::vector<std::pair<std::string, std::string>> named_queries = {
        {"kVwapByMinute", queries::kVwapByMinute},
        {"kRollingMidpointVolatility", queries::kRollingMidpointVolatility},
        {"kSpreadDepthByBucket", queries::kSpreadDepthByBucket},
        {"kOrderToTradeRatio", queries::kOrderToTradeRatio},
        {"kImbalanceRegimeVsForwardMove", queries::kImbalanceRegimeVsForwardMove},
        {"kIntradayVolumeProfile", queries::kIntradayVolumeProfile},
    };

    for (const auto& [name, query_text] : named_queries) {
        std::string normalized_query = normalize_whitespace(query_text);
        EXPECT_NE(sql_file_normalized.find(normalized_query), std::string::npos)
            << "queries::" << name << " does not appear (mod whitespace) in sql/003_analytics.sql -- "
            << "the two have diverged.";
    }
}

// =============================================================================
// AnalyticsExactTest -- queries 1 (VWAP) and 4 (order-to-trade ratio),
// exact hand-computed correctness against synthetic data. Requires
// MDE_DB_PASSWORD.
// =============================================================================

class AnalyticsExactTest : public ::testing::Test {
protected:
    DbConnection& conn() { return test_connection(); }

    void SetUp() override { cleanup_test_locate_range(conn()); }
    void TearDown() override { cleanup_test_locate_range(conn()); }
};

TEST_F(AnalyticsExactTest, VwapIsShareWeightedNotSimpleAverage) {
    constexpr uint16_t kLocate = 60001;
    insert_symbol(conn(), kLocate, "ZTST1");

    // All three trades land in the same 60-second minute_bucket:
    // 40,000,000,000,000 DIV 60,000,000,000 == 666 for all three offsets
    // below (max timestamp used is 40,002,000,000,000, still short of the
    // 40,020,000,000,000 boundary where the bucket would roll over).
    constexpr uint64_t kBaseTs = 40000000000000ull;
    insert_trade(conn(), kLocate, 1, 1000000, 100, 901, kBaseTs); // $100.00 x 100
    insert_trade(conn(), kLocate, 2, 1010000, 200, 902, kBaseTs + 1000000000ull); // $101.00 x 200
    insert_trade(conn(), kLocate, 3, 990000, 100, 903, kBaseTs + 2000000000ull); // $99.00 x 100

    // Hand-computed: sum(price*shares)/sum(shares)/10000
    // = (1,000,000*100 + 1,010,000*200 + 990,000*100) / 400 / 10000
    // = 401,000,000 / 400 / 10000 = 100.25
    constexpr double kExpectedVwap = 100.25;
    constexpr uint64_t kExpectedVolume = 400;
    constexpr uint64_t kExpectedTradeCount = 3;

    QueryResult result = run_query(conn(), queries::kVwapByMinute);
    const std::vector<mysqlx::Value>* row = find_row_by_ticker(result, "ZTST1");
    ASSERT_NE(row, nullptr) << "no row for ZTST1 in VWAP result";

    size_t vwap_col = column_index(result, "vwap");
    size_t volume_col = column_index(result, "total_volume");
    size_t count_col = column_index(result, "trade_count");

    EXPECT_NEAR((*row)[vwap_col].get<double>(), kExpectedVwap, 1e-9);
    EXPECT_EQ((*row)[volume_col].get<uint64_t>(), kExpectedVolume);
    EXPECT_EQ((*row)[count_col].get<uint64_t>(), kExpectedTradeCount);
}

TEST_F(AnalyticsExactTest, OrderToTradeRatioAndCancelRateExactCounts) {
    constexpr uint16_t kLocate = 60002;
    insert_symbol(conn(), kLocate, "ZTST2");

    constexpr uint64_t kTs = 40000000000000ull;
    // 3x 'A' + 2x 'F' = 5 orders
    insert_message(conn(), 'A', kLocate, kTs);
    insert_message(conn(), 'A', kLocate, kTs + 1);
    insert_message(conn(), 'A', kLocate, kTs + 2);
    insert_message(conn(), 'F', kLocate, kTs + 3);
    insert_message(conn(), 'F', kLocate, kTs + 4);
    // 1x 'E' + 1x 'C' = 2 executions
    insert_message(conn(), 'E', kLocate, kTs + 5);
    insert_message(conn(), 'C', kLocate, kTs + 6);
    // 1x 'D' delete, 1x 'X' cancel
    insert_message(conn(), 'D', kLocate, kTs + 7);
    insert_message(conn(), 'X', kLocate, kTs + 8);

    // Hand-computed:
    //   total_orders = 5, total_executions = 2, total_deletes = 1, total_cancels = 1
    //   order_to_trade_ratio = 5 / 2 = 2.5
    //   cancel_rate = (1 + 1) / 5 = 0.4
    QueryResult result = run_query(conn(), queries::kOrderToTradeRatio);
    const std::vector<mysqlx::Value>* row = find_row_by_ticker(result, "ZTST2");
    ASSERT_NE(row, nullptr) << "no row for ZTST2 in order-to-trade-ratio result";

    EXPECT_EQ((*row)[column_index(result, "total_orders")].get<uint64_t>(), 5u);
    EXPECT_EQ((*row)[column_index(result, "total_executions")].get<uint64_t>(), 2u);
    EXPECT_EQ((*row)[column_index(result, "total_deletes")].get<uint64_t>(), 1u);
    EXPECT_EQ((*row)[column_index(result, "total_cancels")].get<uint64_t>(), 1u);
    EXPECT_NEAR((*row)[column_index(result, "order_to_trade_ratio")].get<double>(), 2.5, 1e-9);
    EXPECT_NEAR((*row)[column_index(result, "cancel_rate")].get<double>(), 0.4, 1e-9);
}

TEST_F(AnalyticsExactTest, OrderToTradeRatioIsNullNotErrorWhenNoExecutions) {
    constexpr uint16_t kLocate = 60003;
    insert_symbol(conn(), kLocate, "ZTST3");

    constexpr uint64_t kTs = 40000000000000ull;
    // Orders only, no executions -- order_to_trade_ratio's denominator
    // (total_executions) is 0. NULLIF must turn this into SQL NULL, not a
    // division-by-zero error or a garbage value.
    insert_message(conn(), 'A', kLocate, kTs);
    insert_message(conn(), 'A', kLocate, kTs + 1);

    QueryResult result = run_query(conn(), queries::kOrderToTradeRatio);
    const std::vector<mysqlx::Value>* row = find_row_by_ticker(result, "ZTST3");
    ASSERT_NE(row, nullptr) << "no row for ZTST3 in order-to-trade-ratio result";

    EXPECT_EQ((*row)[column_index(result, "total_orders")].get<uint64_t>(), 2u);
    EXPECT_EQ((*row)[column_index(result, "total_executions")].get<uint64_t>(), 0u);
    EXPECT_TRUE((*row)[column_index(result, "order_to_trade_ratio")].isNull());
    // cancel_rate's denominator (total_orders) is nonzero here -- 0 deletes
    // + 0 cancels over 2 orders is a real, well-defined 0.0, not NULL.
    EXPECT_NEAR((*row)[column_index(result, "cancel_rate")].get<double>(), 0.0, 1e-9);
}

// =============================================================================
// AnalyticsRealDataTest -- queries 2, 3, 5, 6, plausibility against the real
// loaded data (8,906 symbols / 55,748 messages / 1,856 trades / 934
// book_snapshots). Deliberately does NOT drop or recreate any table.
// Requires MDE_DB_PASSWORD.
// =============================================================================

class AnalyticsRealDataTest : public ::testing::Test {
protected:
    DbConnection& conn() { return test_connection(); }
};

TEST_F(AnalyticsRealDataTest, RollingVolatilityIsNonNegativeWhereDefined) {
    QueryResult result = run_query(conn(), queries::kRollingMidpointVolatility);

    // Not asserting a nonzero row count with EXPECT_GT alone would let a
    // silently-broken query (e.g. a typo'd JOIN that matches nothing) pass
    // this test vacuously -- fail loudly instead if there's no data at all.
    ASSERT_FALSE(result.rows.empty()) << "rolling volatility query returned no rows against the real loaded data";

    size_t vol_col = column_index(result, "rolling_volatility_20");
    size_t checked = 0;
    for (const auto& row : result.rows) {
        if (row[vol_col].isNull()) {
            continue; // STDDEV_SAMP is NULL for the first row of each partition (n < 2 in window)
        }
        double vol = row[vol_col].get<double>();
        EXPECT_FALSE(std::isnan(vol));
        EXPECT_FALSE(std::isinf(vol));
        EXPECT_GE(vol, 0.0) << "sample standard deviation must be non-negative";
        ++checked;
    }
    EXPECT_GT(checked, 0u) << "every rolling_volatility_20 value was NULL -- window never filled";
}

TEST_F(AnalyticsRealDataTest, SpreadSummaryIsInternallyConsistentAndNonNegative) {
    QueryResult result = run_query(conn(), queries::kSpreadDepthByBucket);
    ASSERT_FALSE(result.rows.empty()) << "spread/depth query returned no rows against the real loaded data";

    size_t avg_col = column_index(result, "avg_spread");
    size_t min_col = column_index(result, "min_spread");
    size_t max_col = column_index(result, "max_spread");
    size_t bid_depth_col = column_index(result, "avg_bid_depth");
    size_t ask_depth_col = column_index(result, "avg_ask_depth");

    for (const auto& row : result.rows) {
        double avg_spread = row[avg_col].get<double>();
        double min_spread = row[min_col].get<double>();
        double max_spread = row[max_col].get<double>();

        // A real, uncrossed order book always has best_ask >= best_bid.
        EXPECT_GE(min_spread, 0.0);
        EXPECT_LE(min_spread, avg_spread + 1e-9);
        EXPECT_LE(avg_spread, max_spread + 1e-9);
        EXPECT_GE(row[bid_depth_col].get<double>(), 0.0);
        EXPECT_GE(row[ask_depth_col].get<double>(), 0.0);
    }
}

TEST_F(AnalyticsRealDataTest, ImbalanceRegimesAreOneOfTheThreeExpectedLabelsWithPositiveCounts) {
    QueryResult result = run_query(conn(), queries::kImbalanceRegimeVsForwardMove);
    ASSERT_FALSE(result.rows.empty()) << "imbalance-regime query returned no rows against the real loaded data";

    size_t regime_col = column_index(result, "regime");
    size_t count_col = column_index(result, "observation_count");
    size_t avg_move_col = column_index(result, "avg_forward_move_bps");

    for (const auto& row : result.rows) {
        std::string regime = row[regime_col].get<std::string>();
        EXPECT_TRUE(regime == "strong_bid" || regime == "strong_ask" || regime == "balanced")
            << "unexpected regime label: " << regime;
        EXPECT_GT(row[count_col].get<uint64_t>(), 0u);

        if (!row[avg_move_col].isNull()) {
            double move_bps = row[avg_move_col].get<double>();
            EXPECT_FALSE(std::isnan(move_bps));
            EXPECT_FALSE(std::isinf(move_bps));
        }
    }
}

TEST_F(AnalyticsRealDataTest, IntradayVolumeProfileReturnsRowsGivenTheWidenedSessionWindow) {
    // Per sql/003_analytics.sql's comment: the real sample's timestamps run
    // 04:00:00.34-09:30:15.6 ET, so this query is deliberately widened to
    // NASDAQ's full 04:00-16:00 session rather than a strict 09:30-16:00
    // regular-hours filter (which would return next to nothing against
    // this dataset). Confirming actual row counts here is the point of
    // this test -- a regression back to a strict market-hours filter would
    // make this fail.
    QueryResult result = run_query(conn(), queries::kIntradayVolumeProfile);
    ASSERT_FALSE(result.rows.empty())
        << "intraday volume query returned no rows even with the widened 04:00-16:00 session filter";

    size_t volume_col = column_index(result, "total_volume");
    size_t count_col = column_index(result, "trade_count");
    size_t price_col = column_index(result, "avg_price");

    for (const auto& row : result.rows) {
        EXPECT_GT(row[volume_col].get<uint64_t>(), 0u);
        EXPECT_GT(row[count_col].get<uint64_t>(), 0u);
        double avg_price = row[price_col].get<double>();
        EXPECT_GT(avg_price, 0.0);
        EXPECT_LT(avg_price, 1000000.0); // broad sanity bound, not a tight price check
    }
}
