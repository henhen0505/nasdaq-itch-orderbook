#include "parser/itch_messages.h"
#include "parser/itch_parser.h"
#include "parser/binary_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

using namespace mde;

namespace {

// --- Byte-buffer construction helpers, mirroring the big-endian layout ---
// These deliberately do not reuse BinaryReader's
// decode logic -- the test's job is to build bytes independently and check
// the parser decodes them back to the same values.

void push_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void push_be16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

void push_be32(std::vector<uint8_t>& buf, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
}

void push_be48(std::vector<uint8_t>& buf, uint64_t v) {
    for (int shift = 40; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
}

void push_be64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
    }
}

// Appends `text`, space-padded (or truncated) to exactly `width` bytes --
// mirrors ITCH's space-padded fixed-width text fields.
void push_text(std::vector<uint8_t>& buf, const std::string& text, size_t width) {
    std::string padded = text;
    padded.resize(width, ' ');
    buf.insert(buf.end(), padded.begin(), padded.end());
}

// Wraps `body` (the message bytes, type byte through last field -- NOT
// including the length prefix) with its 2-byte big-endian length prefix
// and appends the whole frame to `stream`.
void append_frame(std::vector<uint8_t>& stream, const std::vector<uint8_t>& body) {
    push_be16(stream, static_cast<uint16_t>(body.size()));
    stream.insert(stream.end(), body.begin(), body.end());
}

// Builds the common 11-byte header that starts every message body.
std::vector<uint8_t> make_header(char type, uint16_t stock_locate, uint16_t tracking_number,
                                  uint64_t timestamp_ns) {
    std::vector<uint8_t> body;
    push_u8(body, static_cast<uint8_t>(type));
    push_be16(body, stock_locate);
    push_be16(body, tracking_number);
    push_be48(body, timestamp_ns);
    return body;
}

std::vector<uint8_t> make_stock_directory(uint16_t stock_locate, const std::string& ticker) {
    std::vector<uint8_t> body = make_header('R', stock_locate, 1, 34200000000000ull);
    push_text(body, ticker, 8);
    push_u8(body, 'N');       // market_category
    push_u8(body, ' ');       // financial_status_indicator
    push_be32(body, 100);     // round_lot_size
    push_u8(body, 'N');       // round_lots_only
    push_u8(body, 'C');       // issue_classification
    push_text(body, "CS", 2); // issue_sub_type
    push_u8(body, 'P');       // authenticity
    push_u8(body, 'N');       // short_sale_threshold_indicator
    push_u8(body, 'N');       // ipo_flag
    push_u8(body, '1');       // luld_reference_price_tier
    push_u8(body, 'N');       // etp_flag
    push_be32(body, 0);       // etp_leverage_factor
    push_u8(body, 'N');       // inverse_indicator
    return body;
}

std::vector<uint8_t> make_add_order(uint16_t stock_locate, uint64_t order_ref, char side, uint32_t shares,
                                     const std::string& ticker, uint32_t price_raw) {
    std::vector<uint8_t> body = make_header('A', stock_locate, 7, 34200500000000ull);
    push_be64(body, order_ref);
    push_u8(body, static_cast<uint8_t>(side));
    push_be32(body, shares);
    push_text(body, ticker, 8);
    push_be32(body, price_raw);
    return body;
}

std::vector<uint8_t> make_add_order_mpid(uint16_t stock_locate, uint64_t order_ref, char side, uint32_t shares,
                                          const std::string& ticker, uint32_t price_raw,
                                          const std::string& attribution) {
    std::vector<uint8_t> body = make_header('F', stock_locate, 7, 34200600000000ull);
    push_be64(body, order_ref);
    push_u8(body, static_cast<uint8_t>(side));
    push_be32(body, shares);
    push_text(body, ticker, 8);
    push_be32(body, price_raw);
    push_text(body, attribution, 4);
    return body;
}

std::vector<uint8_t> make_order_executed(uint16_t stock_locate, uint64_t order_ref, uint32_t executed_shares,
                                          uint64_t match_number) {
    std::vector<uint8_t> body = make_header('E', stock_locate, 7, 34201000000000ull);
    push_be64(body, order_ref);
    push_be32(body, executed_shares);
    push_be64(body, match_number);
    return body;
}

