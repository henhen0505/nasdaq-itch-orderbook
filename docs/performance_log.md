# Performance Log — Order-Book Engine Benchmarking Study

## Abstract

This log reports three benchmarking experiments run against a C++ NASDAQ
ITCH 5.0 order-book engine backed by MySQL: (A) insert-throughput
comparison across three persistence strategies, (B) analytical query
latency before and after adding composite indexes, and (C) parser and
order-book processing throughput. All numbers are measured, not modeled or
estimated, on a single Windows 11 development machine against a real
274,452,074-byte NASDAQ ITCH sample and the project's live schema. The
headline results are that transaction batching yields an ~36x insert
throughput improvement over unbatched autocommit, that only one of six
analytical queries shows an index-attributable access-path change at the
current data volume, and that per-message order-book maintenance cost is
not distinguishable from measurement noise at this scale. Each finding is
reported with its supporting evidence and, where the result runs counter
to intuition, an explicit account of what was ruled out before accepting
it. Source: `bench/bench_inserts.cpp` (A), `bench/bench_queries.cpp` (B),
`bench/bench_throughput.cpp` (C).

## 1. Motivation and Research Questions

The engine's write path (`IngestWriter`) was designed around batched,
transacted inserts on the a priori assumption that batching would
dominate other candidate strategies. The analytics layer's indexing
migration (`sql/002_indexes.sql`) was designed around the assumption that
composite `(stock_locate, timestamp_ns)` indexes would measurably help the
six analytical queries it was built for. The order-book layer
(`BookManager`) was assumed to add negligible incremental cost over
parsing alone. This benchmarking pass exists to test those three design
assumptions against measurement rather than take them as given.

- **RQ1 (Part A):** How do unbatched autocommit, transaction-batched, and
  multi-row `INSERT` strategies compare in sustained insert throughput
  against the live schema, and does the ranking match the intuition that
  motivated `IngestWriter`'s design?
- **RQ2 (Part B):** Does adding `sql/002_indexes.sql`'s three composite
  indexes produce a measurable, access-path-level improvement in the six
  analytical queries at the data volume currently loaded (order 10^3-10^4
  rows per table)?
- **RQ3 (Part C):** What is the marginal CPU cost of incremental
  order-book maintenance (`BookManager::process()`) over parsing alone,
  and is that cost resolvable at the current data volume (55,748
  messages)?

## 2. Methodology

- **Dataset:** the real ~100 MB-compressed / 274,452,074-byte-decompressed
  NASDAQ ITCH 5.0 sample (`data/itch_sample_12302019_100mb.bin`), a
  truncated HTTP range-request download (parsing throws
  `BufferUnderrunError` at the exact truncation point — expected, not a
  crash). 55,748 messages are retained for the 5-ticker allowlist (AAPL,
  MSFT, GOOGL, AMZN, NVDA). Part A's synthetic insert rows are generated
  in-process by `bench_inserts.cpp` under a dedicated `stock_locate`
  (65000, confirmed outside the range of any real symbol's locate in this
  dataset); they are not derived from the ITCH file.
- **Repetition and reported statistics:** Parts A and C each report 3
  repetitions per measurement, with mean and sample standard deviation
  (n-1 denominator, matching this project's own `STDDEV_SAMP` convention
  in `sql/003_analytics.sql`). Part B is single-run-before/single-run-after
  within one program execution — indexing is a one-time schema change, not
  a repeatable treatment in the same sense as A and C — so its numbers
  carry more run-to-run variance risk than Parts A/C; this is treated as a
  limitation (Section 6), not smoothed over.
- **Environment:** a single machine, single-session measurement on
  ordinary Windows 11 hardware, with no CPU core isolation, no Turbo
  Boost/frequency-scaling control, and no quiescing of background
  processes. Numbers are real and reproducible to within the reported
  standard deviations on this machine, in this state, but are not
  precise enough to support sub-percent performance-engineering claims.
- **Data-integrity controls:** because Parts A and B write against the
  live `market_data_engine` database rather than a synthetic or
  disposable one, every run verifies real-data row counts
  (`symbols`=8,906, `messages`=55,748, `trades`=1,856,
  `book_snapshots`=934) before and after, and Part A additionally
  self-heals stray state from any prior interrupted run before capturing
  a baseline. Two integrity incidents were caught by these controls during
  development rather than by static inspection; see Appendix A for the
  detail, since the mechanism (not just the postmortem) is relevant to
  anyone extending these benchmarks against live data.

## 3. Results

