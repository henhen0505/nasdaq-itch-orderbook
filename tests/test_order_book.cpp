// In-memory order-book engine tests. No database, no live connection
// required (unlike test_persistence.cpp). OrderBook's own tests call its
// methods directly; only the BookManager dispatch tests need synthetic
// ItchMessage values.

#include "orderbook/book_manager.h"
#include "orderbook/order_book.h"
#include "parser/itch_messages.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using namespace mde;

namespace {

MessageHeader make_header(char type, uint16_t stock_locate, uint64_t timestamp_ns) {
    MessageHeader h{};
    h.message_type = type;
    h.stock_locate = stock_locate;
    h.tracking_number = 1;
    h.timestamp_ns = timestamp_ns;
    return h;
}

AddOrderMessage make_add_order(uint16_t stock_locate, uint64_t order_ref, char side, uint32_t shares,
                                uint32_t price_raw) {
    AddOrderMessage msg{};
    msg.header = make_header('A', stock_locate, 34200500000000ull);
    msg.order_reference_number = order_ref;
    msg.buy_sell_indicator = side;
    msg.shares = shares;
    msg.price_raw = price_raw;
    return msg;
}

OrderExecutedMessage make_order_executed(uint16_t stock_locate, uint64_t order_ref, uint32_t executed_shares) {
    OrderExecutedMessage msg{};
    msg.header = make_header('E', stock_locate, 34201000000000ull);
    msg.order_reference_number = order_ref;
    msg.executed_shares = executed_shares;
    msg.match_number = 1;
    return msg;
}

OrderCancelMessage make_order_cancel(uint16_t stock_locate, uint64_t order_ref, uint32_t cancelled_shares) {
    OrderCancelMessage msg{};
    msg.header = make_header('X', stock_locate, 34201200000000ull);
    msg.order_reference_number = order_ref;
    msg.cancelled_shares = cancelled_shares;
    return msg;
}

OrderDeleteMessage make_order_delete(uint16_t stock_locate, uint64_t order_ref) {
    OrderDeleteMessage msg{};
    msg.header = make_header('D', stock_locate, 34201300000000ull);
    msg.order_reference_number = order_ref;
    return msg;
}

OrderReplaceMessage make_order_replace(uint16_t stock_locate, uint64_t original_order_ref, uint64_t new_order_ref,
                                        uint32_t new_price_raw, uint32_t new_shares) {
    OrderReplaceMessage msg{};
    msg.header = make_header('U', stock_locate, 34201400000000ull);
    msg.original_order_reference_number = original_order_ref;
    msg.new_order_reference_number = new_order_ref;
    msg.shares = new_shares;
    msg.price_raw = new_price_raw;
    return msg;
}

SystemEventMessage make_system_event(uint16_t stock_locate) {
    SystemEventMessage msg{};
    msg.header = make_header('S', stock_locate, 25200000000000ull);
    msg.event_code = 'O';
    return msg;
}

StockDirectoryMessage make_stock_directory(uint16_t stock_locate) {
    StockDirectoryMessage msg{};
    msg.header = make_header('R', stock_locate, 34200000000000ull);
    return msg;
}

} // namespace

// --- OrderBook: BBO, spread, midpoint -----------------------------------

TEST(OrderBookTest, BestBidIsHighestOfThreeBids) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 100);
    book.add_order(2, 'B', 1005000, 100);
    book.add_order(3, 'B', 1002000, 100);

    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 1005000u);
}

TEST(OrderBookTest, BestAskIsLowestAndSpreadAndMidpointAreCorrect) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 100); // $100.00
    book.add_order(2, 'S', 1010000, 100); // $101.00
    book.add_order(3, 'S', 1008000, 100); // $100.80

    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(*book.best_ask(), 1008000u);

    ASSERT_TRUE(book.spread().has_value());
    EXPECT_EQ(*book.spread(), 1008000u - 1000000u);

    ASSERT_TRUE(book.midpoint().has_value());
    EXPECT_NEAR(*book.midpoint(), (100.0 + 100.8) / 2.0, 1e-9);
}

// --- OrderBook: cancel / execute quantity mechanics ---------------------

TEST(OrderBookTest, PartialCancelReducesQuantityOrderStillPresent) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 1000);

    book.cancel_order(1, 400);

    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 1000000u);
    EXPECT_EQ(book.bid_qty_at_best(), 600u);
    EXPECT_EQ(book.quantity_anomaly_count(), 0u);
    EXPECT_EQ(book.unresolved_reference_count(), 0u);

    std::vector<std::pair<uint32_t, uint32_t>> depth = book.depth('B', 5);
    ASSERT_EQ(depth.size(), 1u);
    EXPECT_EQ(depth[0].second, 600u);
}

TEST(OrderBookTest, FullCancelRemovesOrderAndEmptyPriceLevel) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 1000);

    book.cancel_order(1, 1000); // cancels exactly the remaining amount

    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.bid_qty_at_best(), 0u);
    EXPECT_TRUE(book.depth('B', 5).empty());
    EXPECT_EQ(book.quantity_anomaly_count(), 0u);
}