std::vector<uint8_t> make_order_executed_with_price(uint16_t stock_locate, uint64_t order_ref,
                                                      uint32_t executed_shares, uint64_t match_number,
                                                      char printable, uint32_t execution_price_raw) {
    std::vector<uint8_t> body = make_header('C', stock_locate, 7, 34201100000000ull);
    push_be64(body, order_ref);
    push_be32(body, executed_shares);
    push_be64(body, match_number);
    push_u8(body, static_cast<uint8_t>(printable));
    push_be32(body, execution_price_raw);
    return body;
}

std::vector<uint8_t> make_order_cancel(uint16_t stock_locate, uint64_t order_ref, uint32_t cancelled_shares) {
    std::vector<uint8_t> body = make_header('X', stock_locate, 7, 34201200000000ull);
    push_be64(body, order_ref);
    push_be32(body, cancelled_shares);
    return body;
}

std::vector<uint8_t> make_order_delete(uint16_t stock_locate, uint64_t order_ref) {
    std::vector<uint8_t> body = make_header('D', stock_locate, 7, 34201300000000ull);
    push_be64(body, order_ref);
    return body;
}

std::vector<uint8_t> make_order_replace(uint16_t stock_locate, uint64_t original_ref, uint64_t new_ref,
                                         uint32_t shares, uint32_t price_raw) {
    std::vector<uint8_t> body = make_header('U', stock_locate, 7, 34201400000000ull);
    push_be64(body, original_ref);
    push_be64(body, new_ref);
    push_be32(body, shares);
    push_be32(body, price_raw);
    return body;
}

std::vector<uint8_t> make_system_event(uint16_t stock_locate, char event_code) {
    std::vector<uint8_t> body = make_header('S', stock_locate, 1, 25200000000000ull);
    push_u8(body, static_cast<uint8_t>(event_code));
    return body;
}

// Collects every message the parser emits, in order.
std::vector<ItchMessage> parse_all(const std::vector<uint8_t>& stream) {
    std::vector<ItchMessage> received;
    parse_buffer(stream.data(), stream.size(), [&](const ItchMessage& msg) { received.push_back(msg); });
    return received;
}

constexpr uint16_t kAaplLocate = 1;

// A stream that resolves stock_locate 1 -> AAPL via a Stock Directory
// message, used as the prefix for tests that need a target-ticker message
// to survive the allowlist filter.
std::vector<uint8_t> stream_with_aapl_directory() {
    std::vector<uint8_t> stream;
    append_frame(stream, make_stock_directory(kAaplLocate, "AAPL"));
    return stream;
}

} // namespace

TEST(ItchParserTest, ParsesStockDirectory) {
    std::vector<uint8_t> stream;
    append_frame(stream, make_stock_directory(kAaplLocate, "AAPL"));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 1u);

    const auto& msg = std::get<StockDirectoryMessage>(received[0]);
    EXPECT_EQ(msg.header.message_type, 'R');
    EXPECT_EQ(msg.header.stock_locate, kAaplLocate);
    EXPECT_EQ(msg.header.tracking_number, 1);
    EXPECT_EQ(msg.header.timestamp_ns, 34200000000000ull);
    EXPECT_EQ(trim_itch_text(msg.stock, sizeof(msg.stock)), "AAPL");
    EXPECT_EQ(msg.market_category, 'N');
    EXPECT_EQ(msg.financial_status_indicator, ' ');
    EXPECT_EQ(msg.round_lot_size, 100u);
    EXPECT_EQ(msg.round_lots_only, 'N');
    EXPECT_EQ(msg.issue_classification, 'C');
    EXPECT_EQ(msg.issue_sub_type[0], 'C');
    EXPECT_EQ(msg.issue_sub_type[1], 'S');
    EXPECT_EQ(msg.authenticity, 'P');
    EXPECT_EQ(msg.short_sale_threshold_indicator, 'N');
    EXPECT_EQ(msg.ipo_flag, 'N');
    EXPECT_EQ(msg.luld_reference_price_tier, '1');
    EXPECT_EQ(msg.etp_flag, 'N');
    EXPECT_EQ(msg.etp_leverage_factor, 0u);
    EXPECT_EQ(msg.inverse_indicator, 'N');
}