### 3.1 Part A — Insert throughput by persistence strategy

100,000 rows/method, 3 runs/method, against the real `messages` table
under `stock_locate`=65000.

| Method | Rows | Mean rows/sec | Std dev | Runs |
|---|---|---|---|---|
| 1. Unbatched (autocommit) | 100,000 | 132.37 | 8.64 | 3 |
| 2. Batched (transaction, 5,000 rows/tx) | 100,000 | 4,771.27 | 54.01 | 3 |
| 3. Multi-row `INSERT` (5,000 rows/batch) | 100,000 | 131.08 | 1.14 | 3 |

Real-data row counts matched exactly before and after every run.

### 3.2 Part B — Analytical query latency before/after indexing

All 6 queries run with `EXPLAIN` before and after applying
`sql/002_indexes.sql` to the live database.

| Query | Before (ms) | After (ms) | Speedup | EXPLAIN plan changed? |
|---|---|---|---|---|
| Q1 VWAP by minute | 24.756 | 5.879 | 4.21x | No — both `type=ALL` (full scan) |
| Q2 Rolling midpoint volatility | 23.257 | 12.087 | 1.92x | No — both `type=ALL` |
| Q3 Spread/depth by 1s bucket | 16.216 | 12.315 | 1.32x | No — both `type=ALL` |
| Q4 Order-to-trade ratio | 251.391 | 223.589 | 1.12x | **Yes** — `messages` full scan (54,634 est. rows) → indexed `ref` lookup via `idx_messages_locate_time` (6 est. rows) |
| Q5 Imbalance regime vs. forward move | 14.446 | 9.605 | 1.50x | No — both `type=ALL` |
| Q6 Intraday volume profile | 5.934 | 8.101 | 0.73x (slower) | No — both `type=ALL`, identical plan |

### 3.3 Part C — Parser and order-book throughput

No database involved. Run directly against the real
274,452,074-byte ITCH sample; 55,748 messages retained per run in both
conditions, matching the real load count exactly.

| Measurement | Mean | Std dev | Runs | MB/sec | Messages/sec |
|---|---|---|---|---|---|
| Parser only (no book, no DB) | 0.500551 s | 0.007309 s | 3 | 548.30 | 111,373 |
| Parser + order-book (`BookManager::process()`, no DB) | 0.493574 s | 0.004498 s | 3 | 556.05 | 112,948 |

Per-run elapsed times — parser only: 0.495353 s, 0.497392 s, 0.508909 s;
parser + order-book: 0.494110 s, 0.497780 s, 0.488831 s.

## 4. Discussion

**RQ1 — batching dominates, and the mechanism matters more than the
magnitude.** The ~36x gap between unbatched and batched (132 vs. 4,771
rows/sec) is consistent with one `start_transaction()`/`commit()` per
5,000 rows eliminating the implicit per-row transaction/fsync overhead
that dominates Method 1. This confirms the assumption behind
`IngestWriter`'s design. The more informative result is what did *not*
help: Method 3 (multi-row `INSERT`, same 5,000-row batch granularity as
Method 2) performed no better than the unbatched baseline, contradicting
the general expectation that fewer round trips should always help. A
standalone check isolated the cause to the client side: a single
`.bind(vector<mysqlx::Value>)` call carrying 5,000 rows' worth of
parameters (35,000 individual values) took 32.68 seconds on its own,
independent of server-side execution cost. This is a specific,
falsifiable claim about `mysql-connector-cpp` 9.7.0's X DevAPI parameter
serialization, not a general claim about multi-row `INSERT` — see Section
5 for how to test that distinction directly.

**RQ2 — indexing helped exactly where it was expected to, and nowhere
else, at this scale.** Only Q4 shows an `EXPLAIN`-visible access-path
change, and its estimated row count per join iteration dropped four
orders of magnitude (54,634 → 6) — but wall-clock improvement for that
same query was only 1.12x, smaller than several queries whose plan didn't
change at all. The likely explanation is that Q4's cost is dominated by
something downstream of the join (`Using temporary; Using filesort` on a
wide conditional-aggregation `GROUP BY`, present in both plans), so a
cheaper join path does not translate proportionally into wall-clock time.
For Q1, Q2, Q3, and Q5, `EXPLAIN` reports an identical access path
(`type=ALL`, no `key` used) before and after, yet wall-clock time still
improved 1.3x-4.2x — consistent with buffer-pool/page-cache warming
between the "before" and "after" runs within the same program execution,
not with the indexes, since these tables (934-1,856 rows for
`book_snapshots`/`trades`) are small enough that MySQL's optimizer
correctly judges a full scan cheaper than an index lookup regardless. Q6's
nominal 0.73x "slowdown" is noise against a byte-for-byte identical plan,
and was predicted in `sql/002_indexes.sql`'s design comments before this
benchmark ran: Q6 filters on timestamp only, with no `stock_locate`
predicate, so none of the three composite indexes added here are expected
to serve it.

