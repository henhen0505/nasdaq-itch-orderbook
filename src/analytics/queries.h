#pragma once

#include <string>

namespace mde::queries {

// The 6 analytics queries as C++ string constants -- kept
// byte-for-byte (mod whitespace) identical to the bodies of the matching
// numbered query in sql/003_analytics.sql. That file carries the full "why
// this is non-trivial" commentary and the real-data numbers that drove each
// design choice (window sizes, bucket widths, the market-hours filter); the
// comments here are deliberately short, pointing back at it rather than
// duplicating it. tests/test_analytics.cpp asserts these two can't silently
// diverge (QueriesMatchSqlFile), so editing one without the other is a
// build-time-visible test failure, not a silent drift.
//
// No trailing `;` on any of these -- matches the existing
// split_statements()/execute() convention established in
// tests/test_persistence.cpp, where statements are executed one at a time
// via the X Protocol without a terminating semicolon.

// Query 1: time-bucketed VWAP per symbol (1-minute buckets).
inline const std::string kVwapByMinute = R"SQL(
SELECT
    s.ticker,
    t.timestamp_ns DIV 60000000000 AS minute_bucket,
    SUM(t.price_raw * t.shares) / SUM(t.shares) / 10000.0 AS vwap,
    CAST(SUM(t.shares) AS UNSIGNED) AS total_volume,
    COUNT(*) AS trade_count
FROM trades t
JOIN symbols s ON s.stock_locate = t.stock_locate
GROUP BY s.ticker, minute_bucket
ORDER BY s.ticker, minute_bucket
)SQL";

// Query 2: rolling (20-row window) volatility of midpoint log returns.
inline const std::string kRollingMidpointVolatility = R"SQL(
WITH midpoints AS (
    SELECT
        b.stock_locate,
        s.ticker,
        b.timestamp_ns,
        (b.best_bid_raw + b.best_ask_raw) / 2.0 / 10000.0 AS mid
    FROM book_snapshots b
    JOIN symbols s ON s.stock_locate = b.stock_locate
    WHERE b.best_bid_raw IS NOT NULL AND b.best_ask_raw IS NOT NULL
),
returns AS (
    SELECT
        ticker,
        timestamp_ns,
        mid,
        LN(mid / NULLIF(LAG(mid) OVER (PARTITION BY ticker ORDER BY timestamp_ns), 0)) AS log_return
    FROM midpoints
)
SELECT
    ticker,
    timestamp_ns,
    log_return,
    STDDEV_SAMP(log_return) OVER (
        PARTITION BY ticker ORDER BY timestamp_ns
        ROWS BETWEEN 19 PRECEDING AND CURRENT ROW
    ) AS rolling_volatility_20
FROM returns
WHERE log_return IS NOT NULL
ORDER BY ticker, timestamp_ns
)SQL";

// Query 3: spread and depth summary by 1-second time bucket.
inline const std::string kSpreadDepthByBucket = R"SQL(
SELECT
    s.ticker,
    b.timestamp_ns DIV 1000000000 AS second_bucket,
    AVG((b.best_ask_raw - b.best_bid_raw) / 10000.0) AS avg_spread,
    MIN((b.best_ask_raw - b.best_bid_raw) / 10000.0) AS min_spread,
    MAX((b.best_ask_raw - b.best_bid_raw) / 10000.0) AS max_spread,
    AVG(b.bid_qty_at_best) AS avg_bid_depth,
    AVG(b.ask_qty_at_best) AS avg_ask_depth,
    COUNT(*) AS snapshot_count
FROM book_snapshots b
JOIN symbols s ON s.stock_locate = b.stock_locate
WHERE b.best_bid_raw IS NOT NULL AND b.best_ask_raw IS NOT NULL
GROUP BY s.ticker, second_bucket
ORDER BY s.ticker, second_bucket
)SQL";