TEST(ItchParserTest, ParsesSystemEvent) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_system_event(kAaplLocate, 'O'));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<SystemEventMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'S');
    EXPECT_EQ(msg.header.stock_locate, kAaplLocate);
    EXPECT_EQ(msg.header.timestamp_ns, 25200000000000ull);
    EXPECT_EQ(msg.event_code, 'O');
}

TEST(ItchParserTest, ParsesAddOrder) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_add_order(kAaplLocate, 0x0123456789ABCDEFull, 'B', 500, "AAPL", 1015000));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<AddOrderMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'A');
    EXPECT_EQ(msg.header.stock_locate, kAaplLocate);
    EXPECT_EQ(msg.order_reference_number, 0x0123456789ABCDEFull);
    EXPECT_EQ(msg.buy_sell_indicator, 'B');
    EXPECT_EQ(msg.shares, 500u);
    EXPECT_EQ(trim_itch_text(msg.stock, sizeof(msg.stock)), "AAPL");
    EXPECT_EQ(msg.price_raw, 1015000u);
    EXPECT_DOUBLE_EQ(itch_price_to_decimal(msg.price_raw), 101.5);
}

TEST(ItchParserTest, ParsesAddOrderMPID) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_add_order_mpid(kAaplLocate, 42, 'S', 300, "AAPL", 2500000, "EDGX"));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<AddOrderMPIDMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'F');
    EXPECT_EQ(msg.order_reference_number, 42u);
    EXPECT_EQ(msg.buy_sell_indicator, 'S');
    EXPECT_EQ(msg.shares, 300u);
    EXPECT_EQ(trim_itch_text(msg.stock, sizeof(msg.stock)), "AAPL");
    EXPECT_EQ(msg.price_raw, 2500000u);
    EXPECT_EQ(std::string(msg.attribution, 4), "EDGX");
}

TEST(ItchParserTest, ParsesOrderExecuted) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_order_executed(kAaplLocate, 42, 100, 999999));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<OrderExecutedMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'E');
    EXPECT_EQ(msg.order_reference_number, 42u);
    EXPECT_EQ(msg.executed_shares, 100u);
    EXPECT_EQ(msg.match_number, 999999u);
}

TEST(ItchParserTest, ParsesOrderExecutedWithPrice) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_order_executed_with_price(kAaplLocate, 42, 100, 999999, 'Y', 1016000));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<OrderExecutedWithPriceMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'C');
    EXPECT_EQ(msg.order_reference_number, 42u);
    EXPECT_EQ(msg.executed_shares, 100u);
    EXPECT_EQ(msg.match_number, 999999u);
    EXPECT_EQ(msg.printable, 'Y');
    EXPECT_EQ(msg.execution_price_raw, 1016000u);
}

TEST(ItchParserTest, ParsesOrderCancel) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_order_cancel(kAaplLocate, 42, 50));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<OrderCancelMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'X');
    EXPECT_EQ(msg.order_reference_number, 42u);
    EXPECT_EQ(msg.cancelled_shares, 50u);
}

TEST(ItchParserTest, ParsesOrderDelete) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_order_delete(kAaplLocate, 42));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<OrderDeleteMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'D');
    EXPECT_EQ(msg.order_reference_number, 42u);
}

TEST(ItchParserTest, ParsesOrderReplace) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();
    append_frame(stream, make_order_replace(kAaplLocate, 42, 43, 400, 1020000));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);

    const auto& msg = std::get<OrderReplaceMessage>(received[1]);
    EXPECT_EQ(msg.header.message_type, 'U');
    EXPECT_EQ(msg.original_order_reference_number, 42u);
    EXPECT_EQ(msg.new_order_reference_number, 43u);
    EXPECT_EQ(msg.shares, 400u);
    EXPECT_EQ(msg.price_raw, 1020000u);
}

// --- Edge cases -----------------------------------------------------------