**RQ3 — the order book's incremental cost is real but unresolved, not
zero.** The measured delta (parser+book faster than parser-only by 1.39%)
is smaller than the per-condition standard deviation (1-1.5% of the ~0.5 s
mean), so the two conditions are not statistically distinguishable at this
sample size and data volume. The correct reading is "no *measurable*
order-book overhead within a ~1.5% noise floor at 55,748 messages," which
is a claim about the resolution of this experiment, not a claim that
`BookManager::process()` is free — it does real work (O(log n) map
operations per book mutation across ~46,842 book-mutating messages) that
this benchmark is simply not powered to detect.

## 5. Open Questions

1. **Is the multi-row `INSERT` slowness connector-specific or
   protocol-inherent?** The 32.68-second single-call cost was attributed
   to client-side parameter serialization in `mysql-connector-cpp`
   9.7.0's X DevAPI, but this was not compared against the `libmysql`
   plain C API path (`mysql_real_connect`, prepared statements with
   array binding) that this project pre-validated but never used. Does
   the same batch size perform comparably under `libmysql`, or is the
   cost specific to X DevAPI's `bind(vector<Value>)` implementation?
2. **What is the actual scaling curve of `bind()` cost vs. parameter
   count?** Only one batch size (5,000 rows / 35,000 values) was
   measured. Is the client-side cost linear, superlinear, or does it have
   a cliff at some threshold? A sweep across batch sizes (e.g., 100, 500,
   1,000, 2,500, 5,000, 10,000 rows) would distinguish "multi-row INSERT
   is always this bad" from "multi-row INSERT is fine below some batch
   size and pathological above it" — the latter would materially change
   the practical recommendation.
3. **At what table size would the composite indexes actually win?** The
   current tables (934-55,748 rows) are small enough that MySQL's
   optimizer rationally prefers full scans for 5 of 6 queries. What row
   count is the crossover point where `idx_messages_locate_time` and its
   siblings start changing `EXPLAIN` plans for Q1, Q2, Q3, and Q5? This is
   directly testable by reloading a larger ITCH sample (or synthetically
   scaling the existing one) and re-running Part B.
4. **Is Q4's join-path-vs-wall-clock disconnect really the `GROUP BY`
   filesort, or something else?** The hypothesis in Section 4 was not
   isolated — no query was run with the aggregation stripped out to
   confirm the join alone is now cheap and the filesort is what remains
   expensive. `EXPLAIN ANALYZE` (unavailable in the connector path used
   here, but available via the MySQL CLI) would give per-operation timing
   instead of a plan-shape inference.
5. **How much of Part B's "improvement" on the unindexed-plan queries is
   genuinely cache warming, and would randomizing before/after order
   change the result?** The current design always runs "before" first,
   so warming and index effects are confounded by construction. Running
   the two conditions in randomized order across repetitions (with a
   cache-drop or a warm-up pass before each) would separate them.
6. **Does the order-book overhead become resolvable at higher message
   volume, or does it stay within noise proportionally?** RQ3 is
   currently underpowered, not negative. A back-of-envelope power
   calculation — given the current ~0.005 s per-run std dev and a assumed
   true effect size, how many runs or how much more data would be needed
   to resolve it at p<0.05 — has not been done and would clarify whether
   this is worth re-measuring at all.

## 6. Limitations / Threats to Validity

- **Single machine, single session, uncontrolled environment.** No core
  pinning, no frequency-scaling lockout, no isolation from OS/background
  load. Results are internally consistent (reproducible within reported
  std devs on this machine) but not portable performance claims.
- **Small n (3 runs) for Parts A and C.** Sufficient to report a mean and
  sample std dev, not sufficient for tight confidence intervals or to
  rule out a heavy-tailed run-time distribution.
- **Part B is not repeated.** Single before/after pair per query means
  its numbers are the least statistically defensible in this log, despite
  being reported to millisecond precision. Treat the *direction* and
  *EXPLAIN*-plan findings as reliable; treat the specific ms values and
  speedup ratios as indicative only.
