#include <chrono>

#include "common/rng.h"
#include "optimizer/optimizer.h"
#include "sql/parser.h"
#include "test_util.h"

using namespace sysr;
using namespace sysrtest;

namespace {

Database& optDb() {
  static Database* db = [] {
    auto* d = new Database();
    Rng rng(77);
    const TypeId I = TypeId::Int;
    for (int t = 0; t < 5; ++t) d->createTable("t" + std::to_string(t), {{"a", I}, {"b", I}, {"c", I}});
    for (int i = 0; i < 2000; ++i) d->insert("t0", {Value::Int(i), Value::Int(i % 50), Value::Int(rng.range(0, 999))});
    for (int i = 0; i < 300; ++i) d->insert("t1", {Value::Int(rng.range(0, 299)), Value::Int(i % 30), Value::Int(i)});
    for (int i = 0; i < 5000; ++i) d->insert("t2", {Value::Int(rng.range(0, 1999)), Value::Int(i), Value::Int(i % 100)});
    for (int i = 0; i < 50; ++i) d->insert("t3", {Value::Int(i), Value::Int(i % 5), Value::Int(i)});
    for (int i = 0; i < 1000; ++i) d->insert("t4", {Value::Int(rng.range(0, 999)), Value::Int(rng.range(0, 49)), Value::Int(i)});
    d->createIndex("t0", "a");
    d->createIndex("t2", "b");
    d->createIndex("t4", "a");
    d->analyzeAll();
    return d;
  }();
  return *db;
}

std::string randomQuery(Rng& rng, int n, bool allowOrder = true) {
  static const char* cols[] = {"a", "b", "c"};
  std::vector<int> tables;
  while (static_cast<int>(tables.size()) < n) {
    int t = static_cast<int>(rng.uniform(5));
    if (std::find(tables.begin(), tables.end(), t) == tables.end()) tables.push_back(t);
  }
  std::string from, where;
  auto addWhere = [&](const std::string& p) { where += (where.empty() ? "" : " AND ") + p; };
  for (int i = 0; i < n; ++i) {
    from += (i ? ", " : "") + std::string("t") + std::to_string(tables[static_cast<size_t>(i)]) + " x" + std::to_string(i);
    if (i > 0) {
      int j = static_cast<int>(rng.uniform(static_cast<uint64_t>(i)));
      addWhere("x" + std::to_string(i) + "." + cols[rng.uniform(3)] + " = x" + std::to_string(j) + "." + cols[rng.uniform(3)]);
      if (i > 1 && rng.unit() < 0.3) {
        int k = static_cast<int>(rng.uniform(static_cast<uint64_t>(i)));
        addWhere("x" + std::to_string(i) + "." + cols[rng.uniform(3)] + " = x" + std::to_string(k) + "." + cols[rng.uniform(3)]);
      }
    }
    if (rng.unit() < 0.4)
      addWhere("x" + std::to_string(i) + "." + cols[rng.uniform(3)] + (rng.unit() < 0.5 ? " < " : " = ") +
               std::to_string(rng.range(0, 500)));
  }
  std::string sql = "SELECT * FROM " + from + (where.empty() ? "" : " WHERE " + where);
  if (allowOrder && rng.unit() < 0.3)
    sql += " ORDER BY x" + std::to_string(rng.uniform(static_cast<uint64_t>(n))) + "." + cols[rng.uniform(3)];
  return sql;
}

PlanResult plan(Database& db, const std::string& sql, const OptimizerConfig& cfg, BoundQuery* keep = nullptr) {
  Statement st = parseStatement(sql);
  BoundQuery q = bindSelect(*st.select, db.catalog());
  PlanResult r = optimize(q, cfg);
  if (keep) *keep = std::move(q);
  return r;
}

bool rightChildrenAreBase(const PhysNode& p) {
  if (p.kind == PhysKind::HashJoin || p.kind == PhysKind::MergeJoin || p.kind == PhysKind::NestedLoop) {
    const PhysNode* r = p.kids[1].get();
    if (r->kind == PhysKind::Sort) r = r->kids[0].get();
    if (r->kind != PhysKind::SeqScan && r->kind != PhysKind::IndexScan) return false;
  }
  for (const auto& k : p.kids)
    if (!rightChildrenAreBase(*k)) return false;
  return true;
}

}  // namespace

