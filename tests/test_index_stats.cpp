#include <map>

#include "catalog/statistics.h"
#include "common/rng.h"
#include "index/bplus_tree.h"
#include "optimizer/cardinality.h"
#include "sql/parser.h"
#include "test_util.h"

using namespace sysr;
using namespace sysrtest;

TEST(bplus_tree_matches_multimap_with_duplicates) {
  DiskManager disk;
  BufferPool pool(&disk, 32);  // far smaller than the tree: exercises eviction
  BPlusTree tree(&pool);
  std::multimap<int64_t, uint64_t> ref;
  Rng rng(1);
  for (int i = 0; i < 60000; ++i) {
    int64_t key = rng.range(-5000, 5000);
    RID rid{static_cast<page_id_t>(i / 100), static_cast<uint16_t>(i % 100)};
    tree.insert(key, rid);
    ref.emplace(key, rid.pack());
  }
  CHECK_EQ(tree.size(), uint64_t{60000});
  CHECK(tree.height() >= 3);
  for (int q = 0; q < 300; ++q) {
    int64_t lo = rng.range(-5200, 5200), hi = lo + rng.range(0, 400);
    std::vector<std::pair<int64_t, uint64_t>> expect;
    for (auto it = ref.lower_bound(lo); it != ref.end() && it->first <= hi; ++it) expect.push_back(*it);
    std::sort(expect.begin(), expect.end());
    std::vector<std::pair<int64_t, uint64_t>> got;
    auto c = tree.range(lo, hi);
    int64_t k;
    RID rid;
    while (c.next(k, rid)) got.emplace_back(k, rid.pack());
    CHECK(got == expect);
  }
  CHECK_EQ(pool.pinnedFrames(), size_t{0});
}

TEST(bplus_tree_ascending_inserts_and_point_lookups) {
  DiskManager disk;
  BufferPool pool(&disk, 64);
  BPlusTree tree(&pool);
  for (int i = 0; i < 100000; ++i) tree.insert(i, RID{static_cast<page_id_t>(i), 0});
  CHECK_EQ(tree.height(), 3);
  Rng rng(9);
  for (int q = 0; q < 1000; ++q) {
    int64_t key = rng.range(0, 99999), k;
    RID rid;
    auto c = tree.range(key, key);
    CHECK(c.next(k, rid));
    CHECK_EQ(k, key);
    CHECK_EQ(rid.page, static_cast<page_id_t>(key));
    CHECK(!c.next(k, rid));
  }
  int64_t k;
  RID rid;
  auto miss = tree.range(100000, 200000);
  CHECK(!miss.next(k, rid));
}

TEST(hyperloglog_relative_error) {
  for (uint64_t n : {1000ULL, 50000ULL, 400000ULL}) {
    HyperLogLog h;
    for (uint64_t i = 0; i < n; ++i) h.add(Value::Int(static_cast<int64_t>(i)).hash());
    double err = std::fabs(h.estimate() - static_cast<double>(n)) / static_cast<double>(n);
    CHECK(err < 0.04);
  }
}

TEST(space_saving_finds_zipf_heavy_hitters) {
  Rng rng(3);
  Zipf z(1000, 1.2);
  SpaceSaving ss(64);
  for (int i = 0; i < 100000; ++i) ss.add(Value::Int(static_cast<int64_t>(z.sample(rng))));
  auto top = ss.top();
  for (int64_t r = 0; r < 5; ++r) CHECK_EQ(top[static_cast<size_t>(r)].value.asInt(), r);
}

namespace {

double estimateRows(Database& db, const std::string& sql, EstimatorMode mode) {
  Statement st = parseStatement(sql);
  BoundQuery q = bindSelect(*st.select, db.catalog());
  CardinalityEstimator est(q, mode);
  return est.rows((1u << q.rels.size()) - 1);
}

}  // namespace

TEST(analyze_statistics_and_histogram_estimates) {
  Database db;
  db.createTable("t", {{"id", TypeId::Int}, {"x", TypeId::Int}, {"s", TypeId::String}});
  Rng rng(5);
  for (int i = 0; i < 20000; ++i)
    db.insert("t", {Value::Int(i), Value::Int(rng.range(0, 999)), Value::String(rng.unit() < 0.5 ? "hot" : "c" + std::to_string(rng.range(0, 99)))});
  db.analyzeAll();
  const TableStats& ts = db.catalog().require("t").stats;
  CHECK_NEAR(ts.cols[0].correlation, 1.0, 1e-12);  // id inserted in order
  CHECK(ts.cols[1].correlation < 0.6);
  CHECK_NEAR(ts.cols[1].ndv, 1000, 40);
  CHECK(!ts.cols[2].mcv.empty());
  CHECK_EQ(ts.cols[2].mcv[0].first.asString(), std::string("hot"));
  CHECK_NEAR(ts.cols[2].mcv[0].second, 0.5, 0.02);

  double actual = static_cast<double>(scalar(db, "SELECT COUNT(*) FROM t WHERE x < 250"));
  double est = estimateRows(db, "SELECT * FROM t WHERE x < 250", EstimatorMode::Histogram);
  CHECK(std::fabs(est - actual) / actual < 0.05);
  // Range predicates on one column are merged before estimation.
  actual = static_cast<double>(scalar(db, "SELECT COUNT(*) FROM t WHERE x > 100 AND x <= 300"));
  est = estimateRows(db, "SELECT * FROM t WHERE x > 100 AND x <= 300", EstimatorMode::Histogram);
  CHECK(std::fabs(est - actual) / actual < 0.08);
  actual = static_cast<double>(scalar(db, "SELECT COUNT(*) FROM t WHERE s = 'hot'"));
  est = estimateRows(db, "SELECT * FROM t WHERE s = 'hot'", EstimatorMode::Histogram);
  CHECK(std::fabs(est - actual) / actual < 0.05);
}

TEST(histograms_beat_uniform_assumption_on_skew) {
  Database db;
  db.createTable("t", {{"v", TypeId::Int}});
  Rng rng(11);
  for (int i = 0; i < 50000; ++i) {
    double u = rng.unit();
    db.insert("t", {Value::Int(static_cast<int64_t>(1000.0 * u * u * u))});  // mass piled near 0
  }
  db.analyzeAll();
  const char* q = "SELECT * FROM t WHERE v < 100";
  double actual = static_cast<double>(scalar(db, "SELECT COUNT(*) FROM t WHERE v < 100"));
  auto qerr = [&](double e) { return std::max(e / actual, actual / e); };
  double hist = qerr(estimateRows(db, q, EstimatorMode::Histogram));
  double uni = qerr(estimateRows(db, q, EstimatorMode::Uniform));
  CHECK(hist < 1.1);
  CHECK(uni > 3.0);
}
