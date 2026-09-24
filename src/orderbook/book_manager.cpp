#include "orderbook/book_manager.h"

#include <type_traits>
#include <variant>

namespace mde {

std::optional<uint16_t> BookManager::process(const ItchMessage& msg) {
    return std::visit(
        [this](auto&& m) -> std::optional<uint16_t> {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, SystemEventMessage> || std::is_same_v<T, StockDirectoryMessage>) {
                // Neither affects book state; BookManager deals only in
                // stock_locate, never tickers.
                return std::nullopt;
            } else {
                OrderBook& book = books_[m.header.stock_locate]; // creates if new

                if constexpr (std::is_same_v<T, AddOrderMessage> || std::is_same_v<T, AddOrderMPIDMessage>) {
                    book.add_order(m.order_reference_number, m.buy_sell_indicator, m.price_raw, m.shares);
                } else if constexpr (std::is_same_v<T, OrderExecutedMessage> ||
                                      std::is_same_v<T, OrderExecutedWithPriceMessage>) {
                    book.execute_order(m.order_reference_number, m.executed_shares);
                } else if constexpr (std::is_same_v<T, OrderCancelMessage>) {
                    book.cancel_order(m.order_reference_number, m.cancelled_shares);
                } else if constexpr (std::is_same_v<T, OrderDeleteMessage>) {
                    book.delete_order(m.order_reference_number);
                } else if constexpr (std::is_same_v<T, OrderReplaceMessage>) {
                    book.replace_order(m.original_order_reference_number, m.new_order_reference_number,
                                        m.price_raw, m.shares);
                }

                return m.header.stock_locate;
            }
        },
        msg);
}

const OrderBook* BookManager::get_book(uint16_t stock_locate) const {
    auto it = books_.find(stock_locate);
    if (it == books_.end()) {
        return nullptr;
    }
    return &it->second;
}

} // namespace mde
