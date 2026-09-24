# Market Data Engine

C++ engine that reconstructs order books from NASDAQ ITCH 5.0 binary feeds and persists the results to MySQL for analytics.

## What it does

Parses raw ITCH 5.0 binary data (length-prefixed, big-endian messages), maintains per-symbol order books with price-time priority, and writes everything -- messages, resolved trades, and periodic book snapshots -- to MySQL via batched, parameterized transactions.

Six analytics queries run against the persisted data: per-minute VWAP, rolling midpoint volatility, spread/depth summaries, order-to-trade ratios, imbalance regime vs. forward price movement, and intraday volume profiles.

## Stack

- C++17, CMake, MSVC (Visual Studio 2022)
- MySQL 8 via mysql-connector-cpp (X DevAPI, port 33060)
- GoogleTest
- vcpkg (manifest mode)

## Building

Requires vcpkg, CMake 3.20+, and a running MySQL 8 instance. See [SETUP.md](SETUP.md) for full environment setup from a clean machine.

```bash
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Running tests

```bash
# Set the database password (no default -- DbConfig::from_env() throws if unset)
export MDE_DB_PASSWORD=<your-password>

# Run all 61 tests
build/Release/mde_tests.exe
```

Tests that touch the database (`PersistenceTest`, `AnalyticsExactTest`, `AnalyticsRealDataTest`) require a running MySQL instance and `MDE_DB_PASSWORD` set. The remaining 45 tests (parser, order book, query-file consistency) run without a database.

## Project structure

```
src/
  parser/          ITCH 5.0 binary parser (9 message types, ticker allowlist)
  orderbook/       per-symbol order book (std::map price levels, O(1) order lookup)
  persistence/     MySQL writers (IngestWriter, SnapshotWriter), connection, loader
  analytics/       query runner, 6 analytics queries as C++ string constants
  util/            shared helpers (SQL file loading)
sql/
  001_schema.sql   table definitions (symbols, messages, trades, book_snapshots)
  002_indexes.sql  composite indexes for analytics queries
  003_analytics.sql  standalone SQL versions of the 6 analytics queries
bench/
  bench_inserts.cpp     insert throughput (unbatched vs. batched vs. multi-row)
  bench_queries.cpp     query latency before/after indexing
  bench_throughput.cpp  parser + order-book throughput (~550 MB/sec)
tests/
docs/
  performance_log.md  measured benchmarking results with methodology
```

## Benchmarking highlights

All numbers measured against real NASDAQ ITCH data (274 MB decompressed, 55,748 messages for 5 tickers).

- **Insert throughput:** batched transactions beat unbatched by ~36x (4,771 vs. 132 rows/sec)
- **Parser throughput:** ~550 MB/sec, no measurable order-book overhead at this data volume
- **Query indexing:** only 1 of 6 queries shows a genuine EXPLAIN-visible speedup from secondary indexes (the rest are too small to benefit); reported honestly rather than credited to cache warming

See [docs/performance_log.md](docs/performance_log.md) for the full results, methodology, and caveats.

## Data

This project uses a ~100 MB compressed NASDAQ ITCH 5.0 sample (Dec 30, 2019) filtered to 5 tickers: AAPL, MSFT, GOOGL, AMZN, NVDA. The binary sample file is not committed (gitignored under `data/`).

## License

MIT -- see [LICENSE](LICENSE).
