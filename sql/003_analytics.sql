-- SQL analytics layer.
--
-- Six standalone, runnable queries against the schema in sql/001_schema.sql
-- (symbols, messages, trades, book_snapshots -- secondary indexes are in
-- sql/002_indexes.sql). Each query is mirrored verbatim (mod whitespace, no trailing
-- `;`) as a C++ string constant in src/analytics/queries.h --
-- tests/test_analytics.cpp asserts the two can't silently diverge.
--
-- price_raw / best_bid_raw / best_ask_raw are raw ITCH fixed-point integers
-- (divide by 10000.0 for decimal dollars). timestamp_ns is nanoseconds
-- since midnight, not since epoch (09:30 ET = 34200000000000,
-- 16:00 ET = 57600000000000).
--
-- Every query below was calibrated against the real loaded data (8,906
-- symbols, 55,748 messages, 1,856 trades, 934 book_snapshots, from the
-- ~100MB ITCH sample), not written blind -- see
-- each query's comment for the specific numbers that drove a design choice.

-- =============================================================================
-- Query 1: Time-bucketed VWAP (volume-weighted average price) per symbol.
-- =============================================================================
-- Non-trivial because: (a) `trades` only stores stock_locate, so the ticker
-- requires a JOIN to `symbols`; (b) time-bucketing is integer division on
-- timestamp_ns (nanoseconds since midnight, not a native date/time column)
-- -- DIV 60000000000 truncates each trade into its 60-second (1-minute)
-- window; (c) VWAP is a share-weighted average, which has no MySQL builtin
-- aggregate -- AVG(price_raw) would weight every trade equally regardless
-- of size, which is the wrong number for VWAP.
SELECT
    s.ticker,
    t.timestamp_ns DIV 60000000000 AS minute_bucket,
    SUM(t.price_raw * t.shares) / SUM(t.shares) / 10000.0 AS vwap,
    CAST(SUM(t.shares) AS UNSIGNED) AS total_volume,
    COUNT(*) AS trade_count
FROM trades t
JOIN symbols s ON s.stock_locate = t.stock_locate
GROUP BY s.ticker, minute_bucket
ORDER BY s.ticker, minute_bucket;

-- =============================================================================
-- Query 2: Rolling volatility of midpoint log returns.
-- =============================================================================
-- Non-trivial because: a three-stage CTE chain (midpoint -> log return ->
-- rolling sample stddev), each stage consuming a window-function output
-- from the previous one. The window is deliberately small -- 20 rows
-- (19 PRECEDING + CURRENT ROW), not a textbook 60-row window -- because
-- with only ~934 total book_snapshots rows spread across 5 symbols
-- (~187 rows/symbol), a 60-row window would leave too few complete windows
-- per symbol to mean anything. This is a real, small, truncated ~100MB
-- ITCH sample, not a full trading day of snapshots, so the window is sized
-- to the data actually loaded, not to a textbook default.
-- NULLIF around the LAG() denominator guards the LN() argument against a
-- divide-by-zero if a raw best_bid_raw/best_ask_raw of exactly 0 ever shows
-- up (shouldn't happen for a real resting order, but MySQL's LN() would
-- otherwise silently return NULL for a non-positive argument with no signal
-- as to why -- this makes the zero-guard explicit instead of relying on
-- that implicit behavior).
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
ORDER BY ticker, timestamp_ns;

-- =============================================================================
-- Query 3: Spread and depth summary by 1-second time bucket.
-- =============================================================================
-- Non-trivial because: JOIN to `symbols` for ticker, bucketing on raw
-- timestamp_ns (not a native TIME/DATETIME column), and three flavors of
-- aggregate (AVG/MIN/MAX) over a computed spread expression plus two more
-- AVGs over top-of-book depth, all in one pass.
-- Bucket width was checked against the actual loaded data before picking
-- it, per the brief for this stage: the real ~100MB sample's book-mutating
-- messages span roughly 19,815 seconds (04:00:00-09:30:15.6 ET -- see
-- query 6's comment for why the range starts in NASDAQ's pre-market
-- session) across 46,842 rows. At 1-second granularity that's ~8,571
-- distinct buckets averaging ~5.5 rows each -- dense enough to be
-- meaningful, not so dense that most buckets collapse to a single row. A
-- coarser bucket (e.g. 1 minute) would have hidden essentially all
-- intra-minute spread/depth movement for a sample this size.
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
ORDER BY s.ticker, second_bucket;

