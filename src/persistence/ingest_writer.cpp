#include "persistence/bind_helpers.h"
#include "persistence/ingest_writer.h"

#include <mysqlx/xdevapi.h>

#include <type_traits>
#include <variant>

namespace mde {

namespace {

const char* kInsertMessageSql =
    "INSERT INTO messages "
    "(msg_type, timestamp_ns, stock_locate, order_ref, side, shares, price_raw, new_order_ref, match_number) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

const char* kUpsertSymbolSql =
    "INSERT INTO symbols (stock_locate, ticker) VALUES (?, ?) "
    "ON DUPLICATE KEY UPDATE ticker = VALUES(ticker)";

const char* kInsertTradeSql =
    "INSERT INTO trades "
    "(timestamp_ns, stock_locate, order_ref, price_raw, shares, match_number) "
    "VALUES (?, ?, ?, ?, ?, ?)";

} // namespace

MessageRow to_message_row(const ItchMessage& msg) {
    return std::visit(
        [](auto&& m) -> MessageRow {
            using T = std::decay_t<decltype(m)>;

            MessageRow row{};
            row.msg_type = m.header.message_type;
            row.timestamp_ns = m.header.timestamp_ns;
            row.stock_locate = m.header.stock_locate;

            if constexpr (std::is_same_v<T, StockDirectoryMessage>) {
                row.ticker = trim_itch_text(m.stock, sizeof(m.stock));
            } else if constexpr (std::is_same_v<T, AddOrderMessage> ||
                                  std::is_same_v<T, AddOrderMPIDMessage>) {
                row.order_ref = m.order_reference_number;
                row.side = m.buy_sell_indicator;
                row.shares = m.shares;
                row.price_raw = m.price_raw;
            } else if constexpr (std::is_same_v<T, OrderExecutedMessage>) {
                row.order_ref = m.order_reference_number;
                row.shares = m.executed_shares;
                row.match_number = m.match_number;
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPriceMessage>) {
                row.order_ref = m.order_reference_number;
                row.shares = m.executed_shares;
                row.match_number = m.match_number;
                row.price_raw = m.execution_price_raw;
            } else if constexpr (std::is_same_v<T, OrderCancelMessage>) {
                row.order_ref = m.order_reference_number;
                row.shares = m.cancelled_shares;
            } else if constexpr (std::is_same_v<T, OrderDeleteMessage>) {
                row.order_ref = m.order_reference_number;
            } else if constexpr (std::is_same_v<T, OrderReplaceMessage>) {
                row.order_ref = m.original_order_reference_number;
                row.new_order_ref = m.new_order_reference_number;
                row.shares = m.shares;
                row.price_raw = m.price_raw;
            }
            // SystemEventMessage: header fields only, everything else NULL.

            return row;
        },
        msg);
}

std::optional<TradeRow> TradePriceResolver::resolve(const ItchMessage& msg) {
    return std::visit(
        [this](auto&& m) -> std::optional<TradeRow> {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, AddOrderMessage> || std::is_same_v<T, AddOrderMPIDMessage>) {
                resting_order_price_[m.order_reference_number] = m.price_raw;
            } else if constexpr (std::is_same_v<T, OrderDeleteMessage>) {
                resting_order_price_.erase(m.order_reference_number);
            } else if constexpr (std::is_same_v<T, OrderReplaceMessage>) {
                resting_order_price_.erase(m.original_order_reference_number);
                resting_order_price_[m.new_order_reference_number] = m.price_raw;
            } else if constexpr (std::is_same_v<T, OrderExecutedMessage>) {
                auto it = resting_order_price_.find(m.order_reference_number);
                if (it == resting_order_price_.end()) {
                    ++unresolved_execution_count_;
                    return std::nullopt;
                }

                TradeRow row{};
                row.timestamp_ns = m.header.timestamp_ns;
                row.stock_locate = m.header.stock_locate;
                row.order_ref = m.order_reference_number;
                row.price_raw = it->second;
                row.shares = m.executed_shares;
                row.match_number = m.match_number;
                return row;
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPriceMessage>) {
                TradeRow row{};
                row.timestamp_ns = m.header.timestamp_ns;
                row.stock_locate = m.header.stock_locate;
                row.order_ref = m.order_reference_number;
                row.price_raw = m.execution_price_raw;
                row.shares = m.executed_shares;
                row.match_number = m.match_number;
                return row;
            }
            // SystemEvent, StockDirectory, OrderCancel: not relevant to
            // trade recording or price resolution -- ignored.
            return std::nullopt;
        },
        msg);
}

size_t TradePriceResolver::unresolved_execution_count() const {
    return unresolved_execution_count_;
}

IngestWriter::IngestWriter(DbConnection& conn, size_t batch_size)
    : conn_(conn), batch_size_(batch_size) {
}

void IngestWriter::write(const ItchMessage& msg) {
    message_buffer_.push_back(to_message_row(msg));

    if (std::optional<TradeRow> trade_row = resolver_.resolve(msg)) {
        trade_buffer_.push_back(*trade_row);
    }

    if (message_buffer_.size() >= batch_size_) {
        flush_batch();
    }
}

void IngestWriter::flush() {
    flush_batch();
}

size_t IngestWriter::unresolved_execution_count() const {
    return resolver_.unresolved_execution_count();
}

void IngestWriter::flush_batch() {
    if (message_buffer_.empty() && trade_buffer_.empty()) {
        return;
    }

    conn_.start_transaction();
    try {
        for (const auto& row : message_buffer_) {
            if (row.msg_type == 'R' && row.ticker.has_value()) {
                conn_.sql(kUpsertSymbolSql)
                    .bind(to_value(static_cast<uint64_t>(row.stock_locate)), mysqlx::Value(*row.ticker))
                    .execute();
            }

            conn_.sql(kInsertMessageSql)
                .bind(to_value(row.msg_type),
                      to_value(row.timestamp_ns),
                      to_value(static_cast<uint64_t>(row.stock_locate)),
                      to_value(row.order_ref),
                      to_value(row.side),
                      to_value(row.shares),
                      to_value(row.price_raw),
                      to_value(row.new_order_ref),
                      to_value(row.match_number))
                .execute();
        }

        for (const auto& row : trade_buffer_) {
            conn_.sql(kInsertTradeSql)
                .bind(mysqlx::Value(row.timestamp_ns),
                      mysqlx::Value(static_cast<uint64_t>(row.stock_locate)),
                      mysqlx::Value(row.order_ref),
                      mysqlx::Value(row.price_raw),
                      mysqlx::Value(row.shares),
                      mysqlx::Value(row.match_number))
                .execute();
        }

        conn_.commit();
    } catch (...) {
        conn_.rollback();
        throw;
    }

    message_buffer_.clear();
    trade_buffer_.clear();
}

} // namespace mde
