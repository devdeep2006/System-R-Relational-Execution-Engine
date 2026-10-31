# Benchmark results

Page size 4 KB, buffer pool 256 frames, t_S = 10 ms per seek, t_T = 0.1 ms per block. Simulated I/O = seeks x t_S + transfers x t_T.

## Join algorithms: huge_users (100K) x huge_transactions (500K), M = 64 pages

| Algorithm | Estimated cost (ms) | Measured transfers | Measured seeks | Simulated I/O (ms) | Wall time (ms) | Rows |
|---|---:|---:|---:|---:|---:|---:|
| Optimizer's choice (BlockNestedLoopJoin) | 9021 | 35683 | 384 | 7408 | 979 | 500000 |
| HashJoin | 19801 | 11838 | 2405 | 25234 | 241 | 500000 |
| MergeJoin | 26571 | 16506 | 2690 | 28551 | 384 | 500000 |
| BlockNestedLoopJoin | 9021 | 35683 | 384 | 7408 | 1395 | 500000 |
| IndexNestedLoopJoin | 79046 | 7827 | 7827 | 79053 | 278 | 500000 |

## Cost model vs measured disk I/O (cold buffer pool)

| Plan | Est. transfers | Measured | Error | Est. seeks | Measured | Error |
|---|---:|---:|---:|---:|---:|---:|
| Grace hash join (100K x 500K) | 11830 | 11838 | -0.1% | 1814 | 2405 | -24.6% |
| Grace hash join, M = 16 | 19002 | 19106 | -0.5% | 14338 | 10656 | 34.6% |
| Merge join + external sort | 16398 | 16506 | -0.7% | 2250 | 2690 | -16.4% |
| Merge join, both sides sorted, M = 32 | 22264 | 22490 | -1.0% | 3764 | 4291 | -12.3% |
| Block nested loop (spilled inner) | 35830 | 35683 | 0.4% | 416 | 384 | 8.3% |
| External sort of 500K rows | 15765 | 15873 | -0.7% | 2249 | 1754 | 28.2% |
| External sort, M = 16 | 33363 | 33929 | -1.7% | 6791 | 6286 | 8.0% |
| Hash join orders x customers | 2740 | 2740 | 0.0% | 2 | 1 | 100.0% |
| Clustered index range scan | 633 | 633 | 0.0% | 1 | 1 | 0.0% |
| Sequential scan + aggregate | 2638 | 2638 | 0.0% | 1 | 1 | 0.0% |

Median absolute error: transfers 0.5%, seeks 16.4%. Max: transfers 1.7%, seeks 100.0%.

## Interesting orders: join + ORDER BY on the join key

| Interesting orders | Plan | Estimated cost (ms) | Simulated I/O (ms) | Wall time (ms) |
|---|---|---:|---:|---:|
| on | MergeJoin | 26571 | 28551 | 634 |
| off | BlockNestedLoopJoin + final sort | 40354 | 37576 | 2054 |

Estimated cost ratio 1.52x, measured simulated I/O ratio 1.32x.

## Cardinality estimation on skewed data (34 queries on orders/customers: Zipf customer ids, skewed status/day, log-normal amounts)

| Estimator | Median q-error | 90th percentile | Max |
|---|---:|---:|---:|
| System R constants (1/10, 1/3) | 2.70 | 39.25 | 526.32 |
| Uniform min/max + 1/NDV | 6.53 | 411.28 | 1376.56 |
| MCV + equi-depth histogram | 1.01 | 1.56 | 4.09 |

## Optimizer scalability (planning only)

| Join graph | Relations | Left-deep: plans costed | Left-deep: ms | Bushy: plans costed | Bushy: ms |
|---|---:|---:|---:|---:|---:|
| chain | 4 | 52 | 0.4 | 84 | 0.0 |
| chain | 6 | 126 | 0.1 | 286 | 0.1 |
| chain | 8 | 232 | 0.1 | 680 | 0.8 |
| chain | 10 | 370 | 0.5 | 1330 | 1.3 |
| chain | 12 | 540 | 0.6 | 2300 | 2.2 |
| star | 4 | 64 | 0.1 | 100 | 0.1 |
| star | 6 | 346 | 0.4 | 646 | 0.8 |
| star | 8 | 1828 | 2.2 | 3592 | 2.3 |
| star | 10 | 9262 | 9.4 | 18442 | 19.6 |
| star | 12 | 45112 | 50.2 | 90124 | 89.0 |
| clique | 4 | 116 | 0.2 | 204 | 0.3 |
| clique | 6 | 750 | 1.2 | 2414 | 3.9 |
| clique | 8 | 4072 | 6.2 | 24208 | 40.4 |
| clique | 10 | 20450 | 38.0 | 228018 | 529.5 |

## Bushy vs left-deep plan quality (300 random 4-6 way joins, table sizes 50 to 80K)

Bushy search found a cheaper plan for 22 of 300 queries (7.3%); median improvement on those 1.00x, best 1.02x.