// Query 4: order-to-trade ratio and cancel rate by symbol.
inline const std::string kOrderToTradeRatio = R"SQL(
SELECT
    s.ticker,
    CAST(SUM(CASE WHEN m.msg_type IN ('A', 'F') THEN 1 ELSE 0 END) AS UNSIGNED) AS total_orders,
    CAST(SUM(CASE WHEN m.msg_type IN ('E', 'C') THEN 1 ELSE 0 END) AS UNSIGNED) AS total_executions,
    CAST(SUM(CASE WHEN m.msg_type = 'D' THEN 1 ELSE 0 END) AS UNSIGNED) AS total_deletes,
    CAST(SUM(CASE WHEN m.msg_type = 'X' THEN 1 ELSE 0 END) AS UNSIGNED) AS total_cancels,
    SUM(CASE WHEN m.msg_type IN ('A', 'F') THEN 1 ELSE 0 END)
        / NULLIF(SUM(CASE WHEN m.msg_type IN ('E', 'C') THEN 1 ELSE 0 END), 0) AS order_to_trade_ratio,
    (SUM(CASE WHEN m.msg_type = 'D' THEN 1 ELSE 0 END) + SUM(CASE WHEN m.msg_type = 'X' THEN 1 ELSE 0 END))
        / NULLIF(SUM(CASE WHEN m.msg_type IN ('A', 'F') THEN 1 ELSE 0 END), 0) AS cancel_rate
FROM messages m
JOIN symbols s ON s.stock_locate = m.stock_locate
GROUP BY s.ticker
HAVING total_orders > 0
ORDER BY s.ticker
)SQL";

// Query 5: imbalance regime vs. forward (10-snapshot-ahead) price movement.
inline const std::string kImbalanceRegimeVsForwardMove = R"SQL(
WITH book_state AS (
    SELECT
        b.stock_locate,
        s.ticker,
        b.timestamp_ns,
        (b.best_bid_raw + b.best_ask_raw) / 2.0 / 10000.0 AS mid,
        (CAST(b.bid_qty_at_best AS SIGNED) - CAST(b.ask_qty_at_best AS SIGNED))
            / NULLIF(b.bid_qty_at_best + b.ask_qty_at_best, 0) AS imbalance
    FROM book_snapshots b
    JOIN symbols s ON s.stock_locate = b.stock_locate
    WHERE b.best_bid_raw IS NOT NULL AND b.best_ask_raw IS NOT NULL
),
regimes AS (
    SELECT
        ticker,
        timestamp_ns,
        mid,
        imbalance,
        LEAD(mid, 10) OVER (PARTITION BY ticker ORDER BY timestamp_ns) AS forward_mid,
        CASE
            WHEN imbalance >= 0.3 THEN 'strong_bid'
            WHEN imbalance <= -0.3 THEN 'strong_ask'
            ELSE 'balanced'
        END AS regime
    FROM book_state
    WHERE imbalance IS NOT NULL
)
SELECT
    ticker,
    regime,
    COUNT(*) AS observation_count,
    AVG((forward_mid - mid) / NULLIF(mid, 0) * 10000.0) AS avg_forward_move_bps,
    STDDEV_SAMP((forward_mid - mid) / NULLIF(mid, 0) * 10000.0) AS stddev_forward_move_bps
FROM regimes
WHERE forward_mid IS NOT NULL
GROUP BY ticker, regime
ORDER BY ticker, regime
)SQL";

// Query 6: intraday volume profile, 30-minute buckets from the 09:30 open,
// widened to NASDAQ's 04:00-16:00 pre-market-through-close session -- see
// sql/003_analytics.sql for why a strict 09:30-16:00 filter returns almost
// nothing against the real loaded sample.
inline const std::string kIntradayVolumeProfile = R"SQL(
SELECT
    s.ticker,
    FLOOR((CAST(t.timestamp_ns AS SIGNED) - 34200000000000) / 1800000000000) AS half_hour_bucket_from_open,
    CAST(SUM(t.shares) AS UNSIGNED) AS total_volume,
    COUNT(*) AS trade_count,
    AVG(t.price_raw) / 10000.0 AS avg_price
FROM trades t
JOIN symbols s ON s.stock_locate = t.stock_locate
WHERE t.timestamp_ns BETWEEN 14400000000000 AND 57600000000000
GROUP BY s.ticker, half_hour_bucket_from_open
ORDER BY s.ticker, half_hour_bucket_from_open
)SQL";

} // namespace mde::queries
