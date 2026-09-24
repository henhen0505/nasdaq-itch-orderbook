// Persistence layer tests, run against the live local MySQL instance in an
// isolated `market_data_engine_test` database (see test_connection() below)
// -- NOT the `market_data_engine` database that holds real loaded data.
// Requires MDE_DB_PASSWORD; if unset, every test below fails with a clear
// message from DbConfig::from_env().
//
// SetUp() drops and recreates all 4 tables before EVERY test.
// test_connection() hardcodes the schema to `market_data_engine_test`
// regardless of MDE_DB_SCHEMA, so this file structurally cannot touch the
// real data. `market_data_engine_test` must exist with `mde_user` granted
// on it -- see test_connection()'s comment for the one-time root SQL.

#include "parser/itch_messages.h"
#include "persistence/db_config.h"
#include "persistence/db_connection.h"
#include "persistence/ingest_writer.h"
#include "util/sql_file.h"

#include <gtest/gtest.h>
#include <mysqlx/xdevapi.h>

#include <cstring>
#include <string>
#include <vector>

#ifndef MDE_PROJECT_ROOT
#error "MDE_PROJECT_ROOT must be defined by the build -- see CMakeLists.txt"
#endif

using namespace mde;

namespace {

// --- Synthetic ItchMessage builders -----------------------------------
// Unlike test_parser.cpp (which builds raw wire bytes), these tests operate
// one layer above the parser: they exercise the writers directly against
// already-parsed structs, so only the fields relevant to a `messages`/
// `trades` row are populated; everything else is zeroed.

MessageHeader make_header(char type, uint16_t stock_locate, uint64_t timestamp_ns) {
    MessageHeader h{};
    h.message_type = type;
    h.stock_locate = stock_locate;
    h.tracking_number = 1;
    h.timestamp_ns = timestamp_ns;
    return h;
}

StockDirectoryMessage make_stock_directory(uint16_t stock_locate, const std::string& ticker) {
    StockDirectoryMessage msg{};
    msg.header = make_header('R', stock_locate, 34200000000000ull);
    std::string padded = ticker;
    padded.resize(sizeof(msg.stock), ' ');
    std::memcpy(msg.stock, padded.data(), sizeof(msg.stock));
    msg.market_category = 'N';
    msg.financial_status_indicator = ' ';
    msg.round_lot_size = 100;
    msg.round_lots_only = 'N';
    msg.issue_classification = 'C';
    msg.issue_sub_type[0] = 'C';
    msg.issue_sub_type[1] = 'S';
    msg.authenticity = 'P';
    msg.short_sale_threshold_indicator = 'N';
    msg.ipo_flag = 'N';
    msg.luld_reference_price_tier = '1';
    msg.etp_flag = 'N';
    msg.etp_leverage_factor = 0;
    msg.inverse_indicator = 'N';
    return msg;
}

AddOrderMessage make_add_order(uint16_t stock_locate, uint64_t order_ref, char side, uint32_t shares,
                                uint32_t price_raw) {
    AddOrderMessage msg{};
    msg.header = make_header('A', stock_locate, 34200500000000ull);
    msg.order_reference_number = order_ref;
    msg.buy_sell_indicator = side;
    msg.shares = shares;
    std::memset(msg.stock, ' ', sizeof(msg.stock));
    msg.price_raw = price_raw;
    return msg;
}

OrderExecutedMessage make_order_executed(uint16_t stock_locate, uint64_t order_ref, uint32_t executed_shares,
                                          uint64_t match_number) {
    OrderExecutedMessage msg{};
    msg.header = make_header('E', stock_locate, 34201000000000ull);
    msg.order_reference_number = order_ref;
    msg.executed_shares = executed_shares;
    msg.match_number = match_number;
    return msg;
}

OrderExecutedWithPriceMessage make_order_executed_with_price(uint16_t stock_locate, uint64_t order_ref,
                                                               uint32_t executed_shares, uint64_t match_number,
                                                               uint32_t execution_price_raw) {
    OrderExecutedWithPriceMessage msg{};
    msg.header = make_header('C', stock_locate, 34201100000000ull);
    msg.order_reference_number = order_ref;
    msg.executed_shares = executed_shares;
    msg.match_number = match_number;
    msg.printable = 'Y';
    msg.execution_price_raw = execution_price_raw;
    return msg;
}

SystemEventMessage make_system_event(uint16_t stock_locate, char event_code) {
    SystemEventMessage msg{};
    msg.header = make_header('S', stock_locate, 25200000000000ull);
    msg.event_code = event_code;
    return msg;
}

// --- Schema setup/teardown against the live database --------------------

void apply_schema(DbConnection& conn) {
    const std::string path = std::string(MDE_PROJECT_ROOT) + "/sql/001_schema.sql";
    for (const auto& statement : split_statements(read_file(path))) {
        conn.execute(statement);
    }
}

// Drops children before the `symbols` parent so foreign keys don't block
// the drop.
void drop_all_tables(DbConnection& conn) {
    conn.execute("DROP TABLE IF EXISTS book_snapshots");
    conn.execute("DROP TABLE IF EXISTS trades");
    conn.execute("DROP TABLE IF EXISTS messages");
    conn.execute("DROP TABLE IF EXISTS symbols");
}

// One shared connection for the whole test binary, pointed at the isolated
// `market_data_engine_test` database. Overrides MDE_DB_SCHEMA on purpose:
// SetUp() drops/recreates all 4 tables, so this must never share a schema
// with real data. `market_data_engine_test` must already exist with
// `mde_user` granted on it (see SETUP.md). Constructed lazily so a missing
// MDE_DB_PASSWORD surfaces as a test failure, not a static-init abort.
DbConnection& test_connection() {
    static DbConnection conn = [] {
        DbConfig config = DbConfig::from_env();
        config.schema = "market_data_engine_test";
        return DbConnection(config);
    }();
    return conn;
}

void insert_symbol(DbConnection& conn, uint16_t stock_locate, const std::string& ticker) {
    conn.sql("INSERT INTO symbols (stock_locate, ticker) VALUES (?, ?)")
        .bind(mysqlx::Value(static_cast<uint64_t>(stock_locate)), mysqlx::Value(ticker))
        .execute();
}

} // namespace