// Dynamic programming with interesting orders must find exactly the cheapest plan that an
// exhaustive enumeration (no pruning at all) finds, for both search spaces.
TEST(dp_cost_equals_exhaustive_enumeration) {
  Database& db = optDb();
  Rng rng(1234);
  int checked = 0;
  for (int i = 0; i < 120; ++i) {
    int n = 2 + i % 3;
    std::string sql = randomQuery(rng, n);
    for (bool bushy : {false, true}) {
      OptimizerConfig cfg;
      cfg.bushy = bushy;
      double dp = plan(db, sql, cfg).root->totalMs;
      cfg.exhaustive = true;
      PlanResult ex = plan(db, sql, cfg);
      CHECK(ex.stats.plansKept >= 1);
      if (std::fabs(dp - ex.root->totalMs) > 1e-6 * std::max(1.0, dp))
        throw Failure{SYSR_WHERE, "DP " + std::to_string(dp) + " vs exhaustive " + std::to_string(ex.root->totalMs) + " for " + sql};
      ++checked;
    }
  }
  CHECK_EQ(checked, 240);
}

TEST(larger_search_spaces_never_cost_more) {
  Database& db = optDb();
  Rng rng(99);
  for (int i = 0; i < 150; ++i) {
    std::string sql = randomQuery(rng, 2 + i % 4);
    OptimizerConfig base;
    double leftDeep = plan(db, sql, base).root->totalMs;
    OptimizerConfig b = base;
    b.bushy = true;
    CHECK(plan(db, sql, b).root->totalMs <= leftDeep * (1 + 1e-9));
    OptimizerConfig noOrders = base;
    noOrders.interestingOrders = false;
    CHECK(leftDeep <= plan(db, sql, noOrders).root->totalMs * (1 + 1e-9));
  }
}

TEST(left_deep_mode_only_builds_left_deep_trees) {
  Database& db = optDb();
  Rng rng(5);
  for (int i = 0; i < 60; ++i) {
    OptimizerConfig cfg;
    PlanResult r = plan(db, randomQuery(rng, 3 + i % 3), cfg);
    CHECK(rightChildrenAreBase(*r.root));
  }
}

TEST(planner_handles_twelve_relation_chain_and_star) {
  Database& db = optDb();
  std::string chain = "SELECT * FROM t3 x0", star = "SELECT * FROM t3 x0";
  std::string chainWhere, starWhere;
  for (int i = 1; i < 12; ++i) {
    chain += ", t3 x" + std::to_string(i);
    star += ", t3 x" + std::to_string(i);
    chainWhere += std::string(i > 1 ? " AND " : "") + "x" + std::to_string(i - 1) + ".a = x" + std::to_string(i) + ".b";
    starWhere += std::string(i > 1 ? " AND " : "") + "x0.a = x" + std::to_string(i) + ".c";
  }
  for (bool bushy : {false, true}) {
    OptimizerConfig cfg;
    cfg.bushy = bushy;
    PlanResult c = plan(db, chain + " WHERE " + chainWhere, cfg);
    CHECK_EQ(c.stats.subsetsPlanned, size_t{78});  // connected subsets of a 12-chain: 12*13/2
    PlanResult s = plan(db, star + " WHERE " + starWhere, cfg);
    CHECK(s.root != nullptr);
  }
}

TEST(cross_products_only_when_graph_is_disconnected) {
  Database& db = optDb();
  OptimizerConfig cfg;
  BoundQuery q;
  PlanResult r = plan(db, "SELECT * FROM t3 x, t1 y", cfg, &q);
  CHECK(r.root != nullptr);
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM t3 x, t1 y"), int64_t{50 * 300});
  // Connected: no subset that needs a cross product is ever planned.
  PlanResult c = plan(db, "SELECT * FROM t3 x, t1 y, t4 z WHERE x.a = y.a AND y.b = z.b", cfg);
  CHECK_EQ(c.stats.subsetsPlanned, size_t{6});  // {x},{y},{z},{x,y},{y,z},{x,y,z}
}