TEST(ItchParserTest, TruncatedMessageThrowsCleanly) {
    // Frame prefix claims a 19-byte 'D' message body, but the stream is
    // cut off after only 10 of those bytes -- simulates a file truncated
    // mid-message.
    std::vector<uint8_t> full_body = make_order_delete(kAaplLocate, 42);
    ASSERT_EQ(full_body.size(), 19u);

    std::vector<uint8_t> stream;
    push_be16(stream, static_cast<uint16_t>(full_body.size()));
    stream.insert(stream.end(), full_body.begin(), full_body.begin() + 10);

    EXPECT_THROW(parse_all(stream), BufferUnderrunError);
}

TEST(ItchParserTest, MinimalLengthFrameForKnownTypeThrowsCleanly) {
    // The frame's declared length is fully present in the stream, but it is
    // too small to hold even the common 11-byte header ('D' claims only 4
    // bytes: type + 3). Must not read past this frame into whatever
    // (nonexistent) bytes follow.
    std::vector<uint8_t> stream;
    std::vector<uint8_t> short_body = {'D', 0x00, 0x01, 0x02};
    append_frame(stream, short_body);

    EXPECT_THROW(parse_all(stream), BufferUnderrunError);
}

TEST(ItchParserTest, ZeroLengthFrameIsSkippedWithoutError) {
    std::vector<uint8_t> stream;
    append_frame(stream, {}); // zero-length frame: just a 2-byte prefix of 0
    append_frame(stream, make_stock_directory(kAaplLocate, "AAPL"));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(std::get<StockDirectoryMessage>(received[0]).header.message_type, 'R');
}

TEST(ItchParserTest, UnknownTypeByteIsSkippedByFrameLength) {
    std::vector<uint8_t> stream = stream_with_aapl_directory();

    // 'P' (Trade) is not one of the 9 required types -- arbitrary body
    // bytes that would fail to parse as any known message's fields.
    std::vector<uint8_t> unknown_body = {'P', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    append_frame(stream, unknown_body);

    append_frame(stream, make_order_delete(kAaplLocate, 7));

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(std::get<StockDirectoryMessage>(received[0]).header.message_type, 'R');
    EXPECT_EQ(std::get<OrderDeleteMessage>(received[1]).order_reference_number, 7u);
}

TEST(ItchParserTest, MultiMessageStreamFiltersNonTargetTicker) {
    constexpr uint16_t kTslaLocate = 2; // TSLA is not in target_tickers()

    std::vector<uint8_t> stream;
    append_frame(stream, make_stock_directory(kAaplLocate, "AAPL"));
    append_frame(stream, make_stock_directory(kTslaLocate, "TSLA"));
    append_frame(stream, make_add_order(kAaplLocate, 1, 'B', 100, "AAPL", 1000000));  // retained
    append_frame(stream, make_add_order(kTslaLocate, 2, 'B', 200, "TSLA", 2000000));  // dropped
    append_frame(stream, make_order_delete(kAaplLocate, 1));                          // retained
    append_frame(stream, make_order_delete(kTslaLocate, 2));                          // dropped

    auto received = parse_all(stream);
    ASSERT_EQ(received.size(), 4u);

    EXPECT_EQ(std::get<StockDirectoryMessage>(received[0]).header.stock_locate, kAaplLocate);
    EXPECT_EQ(std::get<StockDirectoryMessage>(received[1]).header.stock_locate, kTslaLocate);

    const auto& add = std::get<AddOrderMessage>(received[2]);
    EXPECT_EQ(add.header.stock_locate, kAaplLocate);
    EXPECT_EQ(add.order_reference_number, 1u);

    const auto& del = std::get<OrderDeleteMessage>(received[3]);
    EXPECT_EQ(del.header.stock_locate, kAaplLocate);
    EXPECT_EQ(del.order_reference_number, 1u);
}

TEST(ItchParserTest, TargetTickersContainsExactlyTheFiveSettledSymbols) {
    const auto& tickers = target_tickers();
    EXPECT_EQ(tickers.size(), 5u);
    EXPECT_EQ(tickers.count("AAPL"), 1u);
    EXPECT_EQ(tickers.count("MSFT"), 1u);
    EXPECT_EQ(tickers.count("GOOGL"), 1u);
    EXPECT_EQ(tickers.count("AMZN"), 1u);
    EXPECT_EQ(tickers.count("NVDA"), 1u);
}
