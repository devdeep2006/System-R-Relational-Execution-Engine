# System R: A Cost-Based Relational Query Engine in C++17

A from-scratch relational database engine built around the ideas of IBM's System R
optimizer: a SQL front end, a page-based storage engine with a buffer pool and B+ tree
indexes, sketch-based statistics, a bottom-up dynamic-programming join optimizer with
**interesting orders** and **access path selection**, and a Volcano-style executor whose
join and sort operators **spill to disk** under a fixed memory budget.

Every I/O goes through a simulated disk that counts block transfers and seeks, so the
optimizer's cost estimates can be checked against what execution actually did.
`EXPLAIN ANALYZE` prints both.

~5,400 lines of engine code, ~1,100 lines of tests, no third-party dependencies.

## Highlights

| | |
|---|---|
| **Optimizer** | System R DP over relation subsets (left-deep, or bushy over all O(3^n) splits), interesting orders, cross products only when the join graph is disconnected; DP result verified equal to exhaustive enumeration for 120 random queries in both search spaces |
| **Join algorithms** | Grace hash join (recursive partitioning), sort-merge join (many-to-many), block nested loop (hashed blocks, theta joins), index nested loop |
| **Cost model** | Seeks vs. block transfers (t_S = 10 ms, t_T = 0.1 ms); block-transfer estimates within **1.7%** of measured disk I/O on every plan benchmarked (median 0.5%) |
| **Statistics** | One-pass `ANALYZE`: HyperLogLog NDV, Space-Saving heavy hitters (MCV lists), reservoir-sampled equi-depth histograms, physical-order correlation |
| **Estimation** | Median q-error **1.01** (p90 1.56) on a skewed workload vs. 2.70 (p90 39) for System R's default selectivity constants |
| **Query rewrites** | Equivalence classes (transitive join predicates), constant and range propagation, selection push-down, column pruning |
| **Storage** | 4 KB slotted pages, CLOCK buffer pool with pinning, disk-resident B+ tree with duplicate keys, direct-I/O spill files |
| **Testing** | 33 test cases incl. 3,290 differential executions against a brute-force reference evaluator across 11 optimizer configurations |

## Architecture

```
 SQL text
    |
    v
 Lexer / recursive-descent parser ........ src/sql        SELECT-FROM-WHERE-GROUP BY-ORDER BY-LIMIT,
    |                                                       JOIN ... ON, BETWEEN, IN, aggregates, DDL/DML
    v
 Binder .................................. src/planner    name resolution, type checking, conjunct
    |                                                       classification, equivalence classes,
    |                                                       constant/range propagation, column pruning
    v
 Optimizer ............................... src/optimizer  cardinality estimation -> cost model ->
    |                                                       DP over subsets with interesting orders
    v
 Physical plan --> Volcano executor ...... src/execution  open()/next()/close() operators with
    |                                                       per-operator memory budget M
    v
 Catalog + statistics .................... src/catalog    tables, indexes, TableStats/ColumnStats
 B+ tree ................................. src/index
 Heap files, buffer pool, spill files .... src/storage    every page access is counted
 Simulated disk (transfers, seeks)
```

## Optimizer

**Search.** Bottom-up DP over relation subsets (bitmask). For each subset `S` it enumerates
every split `S1 | S2`: all O(3^n) of them in bushy mode, or only splits whose inner side is a
single base relation in left-deep mode. Subsets that are not connected in the join graph are
skipped unless the query itself is disconnected, so cross products appear only when unavoidable.

**Interesting orders.** The DP table keeps the cheapest plan per *(subset, sort order)*,
not just per subset. An order is kept only if it is "interesting": a later merge join can
use it, or it matches `ORDER BY`. A plan that is more expensive but sorted can therefore
survive and win later, as when a merge join removes the final sort (see benchmarks).

**Access paths.** Each base relation gets a sequential scan, which carries the heap's
physical order if `ANALYZE` found it perfectly sorted. It also gets a B+ tree range scan for
every index whose column has sargable predicates, or whose order is interesting. Clustered
and unclustered indexes are costed differently, using Cardenas' formula for unclustered fetches.

**Join methods.** Hash join (either input as build side), sort-merge join (adds an external
sort for an input not already in key order), block nested loop (either orientation; handles
theta and cross joins), and index nested loop (inner = base relation with an index on the key).
The index nested loop cost uses the outer key's correlation statistic, which detects probes
that sweep the index in key order.

**Correctness of the search.** A test re-plans 120 random queries with pruning disabled
(`exhaustive = true`, keeping every plan) for both search spaces and asserts the DP's plan costs
exactly the same. Further tests assert that bushy plans are never costlier than left-deep plans,
and that interesting orders never increase cost.

