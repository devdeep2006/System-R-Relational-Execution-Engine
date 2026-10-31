// Differential testing: random queries are executed by the engine under many optimizer
// configurations (each forcing different join algorithms, search spaces, estimators and
// memory budgets) and compared with a brute-force nested-loop reference evaluator.

#include <functional>
#include <map>

#include "common/rng.h"
#include "test_util.h"

using namespace sysr;
using namespace sysrtest;

namespace {

const char* kCols[] = {"a", "b", "c"};  // a: INT [0, A), b: INT [0, B), c: TEXT {x, y, z}

struct Spec {
  std::vector<int> tables;
  struct Join { int l, lc, r, rc; bool theta; };
  std::vector<Join> joins;
  struct Filter { int t, c; CmpOp op; Value v; std::vector<Value> in; };
  std::vector<Filter> filters;
  bool agg = false;
  int groupT = 0, sumT = 0;
  int orderT = -1;
};

using Data = std::vector<std::vector<Tuple>>;

void loadTables(Database& db, Data& data, const std::string& prefix, const std::vector<int>& sizes, int64_t aDomain,
                int64_t bDomain, uint64_t seed) {
  Rng rng(seed);
  static const char* texts[] = {"x", "y", "z"};
  for (size_t t = 0; t < sizes.size(); ++t) {
    std::string name = prefix + std::to_string(t);
    db.createTable(name, {{"a", TypeId::Int}, {"b", TypeId::Int}, {"c", TypeId::String}});
    std::vector<Tuple> rows;
    for (int i = 0; i < sizes[t]; ++i) {
      Tuple row{Value::Int(rng.range(0, aDomain - 1)), Value::Int(rng.range(0, bDomain - 1)), Value::String(texts[rng.uniform(3)])};
      db.insert(name, row);
      rows.push_back(row);
    }
    data.push_back(rows);
  }
}

std::string toSql(const Spec& s, const std::string& prefix) {
  auto col = [](int t, int c) { return "x" + std::to_string(t) + "." + kCols[c]; };
  std::string sql = "SELECT ";
  if (s.agg) sql += col(s.groupT, 0) + ", COUNT(*), SUM(" + col(s.sumT, 1) + ")";
  else sql += "*";
  sql += " FROM ";
  std::vector<std::string> where;
  std::vector<bool> used(s.joins.size(), false);
  for (size_t i = 0; i < s.tables.size(); ++i) {
    std::string ref = prefix + std::to_string(s.tables[i]) + " x" + std::to_string(i);
    // Introduce a table with JOIN ... ON when some equality links it to an earlier one.
    int on = -1;
    for (size_t j = 0; j < s.joins.size() && i > 0; ++j)
      if (!s.joins[j].theta && static_cast<size_t>(s.joins[j].r) == i && i % 2 == 1) { on = static_cast<int>(j); break; }
    if (i == 0) sql += ref;
    else if (on >= 0) {
      const auto& j = s.joins[static_cast<size_t>(on)];
      sql += " JOIN " + ref + " ON " + col(j.l, j.lc) + " = " + col(j.r, j.rc);
      used[static_cast<size_t>(on)] = true;
    } else sql += ", " + ref;
  }
  for (size_t j = 0; j < s.joins.size(); ++j) {
    if (used[j]) continue;
    const auto& jn = s.joins[j];
    where.push_back(jn.theta ? col(jn.l, 1) + " < " + col(jn.r, 1) : col(jn.l, jn.lc) + " = " + col(jn.r, jn.rc));
  }
  for (const auto& f : s.filters) {
    if (!f.in.empty()) {
      std::string list;
      for (size_t i = 0; i < f.in.size(); ++i) list += (i ? ", " : "") + std::string("'") + f.in[i].asString() + "'";
      where.push_back(col(f.t, f.c) + " IN (" + list + ")");
    } else {
      std::string v = f.v.type() == TypeId::String ? "'" + f.v.asString() + "'" : f.v.toString();
      where.push_back(col(f.t, f.c) + " " + cmpSymbol(f.op) + " " + v);
    }
  }
  for (size_t i = 0; i < where.size(); ++i) sql += (i ? " AND " : " WHERE ") + where[i];
  if (s.agg) sql += " GROUP BY " + col(s.groupT, 0);
  else if (s.orderT >= 0) sql += " ORDER BY " + col(s.orderT, 1);
  return sql;
}

bool cmpHolds(const Value& a, CmpOp op, const Value& b) {
  int c = a.compare(b);
  switch (op) {
    case CmpOp::Eq: return c == 0;
    case CmpOp::Ne: return c != 0;
    case CmpOp::Lt: return c < 0;
    case CmpOp::Le: return c <= 0;
    case CmpOp::Gt: return c > 0;
    case CmpOp::Ge: return c >= 0;
  }
  return false;
}

std::vector<Tuple> reference(const Spec& s, const Data& data) {
  const size_t n = s.tables.size();
  std::vector<const Tuple*> bound(n);
  std::vector<Tuple> out;
  std::map<std::string, std::pair<int64_t, int64_t>> groups;
  std::map<std::string, Value> groupKey;
  std::function<void(size_t)> rec = [&](size_t i) {
    if (i == n) {
      if (s.agg) {
        const Value& k = (*bound[static_cast<size_t>(s.groupT)])[0];
        auto& g = groups[k.toString()];
        groupKey[k.toString()] = k;
        g.first += 1;
        g.second += (*bound[static_cast<size_t>(s.sumT)])[1].asInt();
      } else {
        Tuple row;
        for (size_t t = 0; t < n; ++t) row.insert(row.end(), bound[t]->begin(), bound[t]->end());
        out.push_back(row);
      }
      return;
    }
    for (const Tuple& row : data[static_cast<size_t>(s.tables[i])]) {
      bound[i] = &row;
      bool ok = true;
      for (const auto& f : s.filters) {
        if (static_cast<size_t>(f.t) != i) continue;
        const Value& v = row[static_cast<size_t>(f.c)];
        if (!f.in.empty()) {
          bool any = false;
          for (const auto& x : f.in) any = any || v.compare(x) == 0;
          ok = ok && any;
        } else {
          ok = ok && cmpHolds(v, f.op, f.v);
        }
      }
      for (const auto& j : s.joins) {
        if (static_cast<size_t>(std::max(j.l, j.r)) != i) continue;
        const Tuple& L = *bound[static_cast<size_t>(j.l)];
        const Tuple& R = *bound[static_cast<size_t>(j.r)];
        ok = ok && (j.theta ? L[1].compare(R[1]) < 0 : L[static_cast<size_t>(j.lc)].compare(R[static_cast<size_t>(j.rc)]) == 0);
      }
      if (ok) rec(i + 1);
    }
  };
  rec(0);
  if (s.agg)
    for (const auto& g : groups) out.push_back({groupKey[g.first], Value::Int(g.second.first), Value::Int(g.second.second)});
  return out;
}

Spec randomSpec(Rng& rng, int numTables, int maxRels, int64_t aDomain, int64_t bDomain) {
  Spec s;
  int n = 1 + static_cast<int>(rng.uniform(static_cast<uint64_t>(maxRels)));
  for (int i = 0; i < n; ++i) s.tables.push_back(static_cast<int>(rng.uniform(static_cast<uint64_t>(numTables))));
  auto intCol = [&]() { return static_cast<int>(rng.uniform(2)); };
  for (int i = 1; i < n; ++i) {
    if (rng.unit() < 0.1) continue;  // leave disconnected: cross product
    int j = static_cast<int>(rng.uniform(static_cast<uint64_t>(i)));
    int c = intCol();
    s.joins.push_back({j, c, i, rng.unit() < 0.8 ? c : 1 - c, false});
    if (rng.unit() < 0.2) {
      int k = static_cast<int>(rng.uniform(static_cast<uint64_t>(i)));
      s.joins.push_back({k, rng.unit() < 0.5 ? 2 : intCol(), i, 0, false});
      if (s.joins.back().lc == 2) s.joins.back().rc = 2;
    }
    if (rng.unit() < 0.15) s.joins.push_back({j, 1, i, 1, true});
  }
  for (int i = 0; i < n; ++i) {
    if (rng.unit() > 0.5) continue;
    Spec::Filter f;
    f.t = i;
    double u = rng.unit();
    if (u < 0.25) {
      f.c = 2;
      f.op = CmpOp::Eq;
      f.v = Value::String(rng.unit() < 0.5 ? "x" : "z");
    } else if (u < 0.35) {
      f.c = 2;
      f.in = {Value::String("x"), Value::String("y")};
    } else {
      f.c = intCol();
      static const CmpOp ops[] = {CmpOp::Eq, CmpOp::Ne, CmpOp::Lt, CmpOp::Le, CmpOp::Gt, CmpOp::Ge};
      f.op = ops[rng.uniform(6)];
      f.v = Value::Int(rng.range(0, (f.c == 0 ? aDomain : bDomain) - 1));
    }
    s.filters.push_back(f);
  }
  if (rng.unit() < 0.3) {
    s.agg = true;
    s.groupT = static_cast<int>(rng.uniform(static_cast<uint64_t>(n)));
    s.sumT = static_cast<int>(rng.uniform(static_cast<uint64_t>(n)));
  } else if (rng.unit() < 0.4) {
    s.orderT = static_cast<int>(rng.uniform(static_cast<uint64_t>(n)));
  }
  return s;
}

using ConfigFn = std::function<void(OptimizerConfig&)>;

std::vector<std::pair<const char*, ConfigFn>> configs() {
  return {
      {"default", [](OptimizerConfig&) {}},
      {"bushy", [](OptimizerConfig& c) { c.bushy = true; }},
      {"hash-only", [](OptimizerConfig& c) { onlyJoin(c, "hash"); }},
      {"merge-only", [](OptimizerConfig& c) { onlyJoin(c, "merge"); c.bushy = true; }},
      {"nestloop-only", [](OptimizerConfig& c) { onlyJoin(c, "nestloop"); }},
      {"indexnlj-only", [](OptimizerConfig& c) { onlyJoin(c, "indexnlj"); }},
      {"magic-no-orders", [](OptimizerConfig& c) { c.estimator = EstimatorMode::Magic; c.interestingOrders = false; c.enableIndexScan = false; }},
      {"tiny-memory", [](OptimizerConfig& c) { c.cost.memPages = 4; c.cost.ioChunk = 1; c.bushy = true; }},
  };
}

void checkQuery(Database& db, const Spec& s, const Data& data, const std::string& prefix, const char* cfgName) {
  std::string sql = toSql(s, prefix);
  std::vector<Tuple> expect = reference(s, data);
  QueryResult r;
  try {
    r = db.execute(sql);
  } catch (const std::exception& e) {
    throw Failure{SYSR_WHERE, std::string(cfgName) + ": " + e.what() + "\n       " + sql};
  }
  if (canon(r.rows) != canon(expect))
    throw Failure{SYSR_WHERE, std::string(cfgName) + ": got " + std::to_string(r.rows.size()) + " rows, expected " +
                                  std::to_string(expect.size()) + "\n       " + sql};
  if (!s.agg && s.orderT >= 0) {
    size_t col = static_cast<size_t>(s.orderT) * 3 + 1;
    for (size_t i = 1; i < r.rows.size(); ++i)
      if (r.rows[i - 1][col].compare(r.rows[i][col]) > 0)
        throw Failure{SYSR_WHERE, std::string(cfgName) + ": ORDER BY violated\n       " + sql};
  }
}

}  // namespace

