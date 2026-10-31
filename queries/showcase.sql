-- Large-table scenarios. Run: sysr_shell queries/showcase.sql
-- Costs are simulated disk time: 10 ms per seek, 0.1 ms per 4 KB block transfer.

-- TC10: 100K x 500K equi-join with M = 64 pages of work memory. Neither input fits, so the
-- optimizer weighs Grace hash join (random partition I/O), sort-merge, and block nested loop
-- (sequential rescans). With seeks 100x dearer than transfers, BNLJ's ~400 seeks win.
EXPLAIN ANALYZE SELECT u.region, t.amount
FROM huge_users u JOIN huge_transactions t ON u.id = t.user_id;

-- TC11: same join with ORDER BY on the join key. huge_users is physically sorted on id, so
-- a merge join emits rows already in id order (an "interesting order") and needs no final sort.
EXPLAIN ANALYZE SELECT u.id, t.amount
FROM huge_users u JOIN huge_transactions t ON u.id = t.user_id
ORDER BY u.id;

-- The same query with interesting orders disabled: the cheapest join, then a full external sort.
SET interesting_orders = off;
EXPLAIN SELECT u.id, t.amount
FROM huge_users u JOIN huge_transactions t ON u.id = t.user_id
ORDER BY u.id;
SET interesting_orders = on;

-- Access path selection: a narrow range uses the clustered B+ tree on huge_users.id.
EXPLAIN ANALYZE SELECT * FROM huge_users WHERE id BETWEEN 5000 AND 5100;

-- Transitive range inference: u.id BETWEEN 100 AND 103 plus u.id = t.user_id implies
-- t.user_id BETWEEN 100 AND 103, so both sides become index range scans feeding a merge join.
EXPLAIN ANALYZE SELECT u.id, t.tx_id, t.amount
FROM huge_users u JOIN huge_transactions t ON u.id = t.user_id
WHERE u.id BETWEEN 100 AND 103;

-- Index nested loop joins: a selective filter on non-key columns leaves one customer, so
-- probing two B+ trees beats scanning 600K rows. (Transitive closure also lets the second
-- join use c.c_id = t.user_id directly.) Compare with index nested loops disabled.
EXPLAIN ANALYZE SELECT c.c_id, u.region, t.tx_id, t.amount
FROM customers c JOIN huge_users u ON c.c_id = u.id JOIN huge_transactions t ON u.id = t.user_id
WHERE c.signup_day = 17 AND c.tier = 4;
SET enable_indexnlj = off;
EXPLAIN ANALYZE SELECT c.c_id, u.region, t.tx_id, t.amount
FROM customers c JOIN huge_users u ON c.c_id = u.id JOIN huge_transactions t ON u.id = t.user_id
WHERE c.signup_day = 17 AND c.tier = 4;
SET enable_indexnlj = on;

-- Skewed data: the MCV list knows 'DELIVERED' covers ~70% of orders; the histogram knows
-- orders pile up late in the year.
EXPLAIN ANALYZE SELECT COUNT(*) FROM orders WHERE status = 'DELIVERED';
EXPLAIN ANALYZE SELECT COUNT(*) FROM orders WHERE day > 300;

-- Hash aggregation + top-N (bounded heap, no full sort).
SELECT customer_id, COUNT(*) AS n, SUM(amount) AS revenue
FROM orders GROUP BY customer_id ORDER BY n DESC LIMIT 5;

-- Constant propagation: o.customer_id = 42 with c.c_id = o.customer_id also filters customers.
EXPLAIN SELECT c.region, o.amount FROM customers c JOIN orders o ON c.c_id = o.customer_id
WHERE o.customer_id = 42;