class PersistenceTest : public ::testing::Test {
protected:
    DbConnection& conn() { return test_connection(); }

    void SetUp() override {
        drop_all_tables(conn());
        apply_schema(conn());
    }
};

// --- Schema creation ------------------------------------------------------

TEST_F(PersistenceTest, SchemaCreatesAllFourTables) {
    EXPECT_NO_THROW(conn().execute("SELECT COUNT(*) FROM symbols"));
    EXPECT_NO_THROW(conn().execute("SELECT COUNT(*) FROM messages"));
    EXPECT_NO_THROW(conn().execute("SELECT COUNT(*) FROM trades"));
    EXPECT_NO_THROW(conn().execute("SELECT COUNT(*) FROM book_snapshots"));
}

// --- Insert-then-read-back round trips ------------------------------------

TEST_F(PersistenceTest, SymbolsInsertReadBackRoundTrip) {
    insert_symbol(conn(), 1, "AAPL");

    mysqlx::SqlResult result =
        conn().sql("SELECT stock_locate, ticker FROM symbols WHERE stock_locate = ?")
            .bind(mysqlx::Value(uint64_t(1)))
            .execute();
    mysqlx::Row row = result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(row));
    EXPECT_EQ(row[0].get<uint64_t>(), 1u);
    EXPECT_EQ(row[1].get<std::string>(), "AAPL");
}

