// Benchmarks for the README. Prints Markdown tables to stdout.
//   sysr_bench            all sections
//   sysr_bench joins      one section: joins | costmodel | orders | estimator | scaling | bushy

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "common/rng.h"
#include "engine/database.h"
#include "engine/datagen.h"
#include "optimizer/cardinality.h"
#include "optimizer/optimizer.h"

using namespace sysr;

namespace {

std::string fmt(double v, int digits = 1) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  return buf;
}

double ioMs(const IOStats& io, const CostParams& p) {
  return static_cast<double>(io.seeks) * p.seekMs + static_cast<double>(io.transfers()) * p.transferMs;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
  return v[std::min(i, v.size() - 1)];
}

void resetConfig(Database& db) {
  db.config() = OptimizerConfig{};
  db.config().cost.bufferFrames = db.pool().frames();
}

void onlyJoin(OptimizerConfig& c, const std::string& which) {
  c.enableHashJoin = which == "hash";
  c.enableMergeJoin = which == "merge";
  c.enableNestedLoop = which == "nestloop";
  c.enableIndexNLJ = which == "indexnlj";
}

std::string firstJoin(const std::string& plan) {
  for (const char* name : {"HashJoin", "MergeJoin", "BlockNestedLoopJoin", "IndexNestedLoopJoin"})
    if (plan.find(name) != std::string::npos) return name;
  return "-";
}

const char* kJoin = "SELECT u.region, t.amount FROM huge_users u JOIN huge_transactions t ON u.id = t.user_id";
const char* kJoinOrdered =
    "SELECT u.id, t.amount FROM huge_users u JOIN huge_transactions t ON u.id = t.user_id ORDER BY u.id";

// ---------------------------------------------------------------------------
void benchJoins(Database& db) {
  std::cout << "\n## Join algorithms: huge_users (100K) x huge_transactions (500K), M = 64 pages\n\n";
  std::cout << "| Algorithm | Estimated cost (ms) | Measured transfers | Measured seeks | Simulated I/O (ms) | Wall time (ms) | Rows |\n";
  std::cout << "|---|---:|---:|---:|---:|---:|---:|\n";
  for (const char* algo : {"optimizer", "hash", "merge", "nestloop", "indexnlj"}) {
    resetConfig(db);
    if (std::string(algo) != "optimizer") onlyJoin(db.config(), algo);
    QueryResult r = db.execute(std::string("EXPLAIN ANALYZE ") + kJoin);
    std::string label = std::string(algo) == "optimizer" ? "Optimizer's choice (" + firstJoin(r.plan) + ")" : firstJoin(r.plan);
    std::cout << "| " << label << " | " << fmt(r.estimatedMs, 0) << " | " << r.io.transfers() << " | " << r.io.seeks << " | "
              << fmt(ioMs(r.io, db.config().cost), 0) << " | " << fmt(r.executionMs, 0) << " | " << r.rows.size() << " |\n";
  }
}

