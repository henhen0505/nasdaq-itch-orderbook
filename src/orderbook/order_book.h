#pragma once

#include "orderbook/order.h"
#include "orderbook/price_level.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mde {

// Reconstructed in-memory limit order book for one symbol, rebuilt purely
// from ITCH message fields (add/execute/cancel/delete/replace). No
// database interaction -- persistence is handled separately by
// SnapshotWriter (src/persistence/snapshot_writer.h).
//
// Side convention matches ITCH's buy_sell_indicator field: 'B' = bid,
// 'S' = ask (see AddOrderMessage in src/parser/itch_messages.h).
//
// Unresolved-reference / quantity-anomaly policy: mutating calls that
// reference an order_ref not currently resting in this book (e.g. the
// originating Add Order predates the capture window -- the same scenario
// TradePriceResolver::unresolved_execution_count() already handles in
// src/persistence/ingest_writer.h) are no-op'd rather than crashing, and
// counted via unresolved_reference_count(). Execute/cancel calls whose
// requested quantity exceeds the order's remaining_qty are clamped to a
// full fill (never underflowed) and counted via quantity_anomaly_count().
class OrderBook {
public:
    // --- Mutating operations, one per ITCH message type --------------------

    // Add Order ('A'/'F'): inserts a new resting order, creating the price
    // level on that side if it doesn't already exist.
    void add_order(uint64_t order_ref, char side, uint32_t price_raw, uint32_t shares);

    // Order Executed ('E') / Executed with Price ('C'): reduces the
    // order's remaining_qty (and its price level's total_qty) by
    // executed_shares. Removes the order once remaining_qty hits zero, and
    // removes the price level once its total_qty hits zero -- best_bid()/
    // best_ask() must never return an empty level.
    void execute_order(uint64_t order_ref, uint32_t executed_shares);

    // Order Cancel ('X'): identical reduction/cleanup mechanics to
    // execute_order() -- no trade implied, but the bookkeeping is the same.
    void cancel_order(uint64_t order_ref, uint32_t cancelled_shares);

    // Order Delete ('D'): full removal of the order regardless of its
    // remaining quantity, with the same price-level cleanup.
    void delete_order(uint64_t order_ref);

    // Order Replace ('U'): OrderReplaceMessage carries no side field, so
    // original_order_ref is looked up first to determine which side it's
    // on, *then* the original is deleted and a new order is added on that
    // same side, at the new price, with the new share count. This ordering
    // matters -- look up before delete.
    void replace_order(uint64_t original_order_ref, uint64_t new_order_ref, uint32_t new_price_raw,
                        uint32_t new_shares);

    // --- Queries -------------------------------------------------------

    // Price of the top level on each side (raw ITCH fixed-point units --
    // see itch_price_to_decimal() to convert). nullopt if that side is
    // empty.
    std::optional<uint32_t> best_bid() const;
    std::optional<uint32_t> best_ask() const;

    // best_ask() - best_bid(), raw fixed-point units. nullopt if either
    // side is empty.
    std::optional<uint32_t> spread() const;

    // (best_bid() + best_ask()) / 2, converted to decimal dollars via
    // itch_price_to_decimal() scaling. nullopt if either side is empty.
    std::optional<double> midpoint() const;

    // total_qty of the top-of-book level on each side, 0 if that side is
    // empty. Corresponds directly to book_snapshots.bid_qty_at_best /
    // ask_qty_at_best (sql/001_schema.sql).
    uint32_t bid_qty_at_best() const;
    uint32_t ask_qty_at_best() const;

    // (bid_qty_at_best - ask_qty_at_best) / (bid_qty_at_best +
    // ask_qty_at_best). nullopt if both sides are empty (zero denominator).
    std::optional<double> imbalance() const;

    // Top n_levels (price_raw, total_qty) pairs on the given side ('B' or
    // 'S'), best-first. Fewer than n_levels if the book doesn't have that
    // many levels on that side.
    std::vector<std::pair<uint32_t, uint32_t>> depth(char side, size_t n_levels) const;

    // Count of execute/cancel/delete/replace calls referencing an
    // order_ref not currently resting in this book. No-op'd rather than
    // crashing; see the class comment for the full policy.
    size_t unresolved_reference_count() const;

    // Count of execute/cancel calls whose requested quantity exceeded the
    // order's remaining_qty. Clamped to a full fill rather than
    // underflowing remaining_qty; see the class comment for the full
    // policy.
    size_t quantity_anomaly_count() const;

private:
    // bids_ sorted descending (best bid = highest price = begin()); asks_
    // sorted ascending (best ask = lowest price = begin()).
    using BidMap = std::map<uint32_t, PriceLevel, std::greater<uint32_t>>;
    using AskMap = std::map<uint32_t, PriceLevel>;

    // O(1) lookup from order_ref to everything needed to mutate it without
    // scanning: which side it's on, a pointer directly to its PriceLevel
    // (stable across insert/erase of *other* keys in bids_/asks_, since
    // std::map is node-based), and an iterator directly into that level's
    // FIFO list.
    struct OrderLocation {
        char side;
        PriceLevel* level;
        std::list<Order>::iterator it;
    };

    // Inserts a new order into side_map (bids_ or asks_), creating the
    // price level if needed, and indexes it in orders_by_ref_. Caller must
    // pass the map matching `side` ('B' -> bids_, 'S' -> asks_).
    template <typename MapT>
    void insert_order(MapT& side_map, char side, uint64_t order_ref, uint32_t price_raw, uint32_t shares);

    // Shared reduction logic for execute_order()/cancel_order(): looks up
    // order_ref (no-op + unresolved_reference_count_ if absent), clamps
    // qty to the order's remaining_qty (+ quantity_anomaly_count_ if it
    // was clamped), decrements remaining_qty and the price level's
    // total_qty by the applied amount, and fully removes the order (via
    // erase_order()) once remaining_qty reaches zero.
    void reduce_order(uint64_t order_ref, uint32_t qty);

    // Fully removes the order at ref_it from its price level's FIFO list
    // and from orders_by_ref_, then removes the price level itself from
    // bids_/asks_ if that was its last order. Does NOT adjust total_qty --
    // callers (delete_order(), reduce_order()'s full-consumption path)
    // are responsible for that before calling this.
    void erase_order(std::unordered_map<uint64_t, OrderLocation>::iterator ref_it);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<uint64_t, OrderLocation> orders_by_ref_;

    size_t unresolved_reference_count_ = 0;
    size_t quantity_anomaly_count_ = 0;
};

} // namespace mde