-- =============================================================================
-- Query 4: Order-to-trade ratio and cancel rate by symbol.
-- =============================================================================
-- Non-trivial because: conditional aggregation (CASE WHEN inside SUM)
-- buckets the one wide `messages` table into per-type counts in a single
-- pass instead of N separate queries, and NULLIF-guarded ratios mean a
-- symbol with zero orders (or zero executions) produces a NULL ratio
-- instead of a MySQL division-by-zero warning/undefined result.
-- HAVING total_orders > 0 matters here specifically (not decorative):
-- every one of the 8,906 symbols in `symbols` gets exactly one Stock
-- Directory ('R') message written to `messages` regardless of the 5-ticker
-- allowlist (that row is what resolves stock_locate -> ticker in the first
-- place), but 'R' matches none of this query's CASE WHEN branches. Without
-- the HAVING filter, the JOIN produces one all-zero/NULL row per symbol
-- for all 8,901 non-traded tickers, burying the 5 real ones -- confirmed
-- against the real loaded data, not a hypothetical.
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
ORDER BY s.ticker;

-- =============================================================================
-- Query 5: Imbalance regime vs. forward price movement.
-- =============================================================================
-- The one query here that asks a real microstructure question -- does
-- current top-of-book pressure predict near-term price direction -- rather
-- than just reporting a summary statistic.
-- Non-trivial because: a CTE with LEAD(mid, 10) for a forward-looking price
-- (10 snapshots ahead, not the textbook 60 -- same ~187-rows/symbol
-- reasoning as query 2's window size), imbalance computed from top-of-book
-- quantities only (this schema has no full-depth "total" quantity column,
-- so book_snapshots.bid_qty_at_best/ask_qty_at_best is genuinely all that's
-- available here -- not a simplification of a richer signal that exists
-- elsewhere in the schema), CASE-based regime bucketing, then
-- AVG/STDDEV_SAMP of the forward move in basis points per regime.
-- NULLIF guards both the imbalance denominator (top-of-book qty sum can be
-- zero) and the forward-move-in-bps denominator (mid could in principle be
-- zero for a raw/anomalous row).
-- bid_qty_at_best/ask_qty_at_best are INT UNSIGNED columns -- subtracting
-- them directly defaults to unsigned arithmetic in MySQL, which throws
-- "BIGINT UNSIGNED value is out of range" the moment ask exceeds bid (a
-- perfectly ordinary book state, confirmed against the real loaded data:
-- this fired on the very first run). CAST(... AS SIGNED) on both operands
-- forces signed arithmetic so a negative imbalance is a real negative
-- number, not an underflow error.
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
ORDER BY ticker, regime;

-- =============================================================================
-- Query 6: Intraday volume profile (30-minute buckets from the 09:30 open).
-- =============================================================================
-- Checked first against the real loaded data before writing this, per the
-- brief for this stage: a throwaway, non-DB driver (parse_file() +
-- timestamp tracking only, no DbConnection -- not committed, deleted after
-- use) run against data/itch_sample_12302019_100mb.bin showed the real
-- sample's timestamps run from 04:00:00.34 to 09:30:15.6 ET. 46,431 of the
-- 55,748 retained messages (83%) fall outside 09:30-16:00 -- this capture
-- began in NASDAQ's pre-market session and was truncated only ~15 seconds
-- after the regular-hours open. A strict
-- `BETWEEN 34200000000000 AND 57600000000000` filter (regular hours only)
-- would return next to nothing against this dataset -- one bucket with
-- ~15 seconds of trades -- which would silently look like "the query is
-- broken" rather than "the data doesn't cover this window".
-- Widened to 04:00:00-16:00:00 ET (14400000000000-57600000000000 ns),
-- NASDAQ's actual pre-market-through-close session -- a real market-session
-- boundary, not an arbitrary fudge for this one sample. Buckets stay
-- indexed relative to the 09:30 open (bucket 0 = 09:30-10:00), so
-- pre-market buckets come out negative (e.g. bucket -11 = 04:00-04:30).
-- FLOOR() is used instead of DIV so negative offsets floor toward
-- -infinity correctly instead of truncating toward zero.
-- timestamp_ns is BIGINT UNSIGNED -- subtracting the (larger) 09:30
-- constant from a pre-market (smaller) timestamp underflows unsigned
-- arithmetic and throws, exactly the case that dominates this dataset
-- (83% of rows are pre-market). CAST(... AS SIGNED) forces the subtraction
-- itself to be signed before FLOOR() divides it.
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
ORDER BY s.ticker, half_hour_bucket_from_open;