// ---------------------------------------------------------------------------
void benchCostModel(Database& db) {
  struct Case {
    const char* name;
    std::string sql;
    std::function<void(OptimizerConfig&)> cfg;
  };
  std::vector<Case> cases = {
      {"Grace hash join (100K x 500K)", kJoin, [](OptimizerConfig& c) { onlyJoin(c, "hash"); }},
      {"Grace hash join, M = 16", kJoin, [](OptimizerConfig& c) { onlyJoin(c, "hash"); c.cost.memPages = 16; }},
      {"Merge join + external sort", kJoinOrdered, [](OptimizerConfig&) {}},
      {"Merge join, both sides sorted, M = 32", kJoin, [](OptimizerConfig& c) { onlyJoin(c, "merge"); c.cost.memPages = 32; }},
      {"Block nested loop (spilled inner)", kJoin, [](OptimizerConfig& c) { onlyJoin(c, "nestloop"); }},
      {"External sort of 500K rows", "SELECT tx_id, amount FROM huge_transactions ORDER BY amount", [](OptimizerConfig&) {}},
      {"External sort, M = 16", "SELECT tx_id, amount FROM huge_transactions ORDER BY amount", [](OptimizerConfig& c) { c.cost.memPages = 16; }},
      {"Hash join orders x customers", "SELECT c.region, o.amount FROM orders o JOIN customers c ON o.customer_id = c.c_id",
       [](OptimizerConfig&) {}},
      {"Clustered index range scan", "SELECT * FROM huge_users WHERE id BETWEEN 20000 AND 30000", [](OptimizerConfig&) {}},
      {"Sequential scan + aggregate", "SELECT status, COUNT(*), AVG(amount) FROM orders GROUP BY status", [](OptimizerConfig&) {}},
  };
  std::cout << "\n## Cost model vs measured disk I/O (cold buffer pool)\n\n";
  std::cout << "| Plan | Est. transfers | Measured | Error | Est. seeks | Measured | Error |\n";
  std::cout << "|---|---:|---:|---:|---:|---:|---:|\n";
  std::vector<double> tErr, sErr;
  for (const auto& c : cases) {
    resetConfig(db);
    c.cfg(db.config());
    QueryResult r = db.execute("EXPLAIN ANALYZE " + c.sql);
    double mt = static_cast<double>(r.io.transfers()), ms = static_cast<double>(r.io.seeks);
    double et = r.estimated.transfers, es = r.estimated.seeks;
    double te = mt > 0 ? (et - mt) / mt * 100 : 0, se = ms > 0 ? (es - ms) / ms * 100 : 0;
    tErr.push_back(std::fabs(te));
    sErr.push_back(std::fabs(se));
    std::cout << "| " << c.name << " | " << fmt(et, 0) << " | " << r.io.transfers() << " | " << fmt(te, 1) << "% | "
              << fmt(es, 0) << " | " << r.io.seeks << " | " << fmt(se, 1) << "% |\n";
  }
  std::cout << "\nMedian absolute error: transfers " << fmt(percentile(tErr, 0.5), 1) << "%, seeks "
            << fmt(percentile(sErr, 0.5), 1) << "%. Max: transfers " << fmt(percentile(tErr, 1.0), 1) << "%, seeks "
            << fmt(percentile(sErr, 1.0), 1) << "%.\n";
}

// ---------------------------------------------------------------------------
void benchOrders(Database& db) {
  std::cout << "\n## Interesting orders: join + ORDER BY on the join key\n\n";
  std::cout << "| Interesting orders | Plan | Estimated cost (ms) | Simulated I/O (ms) | Wall time (ms) |\n";
  std::cout << "|---|---|---:|---:|---:|\n";
  double est[2] = {0, 0}, io[2] = {0, 0};
  for (int on = 1; on >= 0; --on) {
    resetConfig(db);
    db.config().interestingOrders = on;
    QueryResult r = db.execute(std::string("EXPLAIN ANALYZE ") + kJoinOrdered);
    std::string plan = firstJoin(r.plan) + (r.plan.find("Sort by u.id") != std::string::npos ? " + final sort" : "");
    est[on] = r.estimatedMs;
    io[on] = ioMs(r.io, db.config().cost);
    std::cout << "| " << (on ? "on" : "off") << " | " << plan << " | " << fmt(r.estimatedMs, 0) << " | " << fmt(io[on], 0)
              << " | " << fmt(r.executionMs, 0) << " |\n";
  }
  std::cout << "\nEstimated cost ratio " << fmt(est[0] / est[1], 2) << "x, measured simulated I/O ratio "
            << fmt(io[0] / io[1], 2) << "x.\n";
}

