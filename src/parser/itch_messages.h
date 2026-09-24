#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace mde {

// Common 11-byte header present on every ITCH 5.0 message (offsets 0-10 on
// the wire): type(1) + stock_locate(2) + tracking_number(2) + timestamp(6).
struct MessageHeader {
    char message_type;
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns; // nanoseconds since midnight (wire value is 48-bit)
};

// 'S' -- System Event
struct SystemEventMessage {
    MessageHeader header;
    char event_code;
};

// 'R' -- Stock Directory
struct StockDirectoryMessage {
    MessageHeader header;
    char stock[8]; // space-padded ticker, not null-terminated
    char market_category;
    char financial_status_indicator;
    uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    char issue_sub_type[2];
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    uint32_t etp_leverage_factor;
    char inverse_indicator;
};

// 'A' -- Add Order (no MPID attribution)
struct AddOrderMessage {
    MessageHeader header;
    uint64_t order_reference_number;
    char buy_sell_indicator;
    uint32_t shares;
    char stock[8];
    uint32_t price_raw; // fixed-point; see itch_price_to_decimal()
};

// 'F' -- Add Order with MPID Attribution
struct AddOrderMPIDMessage {
    MessageHeader header;
    uint64_t order_reference_number;
    char buy_sell_indicator;
    uint32_t shares;
    char stock[8];
    uint32_t price_raw;
    char attribution[4];
};

// 'E' -- Order Executed
struct OrderExecutedMessage {
    MessageHeader header;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
};

// 'C' -- Order Executed with Price
struct OrderExecutedWithPriceMessage {
    MessageHeader header;
    uint64_t order_reference_number;
    uint32_t executed_shares;
    uint64_t match_number;
    char printable;
    uint32_t execution_price_raw;
};

// 'X' -- Order Cancel
struct OrderCancelMessage {
    MessageHeader header;
    uint64_t order_reference_number;
    uint32_t cancelled_shares;
};

// 'D' -- Order Delete
struct OrderDeleteMessage {
    MessageHeader header;
    uint64_t order_reference_number;
};

// 'U' -- Order Replace
struct OrderReplaceMessage {
    MessageHeader header;
    uint64_t original_order_reference_number;
    uint64_t new_order_reference_number;
    uint32_t shares;
    uint32_t price_raw;
};

// Tagged union over the 9 required message types, used to dispatch a
// parsed ITCH message to downstream consumers (order-book engine, writers)
// without them needing to know the parser's internals.
using ItchMessage = std::variant<
    SystemEventMessage,
    StockDirectoryMessage,
    AddOrderMessage,
    AddOrderMPIDMessage,
    OrderExecutedMessage,
    OrderExecutedWithPriceMessage,
    OrderCancelMessage,
    OrderDeleteMessage,
    OrderReplaceMessage>;

// ITCH prices are 4-byte fixed-point integers with an implied 4 decimal
// places (i.e. decimal value = raw / 10,000).
constexpr double itch_price_to_decimal(uint32_t raw) {
    return static_cast<double>(raw) / 10000.0;
}

// Trims trailing ASCII spaces from a fixed-width, space-padded ITCH text
// field (e.g. the 8-byte `stock` ticker).
inline std::string trim_itch_text(const char* data, size_t len) {
    size_t end = len;
    while (end > 0 && data[end - 1] == ' ') {
        --end;
    }
    return std::string(data, end);
}

} // namespace mde