TEST(OrderBookTest, ExecutePartialThenFullFillRemovesOrderAndLevel) {
    OrderBook book;
    book.add_order(1, 'S', 2000000, 800);

    book.execute_order(1, 300); // partial fill
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_EQ(book.ask_qty_at_best(), 500u);

    book.execute_order(1, 500); // exactly remaining -> full fill
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.ask_qty_at_best(), 0u);
    EXPECT_TRUE(book.depth('S', 5).empty());
    EXPECT_EQ(book.quantity_anomaly_count(), 0u);
}

TEST(OrderBookTest, CancelMoreThanRemainingClampsAndCountsAnomaly) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 500);

    book.cancel_order(1, 700); // more than remaining -- must clamp, not underflow

    EXPECT_FALSE(book.best_bid().has_value()); // order treated as fully removed
    EXPECT_EQ(book.bid_qty_at_best(), 0u);
    EXPECT_EQ(book.quantity_anomaly_count(), 1u);
    EXPECT_EQ(book.unresolved_reference_count(), 0u); // the reference itself was valid
}

TEST(OrderBookTest, DeleteRemovesOrderRegardlessOfRemainingQuantity) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 500);
    book.add_order(2, 'B', 1000000, 300); // second order at the same level so it survives

    book.execute_order(1, 100); // order 1 now has 400 remaining, not zero

    book.delete_order(1); // deletes regardless of remaining qty (400 != 0)

    // Level still exists (order 2 is still resting); only order 2's
    // quantity remains.
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(book.bid_qty_at_best(), 300u);
    EXPECT_EQ(book.unresolved_reference_count(), 0u);
}

// --- OrderBook: unresolved references ------------------------------------

TEST(OrderBookTest, UnresolvedReferenceOperationsNoOpAndCountRatherThanCrash) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 100); // one real order, to confirm it's untouched

    EXPECT_EQ(book.unresolved_reference_count(), 0u);

    book.execute_order(999, 50);
    EXPECT_EQ(book.unresolved_reference_count(), 1u);

    book.cancel_order(999, 50);
    EXPECT_EQ(book.unresolved_reference_count(), 2u);

    book.delete_order(999);
    EXPECT_EQ(book.unresolved_reference_count(), 3u);

    book.replace_order(999, 1000, 1001000, 200);
    EXPECT_EQ(book.unresolved_reference_count(), 4u);

    // No state change: the real order is untouched, and no new order was
    // added on behalf of the failed replace.
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 1000000u);
    EXPECT_EQ(book.bid_qty_at_best(), 100u);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.quantity_anomaly_count(), 0u);
}

// --- OrderBook: replace ---------------------------------------------------

TEST(OrderBookTest, ReplaceMovesOrderToNewPriceLevelSameSide) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 100); // original bid: $100.00, 100 shares

    book.replace_order(1, 2, 1002000, 250); // -> $100.20, 250 shares

    // Old level (only order there) is gone -- best_bid reflects the new
    // price, not the old one.
    ASSERT_TRUE(book.best_bid().has_value());
    EXPECT_EQ(*book.best_bid(), 1002000u);
    EXPECT_EQ(book.bid_qty_at_best(), 250u);

    // Still on the bid side, not flipped to the ask side.
    EXPECT_FALSE(book.best_ask().has_value());

    std::vector<std::pair<uint32_t, uint32_t>> depth = book.depth('B', 5);
    ASSERT_EQ(depth.size(), 1u);
    EXPECT_EQ(depth[0].first, 1002000u);
    EXPECT_EQ(depth[0].second, 250u);
}

// --- OrderBook: FIFO price-time priority -----------------------------------

TEST(OrderBookTest, ExecutePreservesFifoPriceTimePriority) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 300); // first in at this price
    book.add_order(2, 'B', 1000000, 500); // second in, same price

    book.execute_order(1, 300); // exactly the first order's quantity

    // Aggregate qty at the level reflects only order 2 remaining.
    EXPECT_EQ(book.bid_qty_at_best(), 500u);

    // Order 1 is fully consumed: cancelling it now is a no-op counted as
    // an unresolved reference, proving order 1 (not some portion of order
    // 2) was the one consumed.
    size_t unresolved_before = book.unresolved_reference_count();
    book.cancel_order(1, 1);
    EXPECT_EQ(book.unresolved_reference_count(), unresolved_before + 1);

    // Order 2 is untouched: cancelling its full remaining amount succeeds
    // cleanly (no anomaly) and empties the level.
    book.cancel_order(2, 500);
    EXPECT_EQ(book.quantity_anomaly_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

// --- OrderBook: imbalance ---------------------------------------------------

TEST(OrderBookTest, ImbalanceMatchesHandCalculatedExpectation) {
    OrderBook book;
    book.add_order(1, 'B', 1000000, 300);
    book.add_order(2, 'B', 1000000, 200); // total bid qty at best = 500
    book.add_order(3, 'S', 1001000, 100); // ask qty at best = 100

    // (500 - 100) / (500 + 100) = 400 / 600
    ASSERT_TRUE(book.imbalance().has_value());
    EXPECT_NEAR(*book.imbalance(), 400.0 / 600.0, 1e-9);
}

// --- OrderBook: empty book ---------------------------------------------------

TEST(OrderBookTest, EmptyBookQueriesReturnNulloptNotGarbage) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.spread().has_value());
    EXPECT_FALSE(book.midpoint().has_value());
    EXPECT_FALSE(book.imbalance().has_value());
    EXPECT_EQ(book.bid_qty_at_best(), 0u);
    EXPECT_EQ(book.ask_qty_at_best(), 0u);
    EXPECT_TRUE(book.depth('B', 5).empty());
    EXPECT_TRUE(book.depth('S', 5).empty());
}