- **Data volume is small relative to the engine's design target.** 55,748
  messages / ≤55,748-row tables is orders of magnitude below what a
  production order-book pipeline would ingest per session. Several
  conclusions here (index non-benefit, order-book noise floor) are
  properties of *this data volume*, not general properties of the
  design — Section 5's open questions largely follow from this.
- **No profiling instrumentation.** All timing is wall-clock at the
  operation level (`std::chrono` around `execute()`/`process()` calls),
  not sampled or instrumented profiling. Attributions of cost to specific
  sub-operations (e.g., "the filesort is what's expensive in Q4") are
  inferences from `EXPLAIN` output, not direct measurement.

## 7. Future Work / Possible Improvements

- Re-run Part A's Method 3 against the `libmysql` C API path with
  prepared-statement array binding, to separate "X DevAPI's `bind()` is
  slow for large vectors" from "multi-row `INSERT` from C++ against MySQL
  is inherently slow at this batch size."
- Sweep batch size for Method 3 (100 through 10,000+ rows) to find the
  actual client-side scaling curve rather than reporting a single point.
- Reload a substantially larger ITCH sample (or synthetically replicate
  the existing one) specifically to find the row-count crossover where
  `sql/002_indexes.sql`'s indexes start changing `EXPLAIN` plans for the
  five queries that currently show no access-path change.
- Use `EXPLAIN ANALYZE` via the MySQL CLI (bypassing the connector,
  which doesn't expose it) to get per-operation timing for Q4 and confirm
  or reject the filesort-dominates hypothesis directly.
- Randomize before/after run order in Part B, or add an explicit warm-up
  pass before each condition, to separate cache-warming effects from
  index effects.
- Run a power calculation for Part C given the observed noise floor, to
  determine whether more runs or more data would ever resolve the
  order-book's incremental cost, or whether it is genuinely negligible at
  any volume this engine is likely to see.
- Consider a controlled benchmarking environment (pinned cores, disabled
  frequency scaling, dedicated machine or isolated cloud instance) if any
  of the above experiments are meant to produce externally comparable
  numbers rather than internally consistent ones.

## 8. Conclusion

Of the three design assumptions this pass set out to test, one was
confirmed as expected (batching), one was partially confirmed in a way
that surfaces new questions rather than closing them (indexing helps
exactly one query, for query-shape reasons this log did not fully
isolate), and one remains genuinely open (order-book overhead is
unresolved, not disproven). The most actionable near-term finding is the
multi-row `INSERT` result: it is specific, reproducible, and worth
verifying against the `libmysql` path before treating it as a general
property of the approach rather than of this connector version.

## Appendix A: Data-integrity controls (incident log)

Two incidents surfaced during Part A's development, caught by this
project's own verification discipline rather than by code review alone.
Recorded here because the *mechanism* — real-data row-count checks before
and after every benchmark run — is what caught both, and is relevant to
anyone extending these benchmarks against live data rather than a
disposable database.

1. **Out-of-range `stock_locate` collided with a real symbol's primary
   key.** `kBenchStockLocate` was originally `70000`, outside
   `stock_locate`'s actual `SMALLINT UNSIGNED` (0-65535) column range.
   C++ silently truncated the constant to `4464` at compile time (70000
   mod 65536) — a real symbol's actual locate (Johnson & Johnson,
   `JNJ`). The benchmark's `ON DUPLICATE KEY UPDATE` and per-run cleanup
   `DELETE` then operated on `JNJ`'s real row instead of a disposable one.
   Caught by the post-run row-count check (`messages` came back at
   55,747, not 55,748), not by inspection. Fixed by setting
   `kBenchStockLocate = 65000`, checked against the real data's actual
   max locate (8,906) rather than assumed safe, and restoring the real
   data from `data/itch_sample_12302019_100mb.bin`.
2. **An interrupted run left stray state that a later run misread as
   corruption.** A run terminated externally (service/session
   interruption, not program logic) skipped its own cleanup path, leaving
   a stray `symbols` row and partial `messages` rows at the bench locate.
   The next run's "capture baseline, then compare after cleanup" logic
   had no way to distinguish that leftover state from real data and
   failed with a confusing mismatch despite the real data being intact.
   Fixed by making `bench_inserts.cpp` defensively clean any stray bench
   rows *before* capturing baseline, so it is self-healing across an
   external interruption rather than requiring manual investigation each
   time; a related race (two instances retrying against the same locate
   before the first had fully exited) was resolved by confirming only one
   instance runs at a time.