TEST_F(PersistenceTest, MessageRoundTripPopulatesSymbolAndMessageRow) {
    IngestWriter writer(conn());
    writer.write(ItchMessage{make_stock_directory(1, "AAPL")});
    writer.write(ItchMessage{make_add_order(1, 42, 'B', 500, 1015000)});
    writer.flush();

    // The 'R' row upserts `symbols` as a side effect.
    mysqlx::SqlResult sym_result =
        conn().sql("SELECT ticker FROM symbols WHERE stock_locate = ?").bind(mysqlx::Value(uint64_t(1))).execute();
    mysqlx::Row sym_row = sym_result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(sym_row));
    EXPECT_EQ(sym_row[0].get<std::string>(), "AAPL");

    mysqlx::SqlResult result =
        conn().sql("SELECT msg_type, stock_locate, order_ref, side, shares, price_raw, new_order_ref, match_number "
                    "FROM messages WHERE msg_type = 'A' AND stock_locate = ?")
            .bind(mysqlx::Value(uint64_t(1)))
            .execute();
    mysqlx::Row row = result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(row));
    EXPECT_EQ(row[0].get<std::string>(), "A");
    EXPECT_EQ(row[1].get<uint64_t>(), 1u);
    EXPECT_EQ(row[2].get<uint64_t>(), 42u);
    EXPECT_EQ(row[3].get<std::string>(), "B");
    EXPECT_EQ(row[4].get<uint64_t>(), 500u);
    EXPECT_EQ(row[5].get<uint64_t>(), 1015000u);
    EXPECT_TRUE(row[6].isNull()); // new_order_ref: only populated for Order Replace
    EXPECT_TRUE(row[7].isNull()); // match_number: only populated for Order Executed(-with-Price)
}

TEST_F(PersistenceTest, NullsAllOptionalColumnsForSystemEvent) {
    insert_symbol(conn(), 1, "AAPL");

    IngestWriter writer(conn());
    writer.write(ItchMessage{make_system_event(1, 'O')});
    writer.flush();

    mysqlx::SqlResult result =
        conn().sql("SELECT order_ref, side, shares, price_raw, new_order_ref, match_number "
                    "FROM messages WHERE msg_type = 'S' AND stock_locate = ?")
            .bind(mysqlx::Value(uint64_t(1)))
            .execute();
    mysqlx::Row row = result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(row));
    for (mysqlx::col_count_t i = 0; i < 6; ++i) {
        EXPECT_TRUE(row[i].isNull()) << "column " << i << " should be NULL for a System Event row";
    }
}

TEST_F(PersistenceTest, TradePriceResolvedFromRestingOrder) {
    insert_symbol(conn(), 1, "AAPL");

    IngestWriter writer(conn());
    writer.write(ItchMessage{make_add_order(1, 42, 'B', 500, 1015000)});
    writer.write(ItchMessage{make_order_executed(1, 42, 200, 999999)});
    writer.flush();

    EXPECT_EQ(writer.unresolved_execution_count(), 0u);

    mysqlx::SqlResult result =
        conn().sql("SELECT stock_locate, order_ref, price_raw, shares, match_number FROM trades WHERE order_ref = ?")
            .bind(mysqlx::Value(uint64_t(42)))
            .execute();
    mysqlx::Row row = result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(row));
    EXPECT_EQ(row[0].get<uint64_t>(), 1u);
    EXPECT_EQ(row[1].get<uint64_t>(), 42u);
    // 'E' carries no price field on the wire -- this must come from the
    // earlier Add Order's posted price.
    EXPECT_EQ(row[2].get<uint64_t>(), 1015000u);
    EXPECT_EQ(row[3].get<uint64_t>(), 200u);
    EXPECT_EQ(row[4].get<uint64_t>(), 999999u);
}

TEST_F(PersistenceTest, ExecutedWithPriceHandled) {
    insert_symbol(conn(), 1, "AAPL");

    IngestWriter writer(conn());
    writer.write(ItchMessage{make_order_executed_with_price(1, 55, 300, 777, 1016000)});
    writer.flush();

    mysqlx::SqlResult result =
        conn().sql("SELECT price_raw, shares, match_number FROM trades WHERE order_ref = ?")
            .bind(mysqlx::Value(uint64_t(55)))
            .execute();
    mysqlx::Row row = result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(row));
    EXPECT_EQ(row[0].get<uint64_t>(), 1016000u);
    EXPECT_EQ(row[1].get<uint64_t>(), 300u);
    EXPECT_EQ(row[2].get<uint64_t>(), 777u);
}

TEST_F(PersistenceTest, SkipsExecutionWithNoResolvablePrice) {
    insert_symbol(conn(), 1, "AAPL");

    IngestWriter writer(conn());
    // No preceding Add Order for order_ref 7 -- its resting price cannot be
    // resolved (e.g. the Add Order predates the capture window). Must be
    // skipped, not written with a fabricated price.
    writer.write(ItchMessage{make_order_executed(1, 7, 100, 555)});
    writer.flush();

    EXPECT_EQ(writer.unresolved_execution_count(), 1u);

    mysqlx::SqlResult result =
        conn().sql("SELECT COUNT(*) FROM trades WHERE order_ref = ?").bind(mysqlx::Value(uint64_t(7))).execute();
    mysqlx::Row row = result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(row));
    EXPECT_EQ(row[0].get<uint64_t>(), 0u);
}

