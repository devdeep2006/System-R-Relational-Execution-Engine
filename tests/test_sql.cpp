#include "engine/datagen.h"
#include "planner/binder.h"
#include "test_util.h"

using namespace sysr;
using namespace sysrtest;

TEST(parser_accepts_full_select_grammar) {
  Statement st = parseStatement(
      "select a.x as y, count(*), sum(u.z * 2) from t a join u on a.x = u.y, v cross join w "
      "where a.x between 1 and 5 and u.z in (1, 2, 3) or not v.w <> 'it''s' -- comment\n"
      "group by a.x order by 2 desc, y limit 10;");
  CHECK(st.kind == Statement::Kind::Select);
  const SelectStmt& s = *st.select;
  CHECK_EQ(s.items.size(), size_t{3});
  CHECK_EQ(s.items[0].alias, std::string("y"));
  CHECK_EQ(s.from.size(), size_t{4});
  CHECK_EQ(s.from[0].alias, std::string("a"));
  CHECK_EQ(s.joinConditions.size(), size_t{1});
  CHECK(s.where != nullptr);
  CHECK_EQ(s.groupBy.size(), size_t{1});
  CHECK_EQ(s.orderBy.size(), size_t{2});
  CHECK(s.orderBy[0].desc);
  CHECK_EQ(s.limit, int64_t{10});
}

TEST(parser_other_statements_and_errors) {
  auto stmts = parseScript(
      "CREATE TABLE t (a INT, b DOUBLE, c VARCHAR(20));"
      "INSERT INTO t VALUES (1, 2.5, 'x'), (-2, -1e3, 'y');"
      "CREATE INDEX t_a ON t (a); ANALYZE t; SET bushy = on; SHOW TABLES; EXPLAIN ANALYZE SELECT * FROM t;");
  CHECK_EQ(stmts.size(), size_t{7});
  CHECK(stmts[1].rows.size() == 2);
  CHECK(stmts[6].kind == Statement::Kind::Explain && stmts[6].explainAnalyze);
  CHECK_THROWS(parseStatement("SELECT FROM t"));
  CHECK_THROWS(parseStatement("SELECT * FROM a LEFT JOIN b ON a.x = b.y"));
  CHECK_THROWS(parseStatement("SELECT * FROM t WHERE s = 'open"));
  CHECK_THROWS(parseStatement("SELECT * FROM t WHERE a = 1 b"));
  CHECK_THROWS(parseStatement("SELECT * FROM t LIMIT x"));
}

TEST(binder_rejects_invalid_queries) {
  Database db;
  DemoDataOptions opt;
  opt.large = false;
  opt.skewed = false;
  loadDemoData(db, opt);
  CHECK_THROWS(db.execute("SELECT nope FROM students"));
  CHECK_THROWS(db.execute("SELECT * FROM missing"));
  CHECK_THROWS(db.execute("SELECT student_id FROM grades, enrollments"));          // ambiguous
  CHECK_THROWS(db.execute("SELECT * FROM students WHERE name = 5"));               // type mismatch
  CHECK_THROWS(db.execute("SELECT name, COUNT(*) FROM students GROUP BY age"));    // not grouped
  CHECK_THROWS(db.execute("SELECT * FROM students s, grades s"));                  // duplicate alias
  CHECK_THROWS(db.execute("SELECT * FROM students WHERE COUNT(*) > 1"));           // aggregate in WHERE
  CHECK_THROWS(db.execute("SELECT SUM(name) FROM students"));                      // non-numeric SUM
  CHECK_THROWS(db.execute("SET memory_pages = 2"));
}

TEST(equivalence_classes_and_constant_propagation) {
  Database db;
  DemoDataOptions opt;
  opt.large = false;
  opt.skewed = false;
  loadDemoData(db, opt);
  Statement st = parseStatement(
      "SELECT * FROM students s, grades g, enrollments e "
      "WHERE s.id = g.student_id AND g.student_id = e.student_id AND e.student_id = 7");
  BoundQuery q = bindSelect(*st.select, db.catalog());
  bool found = false;
  for (const EquivClass& c : q.classes)
    if (c.members.size() == 3 && c.hasConst && c.constant.asInt() == 7) found = true;
  CHECK(found);
  for (const Relation& r : q.rels) CHECK(!r.filters.empty());  // x = 7 pushed into every scan
  // s and e are joinable although no predicate mentions them together.
  QueryResult res = db.execute(
      "SELECT COUNT(*) FROM students s, grades g, enrollments e "
      "WHERE s.id = g.student_id AND g.student_id = e.student_id AND e.student_id = 7");
  CHECK_EQ(res.rows[0][0].asInt(), int64_t{5});
  // Range propagation: s.id < 10 also filters grades before the join.
  Statement rs = parseStatement("SELECT * FROM students s, grades g WHERE s.id = g.student_id AND s.id < 10");
  BoundQuery rq = bindSelect(*rs.select, db.catalog());
  CHECK_EQ(rq.rels[1].filters.size(), size_t{1});
  CHECK(rq.rels[1].filterDerived[0]);
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM students s, grades g WHERE s.id = g.student_id AND s.id < 10"), int64_t{45});
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM students WHERE id = 3 AND id = 4"), int64_t{0});
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM students WHERE 1 = 0"), int64_t{0});
}

TEST(original_regression_queries_return_correct_results) {
  Database db;
  DemoDataOptions opt;
  opt.large = false;
  opt.skewed = false;
  loadDemoData(db, opt);
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM students WHERE age > 20"), int64_t{40});
  CHECK_EQ(scalar(db, "SELECT COUNT(*) FROM students JOIN grades ON students.id = grades.student_id WHERE students.age = 18"),
           int64_t{100});
  // TC8: the original engine returned 5 rows here because its BNLJ never rescanned the inner.
  CHECK_EQ(scalar(db,
                  "SELECT COUNT(*) FROM students JOIN grades ON students.id = grades.student_id "
                  "JOIN enrollments ON students.id = enrollments.student_id "
                  "JOIN courses ON enrollments.course_id = courses.id WHERE students.age > 18"),
           int64_t{350});
  QueryResult r = db.execute("SELECT title FROM courses WHERE id = 'CS2'");
  CHECK_EQ(r.rows.size(), size_t{1});
  CHECK_EQ(r.rows[0][0].asString(), std::string("Course2"));
  r = db.execute("SELECT age, COUNT(*) AS n FROM students GROUP BY age ORDER BY n DESC, age LIMIT 2");
  CHECK_EQ(r.rows.size(), size_t{2});
  CHECK_EQ(r.rows[0][0].asInt(), int64_t{18});
  r = db.execute("SELECT COUNT(*), SUM(age), MIN(name), AVG(age) FROM students WHERE id > 1000");
  CHECK_EQ(r.rows[0][0].asInt(), int64_t{0});
  CHECK(r.rows[0][1].isNull() && r.rows[0][2].isNull() && r.rows[0][3].isNull());
}
