-- MySQL schema for order-book message persistence.
--
-- Design notes:
--   * price_raw columns stay the raw ITCH fixed-point integer (INT UNSIGNED),
--     not DECIMAL -- faithful to the wire format. Divide by 10,000 only at
--     display/query time via itch_price_to_decimal() (C++) or `/ 10000.0`
--     in SQL.
--   * `messages` is deliberately one wide table across all 9 message types,
--     not one table per type -- simpler to query across message types by
--     time. Columns unused by a given msg_type are NULL (expected).
--   * Secondary indexes (beyond primary/foreign keys) are in
--     sql/002_indexes.sql, separate so they can be benchmarked
--     before/after.

CREATE TABLE symbols (
    stock_locate    SMALLINT UNSIGNED NOT NULL PRIMARY KEY,
    ticker          VARCHAR(8) NOT NULL,
    UNIQUE INDEX idx_ticker (ticker)
) ENGINE=InnoDB;

CREATE TABLE messages (
    id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    msg_type        CHAR(1) NOT NULL,
    timestamp_ns    BIGINT UNSIGNED NOT NULL,
    stock_locate    SMALLINT UNSIGNED NOT NULL,
    order_ref       BIGINT UNSIGNED,
    side            CHAR(1),
    shares          INT UNSIGNED,
    price_raw       INT UNSIGNED,
    new_order_ref   BIGINT UNSIGNED,
    match_number    BIGINT UNSIGNED,
    CONSTRAINT fk_msg_symbol FOREIGN KEY (stock_locate) REFERENCES symbols(stock_locate)
) ENGINE=InnoDB;

CREATE TABLE trades (
    id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    timestamp_ns    BIGINT UNSIGNED NOT NULL,
    stock_locate    SMALLINT UNSIGNED NOT NULL,
    order_ref       BIGINT UNSIGNED NOT NULL,
    price_raw       INT UNSIGNED NOT NULL,
    shares          INT UNSIGNED NOT NULL,
    match_number    BIGINT UNSIGNED NOT NULL,
    CONSTRAINT fk_trade_symbol FOREIGN KEY (stock_locate) REFERENCES symbols(stock_locate)
) ENGINE=InnoDB;

CREATE TABLE book_snapshots (
    id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    timestamp_ns    BIGINT UNSIGNED NOT NULL,
    stock_locate    SMALLINT UNSIGNED NOT NULL,
    best_bid_raw    INT UNSIGNED,
    best_ask_raw    INT UNSIGNED,
    bid_qty_at_best INT UNSIGNED,
    ask_qty_at_best INT UNSIGNED,
    CONSTRAINT fk_snap_symbol FOREIGN KEY (stock_locate) REFERENCES symbols(stock_locate)
) ENGINE=InnoDB;