// --- BookManager: per-symbol dispatch and isolation -------------------------

TEST(BookManagerTest, ProcessesInterleavedMessagesPerSymbolIndependently) {
    constexpr uint16_t kLocateA = 100;
    constexpr uint16_t kLocateB = 200;

    BookManager manager;

    ItchMessage add_a1 = make_add_order(kLocateA, 1, 'B', 100, 1000000);
    ItchMessage add_b1 = make_add_order(kLocateB, 2, 'B', 200, 2000000);
    ItchMessage add_a2 = make_add_order(kLocateA, 3, 'S', 150, 1005000);
    ItchMessage cancel_b1 = make_order_cancel(kLocateB, 2, 50);

    std::optional<uint16_t> r1 = manager.process(add_a1);
    std::optional<uint16_t> r2 = manager.process(add_b1);
    std::optional<uint16_t> r3 = manager.process(add_a2);
    std::optional<uint16_t> r4 = manager.process(cancel_b1);

    // process() reports which symbol it just mutated, matching the message
    // that was fed in -- not always the most-recently-touched symbol.
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, kLocateA);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, kLocateB);
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(*r3, kLocateA);
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(*r4, kLocateB);

    const OrderBook* book_a = manager.get_book(kLocateA);
    const OrderBook* book_b = manager.get_book(kLocateB);
    ASSERT_NE(book_a, nullptr);
    ASSERT_NE(book_b, nullptr);

    ASSERT_TRUE(book_a->best_bid().has_value());
    EXPECT_EQ(*book_a->best_bid(), 1000000u);
    ASSERT_TRUE(book_a->best_ask().has_value());
    EXPECT_EQ(*book_a->best_ask(), 1005000u);

    ASSERT_TRUE(book_b->best_bid().has_value());
    EXPECT_EQ(*book_b->best_bid(), 2000000u);
    EXPECT_EQ(book_b->bid_qty_at_best(), 150u); // 200 - 50 cancelled
    // Symbol A's ask order never appears in symbol B's book.
    EXPECT_FALSE(book_b->best_ask().has_value());

    // A stock_locate never seen produces no book.
    EXPECT_EQ(manager.get_book(999), nullptr);
}

TEST(BookManagerTest, SystemEventAndStockDirectoryDoNotCreateABook) {
    constexpr uint16_t kLocate = 300;
    BookManager manager;

    ItchMessage sys = make_system_event(kLocate);
    ItchMessage dir = make_stock_directory(kLocate);

    EXPECT_FALSE(manager.process(sys).has_value());
    EXPECT_FALSE(manager.process(dir).has_value());

    EXPECT_EQ(manager.get_book(kLocate), nullptr);
}

TEST(BookManagerTest, DispatchesEachMessageTypeToTheMatchingOperation) {
    constexpr uint16_t kLocate = 42;
    BookManager manager;

    std::optional<uint16_t> r1 = manager.process(ItchMessage(make_add_order(kLocate, 1, 'B', 500, 1000000)));
    std::optional<uint16_t> r2 =
        manager.process(ItchMessage(make_order_executed(kLocate, 1, 100))); // -> execute_order
    std::optional<uint16_t> r3 = manager.process(ItchMessage(make_add_order(kLocate, 2, 'B', 300, 1000000)));
    std::optional<uint16_t> r4 = manager.process(ItchMessage(make_order_delete(kLocate, 2))); // -> delete_order
    std::optional<uint16_t> r5 =
        manager.process(ItchMessage(make_order_replace(kLocate, 1, 3, 1002000, 250))); // -> replace_order

    // Every one of these is book-mutating, so every call reports kLocate.
    for (const std::optional<uint16_t>& r : {r1, r2, r3, r4, r5}) {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, kLocate);
    }

    const OrderBook* book = manager.get_book(kLocate);
    ASSERT_NE(book, nullptr);

    ASSERT_TRUE(book->best_bid().has_value());
    EXPECT_EQ(*book->best_bid(), 1002000u); // replaced to the new price
    EXPECT_EQ(book->bid_qty_at_best(), 250u); // replaced share count
    EXPECT_EQ(book->unresolved_reference_count(), 0u);
}