// ---------------------------------------------------------------------------
void benchEstimator(Database& db) {
  std::vector<std::string> preds = {
      "o.status = 'DELIVERED'", "o.status = 'RETURNED'", "o.status = 'CANCELLED'", "o.status != 'DELIVERED'",
      "o.day > 300", "o.day > 350", "o.day < 100", "o.day BETWEEN 200 AND 250", "o.day BETWEEN 360 AND 365",
      "o.amount < 10", "o.amount > 100", "o.amount > 500", "o.amount BETWEEN 20 AND 40",
      "o.customer_id = 1", "o.customer_id = 2", "o.customer_id = 500", "o.customer_id < 10", "o.customer_id > 5000",
      "o.status = 'SHIPPED' AND o.day > 300", "o.status = 'DELIVERED' AND o.amount > 100",
      "o.customer_id < 100 AND o.day > 200", "o.status IN ('RETURNED', 'CANCELLED')"};
  std::vector<std::string> joinPreds = {"c.region = 'north'", "c.region = 'fjord'", "c.tier = 1", "c.tier = 4",
                                        "c.c_id < 50", "c.signup_day > 900"};
  std::vector<std::string> queries;
  for (const auto& p : preds) queries.push_back("SELECT COUNT(*) FROM orders o WHERE " + p);
  for (const auto& jp : joinPreds) {
    queries.push_back("SELECT COUNT(*) FROM orders o JOIN customers c ON o.customer_id = c.c_id WHERE " + jp);
    queries.push_back("SELECT COUNT(*) FROM orders o JOIN customers c ON o.customer_id = c.c_id WHERE " + jp +
                      " AND o.status = 'DELIVERED'");
  }
  std::vector<double> actual;
  resetConfig(db);
  for (const auto& q : queries) actual.push_back(static_cast<double>(db.execute(q).rows[0][0].asInt()));

  std::cout << "\n## Cardinality estimation on skewed data (" << queries.size()
            << " queries on orders/customers: Zipf customer ids, skewed status/day, log-normal amounts)\n\n";
  std::cout << "| Estimator | Median q-error | 90th percentile | Max |\n|---|---:|---:|---:|\n";
  for (EstimatorMode m : {EstimatorMode::Magic, EstimatorMode::Uniform, EstimatorMode::Histogram}) {
    std::vector<double> qerr;
    for (size_t i = 0; i < queries.size(); ++i) {
      Statement st = parseStatement(queries[i]);
      BoundQuery q = bindSelect(*st.select, db.catalog());
      CardinalityEstimator est(q, m);
      double e = est.rows((1u << q.rels.size()) - 1);
      double a = std::max(1.0, actual[i]);
      qerr.push_back(std::max(e / a, a / e));
      if (std::getenv("SYSR_VERBOSE"))
        std::cerr << estimatorName(m) << "  est=" << fmt(e, 0) << " actual=" << a << " q=" << fmt(qerr.back(), 2) << "  "
                  << queries[i] << "\n";
    }
    const char* name = m == EstimatorMode::Magic ? "System R constants (1/10, 1/3)"
                     : m == EstimatorMode::Uniform ? "Uniform min/max + 1/NDV"
                                                   : "MCV + equi-depth histogram";
    std::cout << "| " << name << " | " << fmt(percentile(qerr, 0.5), 2) << " | " << fmt(percentile(qerr, 0.9), 2) << " | "
              << fmt(percentile(qerr, 1.0), 2) << " |\n";
  }
}

// ---------------------------------------------------------------------------
// Self-joins of wide(k0..k11). Each predicate uses its own pair of columns so equivalence
// classes never merge and the join graph keeps the intended shape.
std::string graphQuery(const std::string& shape, int n) {
  std::string sql = "SELECT * FROM wide x0";
  for (int i = 1; i < n; ++i) sql += ", wide x" + std::to_string(i);
  std::string where;
  auto add = [&](int a, int b) {
    where += (where.empty() ? " WHERE " : " AND ") + std::string("x") + std::to_string(a) + ".k" + std::to_string(b) +
             " = x" + std::to_string(b) + ".k" + std::to_string(a);
  };
  for (int i = 1; i < n; ++i) {
    if (shape == "chain") add(i - 1, i);
    else if (shape == "star") add(0, i);
    else
      for (int j = 0; j < i; ++j) add(j, i);
  }
  return sql + where;
}

void benchScaling() {
  Database db;
  std::vector<Column> cols;
  for (int k = 0; k < 12; ++k) cols.push_back({"k" + std::to_string(k), TypeId::Int});
  db.createTable("wide", cols);
  Rng rng(12);
  for (int i = 0; i < 2000; ++i) {
    Tuple row;
    for (int k = 0; k < 12; ++k) row.push_back(Value::Int(rng.range(0, 50 * (k + 1))));
    db.insert("wide", row);
  }
  db.analyzeAll();
  std::cout << "\n## Optimizer scalability (planning only)\n\n";
  std::cout << "| Join graph | Relations | Left-deep: plans costed | Left-deep: ms | Bushy: plans costed | Bushy: ms |\n";
  std::cout << "|---|---:|---:|---:|---:|---:|\n";
  for (const char* shape : {"chain", "star", "clique"}) {
    for (int n : {4, 6, 8, 10, 12}) {
      if (std::string(shape) == "clique" && n > 10) continue;
      Statement st = parseStatement(graphQuery(shape, n));
      BoundQuery q = bindSelect(*st.select, db.catalog());
      std::cout << "| " << shape << " | " << n;
      for (bool bushy : {false, true}) {
        OptimizerConfig cfg;
        cfg.bushy = bushy;
        PlanResult r = optimize(q, cfg);
        std::cout << " | " << r.stats.plansConsidered << " | " << fmt(r.stats.planningMs, 1);
      }
      std::cout << " |\n";
    }
  }
}

