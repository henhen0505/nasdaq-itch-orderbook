#include "parser/itch_parser.h"
#include "parser/binary_reader.h"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace mde {

namespace {

bool is_known_type(char type) {
    switch (type) {
        case 'S':
        case 'R':
        case 'A':
        case 'F':
        case 'E':
        case 'C':
        case 'X':
        case 'D':
        case 'U':
            return true;
        default:
            return false;
    }
}

MessageHeader parse_header(BinaryReader& r, char message_type) {
    MessageHeader h{};
    h.message_type = message_type;
    h.stock_locate = r.read_be_u16();
    h.tracking_number = r.read_be_u16();
    h.timestamp_ns = r.read_be_u48();
    return h;
}

SystemEventMessage parse_system_event(BinaryReader& r, const MessageHeader& header) {
    SystemEventMessage msg{};
    msg.header = header;
    msg.event_code = static_cast<char>(r.read_u8());
    return msg;
}

StockDirectoryMessage parse_stock_directory(BinaryReader& r, const MessageHeader& header) {
    StockDirectoryMessage msg{};
    msg.header = header;
    r.read_bytes(msg.stock, sizeof(msg.stock));
    msg.market_category = static_cast<char>(r.read_u8());
    msg.financial_status_indicator = static_cast<char>(r.read_u8());
    msg.round_lot_size = r.read_be_u32();
    msg.round_lots_only = static_cast<char>(r.read_u8());
    msg.issue_classification = static_cast<char>(r.read_u8());
    r.read_bytes(msg.issue_sub_type, sizeof(msg.issue_sub_type));
    msg.authenticity = static_cast<char>(r.read_u8());
    msg.short_sale_threshold_indicator = static_cast<char>(r.read_u8());
    msg.ipo_flag = static_cast<char>(r.read_u8());
    msg.luld_reference_price_tier = static_cast<char>(r.read_u8());
    msg.etp_flag = static_cast<char>(r.read_u8());
    msg.etp_leverage_factor = r.read_be_u32();
    msg.inverse_indicator = static_cast<char>(r.read_u8());
    return msg;
}

AddOrderMessage parse_add_order(BinaryReader& r, const MessageHeader& header) {
    AddOrderMessage msg{};
    msg.header = header;
    msg.order_reference_number = r.read_be_u64();
    msg.buy_sell_indicator = static_cast<char>(r.read_u8());
    msg.shares = r.read_be_u32();
    r.read_bytes(msg.stock, sizeof(msg.stock));
    msg.price_raw = r.read_be_u32();
    return msg;
}

AddOrderMPIDMessage parse_add_order_mpid(BinaryReader& r, const MessageHeader& header) {
    AddOrderMPIDMessage msg{};
    msg.header = header;
    msg.order_reference_number = r.read_be_u64();
    msg.buy_sell_indicator = static_cast<char>(r.read_u8());
    msg.shares = r.read_be_u32();
    r.read_bytes(msg.stock, sizeof(msg.stock));
    msg.price_raw = r.read_be_u32();
    r.read_bytes(msg.attribution, sizeof(msg.attribution));
    return msg;
}

OrderExecutedMessage parse_order_executed(BinaryReader& r, const MessageHeader& header) {
    OrderExecutedMessage msg{};
    msg.header = header;
    msg.order_reference_number = r.read_be_u64();
    msg.executed_shares = r.read_be_u32();
    msg.match_number = r.read_be_u64();
    return msg;
}

OrderExecutedWithPriceMessage parse_order_executed_with_price(BinaryReader& r, const MessageHeader& header) {
    OrderExecutedWithPriceMessage msg{};
    msg.header = header;
    msg.order_reference_number = r.read_be_u64();
    msg.executed_shares = r.read_be_u32();
    msg.match_number = r.read_be_u64();
    msg.printable = static_cast<char>(r.read_u8());
    msg.execution_price_raw = r.read_be_u32();
    return msg;
}

OrderCancelMessage parse_order_cancel(BinaryReader& r, const MessageHeader& header) {
    OrderCancelMessage msg{};
    msg.header = header;
    msg.order_reference_number = r.read_be_u64();
    msg.cancelled_shares = r.read_be_u32();
    return msg;
}

OrderDeleteMessage parse_order_delete(BinaryReader& r, const MessageHeader& header) {
    OrderDeleteMessage msg{};
    msg.header = header;
    msg.order_reference_number = r.read_be_u64();
    return msg;
}

OrderReplaceMessage parse_order_replace(BinaryReader& r, const MessageHeader& header) {
    OrderReplaceMessage msg{};
    msg.header = header;
    msg.original_order_reference_number = r.read_be_u64();
    msg.new_order_reference_number = r.read_be_u64();
    msg.shares = r.read_be_u32();
    msg.price_raw = r.read_be_u32();
    return msg;
}

// Parses the type-specific fields for one of the 8 non-'R' known types
// (the common header has already been consumed) and wraps the result in
// the variant. 'R' is handled separately by the caller since it also needs
// to update the stock_locate -> ticker map.
ItchMessage parse_body(char type, BinaryReader& r, const MessageHeader& header) {
    switch (type) {
        case 'S':
            return parse_system_event(r, header);
        case 'A':
            return parse_add_order(r, header);
        case 'F':
            return parse_add_order_mpid(r, header);
        case 'E':
            return parse_order_executed(r, header);
        case 'C':
            return parse_order_executed_with_price(r, header);
        case 'X':
            return parse_order_cancel(r, header);
        case 'D':
            return parse_order_delete(r, header);
        case 'U':
            return parse_order_replace(r, header);
        default:
            throw std::logic_error("parse_body: unexpected message type");
    }
}

} // namespace