## Cardinality estimation

`rows(S)` depends only on the relation set `S`, never on join order, so every plan for `S` agrees:

* **Selections.** For equality, the MCV list frequency, else `(1 - sum MCV) / (NDV - |MCV|)`.
  For ranges, MCV matches plus linear interpolation inside equi-depth histogram buckets.
  Range predicates on one column are merged into a single interval first (`x > 5 AND x < 10`
  is estimated as one range, not a product).
* **Joins.** For each equivalence class with members `d_1 <= ... <= d_k` (filtered NDVs) inside
  `S`, multiply by `1/d_2 * ... * 1/d_k`. Filtered NDV accounts for both row thinning (Yao's
  formula) and range filters on the column itself.
* **Inference.** `a.x = b.y AND b.y = 5` adds `a.x = 5`; `a.x = b.y AND a.x < 50` adds
  `b.y < 50`. The inferred filters are executed (fewer rows reach the join) and estimated
  with the other table's own histogram.

Three estimators can be switched at run time (`SET estimator = histogram | uniform | magic`)
for comparison.

## Cost model

Costs are simulated disk time, `seeks x t_S + transfers x t_T`, plus a small CPU term. Each
formula mirrors what the operator really does:

| Operator | Transfers (beyond reading inputs) | Notes |
|---|---|---|
| External sort | `2b * passes` | runs of M pages, fan-in `floor(M/b_b) - 1`, `passes = ceil(log_fanin(b/M))` |
| Grace hash join | `2(b_r + b_s) + 2P` per level | in-memory if build fits in M-2 pages; recursive levels when partitions overflow |
| Block nested loop | `b_s (1 + ceil(b_r / (M-2)))` | free if the inner fits in memory; seeks `2*ceil(b_s/b_b) + 2*blocks` |
| Index nested loop | min(random LRU estimate, sorted-sweep estimate) | sorted sweep when the outer is ordered or heap-correlated on the key |
| Index range scan | inner levels + leaves + heap pages | clustered: sequential heap pages; unclustered: Cardenas-capped random fetches |

## Execution engine

Volcano iterators with `open()/next()/close()`. Each blocking operator gets a memory budget of
`M` pages (`SET memory_pages = n`):

* **Hash join**: in-memory hash table with pipelined probe (keeps probe order); otherwise Grace
  partitioning into `P = ceil(1.2 * b / (M-2))` spill files, recursing with a new hash seed for
  partitions that still overflow (up to 3 levels).
* **Sort**: stable external merge sort with multi-pass k-way merging (binary heap).
* **Merge join**: buffers each group of equal right-side keys, so many-to-many joins are complete.
* **Block nested loop**: holds the inner in memory if it fits; otherwise spills the inner once and
  rescans it for every outer block, hashing the block on equi-join keys.
* **Index nested loop**, **index scan**, **hash aggregate** (COUNT/SUM/AVG/MIN/MAX),
  **top-N** (bounded heap for `ORDER BY ... LIMIT`), **limit**, **project**, **filter**.

`EXPLAIN ANALYZE` evicts the buffer pool first, then reports per-operator estimated vs actual
rows, run-time decisions (in-memory vs. spilled, partitions, merge passes, index probes), and
estimated vs measured I/O (here with only hash join enabled):

```
-> HashJoin build=right grace  on t.user_id = u.id  (rows=491463 cost=19703ms actual=500000; grace: 13 partitions, 1 level)
   -> SeqScan huge_transactions AS t  (rows=500000 cost=513ms actual=500000)
   -> SeqScan huge_users AS u  [sorted: u.id]  (rows=100000 cost=93.30ms actual=100000)
Estimated I/O: 11830 transfers, 1814 seeks
Measured  I/O: 11838 transfers (8252 reads, 3586 writes), 2405 seeks
```

## Storage

* **Slotted pages** (4 KB) with a self-describing tuple encoding (INT, DOUBLE, TEXT).
* **Buffer pool** with pin counts, dirty bits and CLOCK replacement; heap scans pin one frame at a time.
* **B+ tree** on INT keys stored in buffer-pool pages; duplicates handled by ordering on (key, RID);
  leaf chaining for range scans; node splits propagate to a new root.
* **Spill files** bypass the buffer pool. Each output buffer of `b_b` pages is written as one
  request to a contiguous extent, which is what makes the textbook seek formulas measurable.
* **Simulated disk** charges one transfer per page and one seek per request that does not
  continue where the previous one ended.

## Benchmarks

Full tables in [docs/BENCHMARKS.md](docs/BENCHMARKS.md) (`./build/sysr_bench`). Highlights:

| Experiment | Result |
|---|---|
| Cost model vs measured I/O (10 plans: Grace hash, merge + external sort, BNLJ, sorts, index scan) | transfers: median error 0.5%, max 1.7% |
| 100K x 500K join, M = 64 pages | optimizer picks BNLJ: 7.4 s simulated I/O vs 25.2 s for Grace hash join (both measured) |
| Join + `ORDER BY` key, interesting orders on vs off | merge join with no final sort: 1.32x less measured I/O |
| 3-way join with a selective filter | chained index nested loops: 151 ms vs 464 ms simulated I/O with INLJ disabled |
| Cardinality q-error, 34 queries on Zipf/log-normal data | median 1.01 / p90 1.56 vs 2.70 / 39 (System R constants) vs 6.53 / 411 (uniform) |
| 10-relation clique planning | left-deep 38 ms, bushy 530 ms (11x more plans) |
| 300 random 4-6 way joins | bushy found a cheaper plan for 7.3% of queries, by at most 2%, which empirically supports System R's left-deep restriction |

## Testing

`./build/sysr_tests` (or `ctest`) runs 33 test cases in ~15 s:

* **Storage**: tuple codec, slotted pages, heap files under eviction, CLOCK replacement and
  pinning, spill-file I/O accounting.
* **Index and statistics**: B+ tree vs `std::multimap` (60K entries with duplicates, 300 ranges),
  HyperLogLog error < 4%, Space-Saving top-k on Zipf data, histogram accuracy.
* **SQL**: grammar coverage, error reporting, binder errors, equivalence classes and propagation.
* **Operators**: multi-pass stable external sort, Grace hash join with recursive partitioning,
  many-to-many merge join, spilled BNLJ and theta joins, index nested loop, top-N, no leaked
  pins or spill pages, measured sort I/O within 3% of the cost formula.
* **Optimizer**: DP == exhaustive enumeration (240 checks), bushy <= left-deep, interesting
  orders never worse, left-deep shape, 12-relation chain/star planning, cross-product rules.
* **Differential**: 400 random 1-4 way queries (equi/theta/cross joins, filters, IN, aggregates,
  ORDER BY) x 8 configurations (forced join algorithms, bushy, magic estimator, 4-page memory),
  plus 90 spilling 2-way joins, all compared against a brute-force nested-loop evaluator.

## Build and run

Requires a C++17 compiler (GCC, Clang or MSVC) and CMake 3.16+.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/sysr_shell                          # REPL over the demo database
./build/sysr_shell queries/showcase.sql     # large-table scenarios (output in examples/)
./build/sysr_bench                          # benchmark tables
```

Without CMake: `CXX=clang++ ./build.sh`.

The demo database loads in about a second: the original project's `students`, `grades`,
`enrollments` and `courses` tables, `huge_users` (100K) and `huge_transactions` (500K), and
skewed `customers` (10K) and `orders` (200K) with Zipf customer ids, a 70/15/8/5/2 status mix,
late-year day skew and log-normal amounts.

### SQL and settings

```sql
SELECT [*, expr [AS alias], COUNT(*), SUM/AVG/MIN/MAX(expr)]
FROM t [alias] {, t | [INNER] JOIN t ON cond | CROSS JOIN t}
[WHERE cond] [GROUP BY expr, ...] [ORDER BY expr [ASC|DESC], ...] [LIMIT n];
EXPLAIN [ANALYZE] SELECT ...;
CREATE TABLE t (col INT | DOUBLE | TEXT, ...);   INSERT INTO t VALUES (...), (...);
CREATE INDEX [name] ON t (col);   ANALYZE [t];   SHOW TABLES;   SHOW STATS t;
SET bushy | interesting_orders | enable_hashjoin | enable_mergejoin | enable_nestloop
    | enable_indexnlj | enable_indexscan | cold_cache = on | off;
SET memory_pages = n;  SET io_chunk = n;  SET estimator = histogram | uniform | magic;
```

## What changed from v1

The first version transliterated textbook formulas but had no storage layer, depended on an
uncommitted SQL parser submodule, and built its join graph by searching the query text for
hard-coded table names. Its execution was also incorrect in places:

* BNLJ never rescanned its inner input, so a 4-way join returned 5 rows instead of 350.
* Merge join advanced both inputs on every match (dropping many-to-many results) and compared
  numbers as strings, and its inputs were never actually sorted.
* Intermediate join sizes reused base-table cardinalities, and a shadowed variable meant
  interesting orders were never propagated.

All of the original test queries are in [queries/regression.sql](queries/regression.sql) with
their now-correct results in [examples/regression.out](examples/regression.out).

## Limitations

Inner joins only (no outer joins or subqueries), no NULLs in stored data, INT-only indexes,
single-threaded, no transactions or recovery, and an in-memory simulated disk.
