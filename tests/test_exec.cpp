#include <map>

#include "common/rng.h"
#include "test_util.h"

using namespace sysr;
using namespace sysrtest;

namespace {

// a(k, x, pad) and b(k, y, pad) with Zipf-skewed join keys so some keys are very frequent.
void makePair(Database& db, int na, int nb, int keys, uint64_t seed) {
  db.createTable("a", {{"k", TypeId::Int}, {"x", TypeId::Int}, {"pad", TypeId::String}});
  db.createTable("b", {{"k", TypeId::Int}, {"y", TypeId::Int}, {"pad", TypeId::String}});
  Rng rng(seed);
  Zipf z(static_cast<size_t>(keys), 0.8);
  std::string pad(40, 'p');
  for (int i = 0; i < na; ++i) db.insert("a", {Value::Int(static_cast<int64_t>(z.sample(rng))), Value::Int(i), Value::String(pad)});
  for (int i = 0; i < nb; ++i) db.insert("b", {Value::Int(static_cast<int64_t>(z.sample(rng))), Value::Int(i), Value::String(pad)});
  db.analyzeAll();
}

int64_t expectedEquiJoin(Database& db) {
  std::map<int64_t, int64_t> ca, cb;
  for (const auto& r : db.execute("SELECT k FROM a").rows) ++ca[r[0].asInt()];
  for (const auto& r : db.execute("SELECT k FROM b").rows) ++cb[r[0].asInt()];
  int64_t n = 0;
  for (const auto& kv : ca) n += kv.second * (cb.count(kv.first) ? cb[kv.first] : 0);
  return n;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

TEST(external_sort_multi_pass_is_correct_and_stable) {
  Database db;
  db.createTable("t", {{"k", TypeId::Int}, {"seq", TypeId::Int}, {"pad", TypeId::String}});
  Rng rng(2);
  for (int i = 0; i < 20000; ++i) db.insert("t", {Value::Int(rng.range(0, 499)), Value::Int(i), Value::String(std::string(40, 'x'))});
  db.config().cost.memPages = 4;
  db.config().cost.ioChunk = 1;  // fan-in 3: several merge passes
  QueryResult r = db.execute("SELECT k, seq FROM t ORDER BY k");
  CHECK_EQ(r.rows.size(), size_t{20000});
  for (size_t i = 1; i < r.rows.size(); ++i) {
    int64_t k0 = r.rows[i - 1][0].asInt(), k1 = r.rows[i][0].asInt();
    CHECK(k0 <= k1);
    if (k0 == k1) CHECK(r.rows[i - 1][1].asInt() < r.rows[i][1].asInt());  // stable
  }
  QueryResult ex = db.execute("EXPLAIN ANALYZE SELECT k, seq FROM t ORDER BY k DESC");
  CHECK(contains(ex.message, "external"));
}

TEST(external_sort_measured_io_matches_cost_model) {
  Database db;
  db.createTable("t", {{"k", TypeId::Int}, {"pad", TypeId::String}});
  Rng rng(4);
  for (int i = 0; i < 60000; ++i) db.insert("t", {Value::Int(rng.range(0, 1000000)), Value::String(std::string(30, 'y'))});
  db.config().cost.memPages = 16;
  db.config().cost.ioChunk = 4;
  QueryResult r = db.execute("EXPLAIN ANALYZE SELECT k, pad FROM t ORDER BY k");
  double est = r.estimated.transfers, got = static_cast<double>(r.io.transfers());
  CHECK(std::fabs(est - got) / got < 0.03);
}

TEST(grace_hash_join_recursive_partitioning) {
  Database db;
  makePair(db, 6000, 9000, 300, 21);
  int64_t expect = expectedEquiJoin(db);
  onlyJoin(db.config(), "hash");
  db.config().cost.memPages = 4;  // 2-page hash table: every partition must recurse
  QueryResult r = db.execute("EXPLAIN ANALYZE SELECT a.x, b.y FROM a JOIN b ON a.k = b.k");
  CHECK(contains(r.message, "grace"));
  CHECK(contains(r.message, "levels"));
  CHECK_EQ(static_cast<int64_t>(r.rows.size()), expect);
  // Same answer in memory.
  db.config().cost.memPages = 2000;
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM a JOIN b ON a.k = b.k"), expect);
}

TEST(merge_join_many_to_many_groups) {
  Database db;
  makePair(db, 3000, 4000, 50, 8);
  int64_t expect = expectedEquiJoin(db);
  onlyJoin(db.config(), "merge");
  db.config().cost.memPages = 8;
  QueryResult r = db.execute("EXPLAIN ANALYZE SELECT a.x, b.y FROM a JOIN b ON a.k = b.k");
  CHECK(contains(r.message, "MergeJoin"));
  CHECK_EQ(static_cast<int64_t>(r.rows.size()), expect);
}

TEST(block_nested_loop_spills_inner_and_rescans) {
  Database db;
  makePair(db, 3000, 3000, 400, 13);
  int64_t expect = expectedEquiJoin(db);
  onlyJoin(db.config(), "nestloop");
  db.config().cost.memPages = 6;
  QueryResult r = db.execute("EXPLAIN ANALYZE SELECT a.x, b.y FROM a JOIN b ON a.k = b.k");
  CHECK(contains(r.message, "outer blocks"));
  CHECK_EQ(static_cast<int64_t>(r.rows.size()), expect);
  // Theta join (no equality): every pair is tested.
  int64_t theta = 0;
  std::vector<int64_t> xs, ys;
  for (const auto& t : db.execute("SELECT x FROM a WHERE x < 300").rows) xs.push_back(t[0].asInt());
  for (const auto& t : db.execute("SELECT y FROM b WHERE y < 200").rows) ys.push_back(t[0].asInt());
  for (int64_t x : xs)
    for (int64_t y : ys) theta += (x + 50 < y);
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM a, b WHERE a.x < 300 AND b.y < 200 AND a.x + 50 < b.y"), theta);
}

TEST(index_nested_loop_join_and_index_scan) {
  Database db;
  makePair(db, 2000, 5000, 1000, 17);
  db.createIndex("b", "k");
  int64_t expect = expectedEquiJoin(db);
  onlyJoin(db.config(), "indexnlj");
  QueryResult r = db.execute("EXPLAIN ANALYZE SELECT a.x, b.y FROM a JOIN b ON a.k = b.k");
  CHECK(contains(r.message, "IndexNestedLoopJoin"));
  CHECK_EQ(static_cast<int64_t>(r.rows.size()), expect);
  // A narrow range over a physically ordered key uses the clustered index instead of a scan.
  db.createTable("c", {{"id", TypeId::Int}, {"pad", TypeId::String}});
  for (int i = 0; i < 50000; ++i) db.insert("c", {Value::Int(i), Value::String(std::string(40, 'c'))});
  db.createIndex("c", "id");
  QueryResult s = db.execute("EXPLAIN ANALYZE SELECT * FROM c WHERE id BETWEEN 100 AND 200");
  if (!contains(s.message, "IndexScan")) throw Failure{SYSR_WHERE, "expected IndexScan:\n" + s.message};
  CHECK_EQ(s.rows.size(), size_t{101});
  CHECK(s.io.transfers() < 10);
}

TEST(top_n_matches_full_sort) {
  Database db;
  makePair(db, 5000, 10, 100, 23);
  QueryResult full = db.execute("SELECT x, k FROM a ORDER BY k DESC, x");
  QueryResult top = db.execute("SELECT x, k FROM a ORDER BY k DESC, x LIMIT 7");
  CHECK_EQ(top.rows.size(), size_t{7});
  for (size_t i = 0; i < 7; ++i) CHECK_EQ(rowKey(top.rows[i]), rowKey(full.rows[i]));
  CHECK_EQ(db.execute("SELECT x FROM a ORDER BY x LIMIT 0").rows.size(), size_t{0});
  CHECK_EQ(db.execute("SELECT x FROM a LIMIT 3").rows.size(), size_t{3});
}

TEST(no_operator_leaks_pins_or_spill_pages) {
  Database db;
  makePair(db, 4000, 4000, 200, 31);
  size_t before = db.disk().livePages();
  db.config().cost.memPages = 4;
  for (const char* algo : {"hash", "merge", "nestloop"}) {
    onlyJoin(db.config(), algo);
    db.execute("SELECT a.x, b.y FROM a JOIN b ON a.k = b.k ORDER BY b.y");
    CHECK_EQ(db.pool().pinnedFrames(), size_t{0});
    CHECK_EQ(db.disk().livePages(), before);
  }
}