const std::unordered_set<std::string>& target_tickers() {
    static const std::unordered_set<std::string> tickers = {"AAPL", "MSFT", "GOOGL", "AMZN", "NVDA"};
    return tickers;
}

void parse_buffer(const uint8_t* data, size_t size, const ItchMessageCallback& callback) {
    BinaryReader main_reader(data, size);
    std::unordered_set<uint16_t> allowed_locates;

    while (main_reader.remaining() >= 2) {
        uint16_t frame_len = main_reader.read_be_u16();

        if (main_reader.remaining() < frame_len) {
            throw BufferUnderrunError(
                "parse_buffer: frame at offset " + std::to_string(main_reader.position() - 2) +
                " claims " + std::to_string(frame_len) + " bytes but only " +
                std::to_string(main_reader.remaining()) + " remain in the buffer");
        }

        if (frame_len > 0) {
            // Scope a reader to exactly this frame's declared length so a
            // malformed frame (too small for its message type) throws
            // instead of reading into the next frame's bytes.
            BinaryReader frame_reader(data + main_reader.position(), frame_len);
            char type = static_cast<char>(frame_reader.read_u8());

            if (is_known_type(type)) {
                MessageHeader header = parse_header(frame_reader, type);

                if (type == 'R') {
                    StockDirectoryMessage msg = parse_stock_directory(frame_reader, header);
                    std::string ticker = trim_itch_text(msg.stock, sizeof(msg.stock));
                    if (target_tickers().count(ticker) > 0) {
                        allowed_locates.insert(header.stock_locate);
                    }
                    callback(ItchMessage{msg});
                } else if (allowed_locates.count(header.stock_locate) > 0) {
                    callback(parse_body(type, frame_reader, header));
                }
                // else: known type, but stock_locate not in the resolved
                // allowlist -- drop.
            }
            // else: unknown/unhandled type byte -- skip via frame length
            // below, without attempting field parsing.
        }

        main_reader.skip(frame_len);
    }
}

void parse_file(const std::string& path, const ItchMessageCallback& callback) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("parse_file: failed to open '" + path + "'");
    }

    std::streamsize size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("parse_file: failed to determine size of '" + path + "'");
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        throw std::runtime_error("parse_file: failed to read '" + path + "'");
    }

    parse_buffer(buffer.data(), buffer.size(), callback);
}

} // namespace mde