// --- Batch-boundary test ---------------------------------------------------

TEST_F(PersistenceTest, BatchBoundaryFlushesExactlyBatchSizePlusOneRows) {
    insert_symbol(conn(), 1, "AAPL");

    constexpr size_t kBatchSize = 3;
    constexpr size_t kRowCount = kBatchSize + 1;
    IngestWriter writer(conn(), kBatchSize);

    // Seed the resting-price resolver so every Order Executed written below
    // produces both a `messages` row and a `trades` row -- this exercises
    // the shared message/trade batch threshold, not just the message one.
    writer.write(ItchMessage{make_add_order(1, 500, 'B', 100, 1000000)});

    for (uint64_t i = 0; i < kRowCount; ++i) {
        writer.write(ItchMessage{make_order_executed(1, 500, 10, 900 + i)});
    }
    writer.flush(); // writes the rows left over after the one auto-flush

    EXPECT_EQ(writer.unresolved_execution_count(), 0u);

    // messages: the seed Add Order plus one row per Order Executed, all
    // sharing order_ref 500.
    mysqlx::SqlResult msg_result =
        conn().sql("SELECT COUNT(*) FROM messages WHERE order_ref = ?").bind(mysqlx::Value(uint64_t(500))).execute();
    mysqlx::Row msg_row = msg_result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(msg_row));
    EXPECT_EQ(msg_row[0].get<uint64_t>(), kRowCount + 1);

    // trades: one row per Order Executed (each resolves against the same
    // resting order, which is never deleted or replaced in this test).
    mysqlx::SqlResult trade_result =
        conn().sql("SELECT COUNT(*) FROM trades WHERE order_ref = ?").bind(mysqlx::Value(uint64_t(500))).execute();
    mysqlx::Row trade_row = trade_result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(trade_row));
    EXPECT_EQ(trade_row[0].get<uint64_t>(), kRowCount);
}

// --- Transaction rollback on error -----------------------------------------

TEST_F(PersistenceTest, BatchRollsBackEntirelyOnForeignKeyViolation) {
    insert_symbol(conn(), 1, "AAPL");
    // Deliberately no symbol row for stock_locate 99.

    constexpr size_t kBatchSize = 3;
    IngestWriter writer(conn(), kBatchSize);

    writer.write(ItchMessage{make_add_order(1, 1, 'B', 100, 1000000)});
    // Executed against the order added above -- this is the message that
    // would also produce a `trades` row (price resolved from the Add Order
    // in the same batch), which is what proves messages and trades commit
    // or roll back together rather than independently.
    writer.write(ItchMessage{make_order_executed(1, 1, 100, 555)});
    // stock_locate 99 has no row in `symbols` -- violates fk_msg_symbol when
    // this batch flushes (triggered by this write hitting kBatchSize).
    EXPECT_THROW(writer.write(ItchMessage{make_add_order(99, 3, 'B', 100, 1000000)}), mysqlx::Error);

    // The whole batch -- including the two messages that were individually
    // valid -- must have been rolled back, not partially committed.
    mysqlx::SqlResult msg_result =
        conn().sql("SELECT COUNT(*) FROM messages WHERE stock_locate = 1 AND order_ref = 1").execute();
    mysqlx::Row msg_row = msg_result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(msg_row));
    EXPECT_EQ(msg_row[0].get<uint64_t>(), 0u);

    // The `trades` row that would have been produced by the Order Executed
    // above must also be gone -- this is the assertion that actually proves
    // the fix: messages and trades from the same batch commit or roll back
    // together, not on independent schedules.
    mysqlx::SqlResult trade_result = conn().sql("SELECT COUNT(*) FROM trades WHERE order_ref = 1").execute();
    mysqlx::Row trade_row = trade_result.fetchOne();
    ASSERT_TRUE(static_cast<bool>(trade_row));
    EXPECT_EQ(trade_row[0].get<uint64_t>(), 0u);
}