TEST(differential_random_multiway_queries) {
  Database db;
  Data data;
  loadTables(db, data, "r", {25, 30, 35, 40}, 6, 20, 2024);
  db.createIndex("r0", "a");
  db.createIndex("r2", "b");
  db.analyzeAll();
  Rng rng(42);
  int executed = 0;
  for (int q = 0; q < 400; ++q) {
    Spec s = randomSpec(rng, 4, 4, 6, 20);
    for (const auto& cfg : configs()) {
      db.config() = OptimizerConfig{};
      cfg.second(db.config());
      checkQuery(db, s, data, "r", cfg.first);
      ++executed;
    }
  }
  CHECK_EQ(executed, 400 * 8);
}

// Larger inputs with a 4-page memory budget force every spilling code path: Grace hash
// join (with recursion), multi-pass external sort for merge join, and BNLJ rescans.
TEST(differential_spilling_two_way_joins) {
  Database db;
  Data data;
  loadTables(db, data, "m", {1500, 1800}, 150, 1000, 7);
  db.analyzeAll();
  Rng rng(8);
  for (int q = 0; q < 30; ++q) {
    Spec s = randomSpec(rng, 2, 2, 150, 1000);
    if (s.tables.size() < 2 || s.joins.empty()) {  // avoid 2.7M-row cross products
      s.tables = {0, 1};
      s.joins = {{0, 0, 1, 0, false}};
      s.filters.clear();
      s.orderT = s.agg ? -1 : s.orderT;
      s.groupT = s.sumT = 0;
    }
    for (const char* algo : {"hash", "merge", "nestloop"}) {
      db.config() = OptimizerConfig{};
      onlyJoin(db.config(), algo);
      db.config().cost.memPages = 4;
      db.config().cost.ioChunk = 1;
      checkQuery(db, s, data, "m", algo);
    }
  }
}