// ---------------------------------------------------------------------------
void benchBushy() {
  Database db;
  Rng rng(2026);
  const int sizes[] = {50, 200, 1000, 5000, 20000, 80000};
  for (int t = 0; t < 6; ++t) {
    std::string name = "b" + std::to_string(t);
    db.createTable(name, {{"k1", TypeId::Int}, {"k2", TypeId::Int}, {"v", TypeId::Int}});
    for (int i = 0; i < sizes[t]; ++i)
      db.insert(name, {Value::Int(rng.range(0, sizes[t] / 2)), Value::Int(rng.range(0, 999)), Value::Int(rng.range(0, 99))});
  }
  db.analyzeAll();
  int better = 0, total = 0;
  std::vector<double> gains;
  double best = 1;
  for (int q = 0; q < 300; ++q) {
    int n = 4 + q % 3;
    std::vector<int> pick = {0, 1, 2, 3, 4, 5};
    for (int i = 5; i > 0; --i) std::swap(pick[static_cast<size_t>(i)], pick[rng.uniform(static_cast<uint64_t>(i + 1))]);
    std::string sql = "SELECT * FROM ";
    std::string where;
    for (int i = 0; i < n; ++i) {
      sql += (i ? ", b" : "b") + std::to_string(pick[static_cast<size_t>(i)]) + " x" + std::to_string(i);
      if (i > 0) {
        int j = static_cast<int>(rng.uniform(static_cast<uint64_t>(i)));
        const char* col = rng.unit() < 0.5 ? "k1" : "k2";
        where += (where.empty() ? " WHERE " : " AND ") + std::string("x") + std::to_string(j) + "." + col + " = x" +
                 std::to_string(i) + "." + col;
      }
      if (rng.unit() < 0.5)
        where += (where.empty() ? " WHERE " : " AND ") + std::string("x") + std::to_string(i) + ".v < " +
                 std::to_string(rng.range(1, 60));
    }
    Statement st = parseStatement(sql + where);
    BoundQuery bq = bindSelect(*st.select, db.catalog());
    OptimizerConfig cfg;
    double ld = optimize(bq, cfg).root->totalMs;
    cfg.bushy = true;
    double bu = optimize(bq, cfg).root->totalMs;
    ++total;
    if (bu < ld * 0.999) {
      ++better;
      gains.push_back(ld / bu);
      best = std::max(best, ld / bu);
    }
  }
  std::cout << "\n## Bushy vs left-deep plan quality (300 random 4-6 way joins, table sizes 50 to 80K)\n\n";
  std::cout << "Bushy search found a cheaper plan for " << better << " of " << total << " queries ("
            << fmt(100.0 * better / total, 1) << "%); median improvement on those " << fmt(percentile(gains, 0.5), 2)
            << "x, best " << fmt(best, 2) << "x.\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string only = argc > 1 ? argv[1] : "";
  auto want = [&](const char* s) { return only.empty() || only == s; };
  Database db;
  auto t0 = std::chrono::steady_clock::now();
  loadDemoData(db);
  std::cerr << "demo data loaded in "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s\n";
  std::cout << "# Benchmark results\n\nPage size 4 KB, buffer pool " << db.pool().frames()
            << " frames, t_S = 10 ms per seek, t_T = 0.1 ms per block. Simulated I/O = seeks x t_S + transfers x t_T.\n";
  if (want("joins")) benchJoins(db);
  if (want("costmodel")) benchCostModel(db);
  if (want("orders")) benchOrders(db);
  if (want("estimator")) benchEstimator(db);
  if (want("scaling")) benchScaling();
  if (want("bushy")) benchBushy();
  return 0;
}
