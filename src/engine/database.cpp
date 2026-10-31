#include "engine/database.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <sstream>

#include "planner/binder.h"

namespace sysr {

namespace {

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

std::string fixed(double v, int digits = 2) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  return buf;
}

Value literalValue(const AstPtr& a) {
  switch (a->kind) {
    case AstExpr::Kind::Int: return Value::Int(a->ival);
    case AstExpr::Kind::Double: return Value::Double(a->dval);
    case AstExpr::Kind::Str: return Value::String(a->sval);
    case AstExpr::Kind::Unary:
      if (a->op == "-") {
        Value v = literalValue(a->args[0]);
        if (v.type() == TypeId::Int) return Value::Int(-v.asInt());
        if (v.type() == TypeId::Double) return Value::Double(-v.asDouble());
      }
      break;
    default: break;
  }
  throw DbError("INSERT values must be literals, got " + a->toString());
}

bool parseBool(const std::string& name, const std::string& v) {
  if (v == "on" || v == "true" || v == "1") return true;
  if (v == "off" || v == "false" || v == "0") return false;
  throw DbError("SET " + name + ": expected on/off, got '" + v + "'");
}

QueryResult status(std::string msg) {
  QueryResult r;
  r.message = std::move(msg);
  return r;
}

}  // namespace

Database::Database(size_t bufferFrames) : pool_(&disk_, bufferFrames), catalog_(&pool_) {
  cfg_.cost.bufferFrames = bufferFrames;
}

QueryResult Database::execute(const std::string& sql) { return run(parseStatement(sql)); }

std::vector<QueryResult> Database::executeScript(const std::string& sql) {
  std::vector<QueryResult> out;
  for (const Statement& st : parseScript(sql)) out.push_back(run(st));
  return out;
}

QueryResult Database::run(const Statement& st) {
  switch (st.kind) {
    case Statement::Kind::Select: return runSelect(*st.select, false, false);
    case Statement::Kind::Explain: return runSelect(*st.select, true, st.explainAnalyze);
    case Statement::Kind::CreateTable: {
      std::vector<Column> cols;
      for (const auto& c : st.columns) cols.push_back(Column{c.name, c.type});
      createTable(st.table, cols);
      return status("CREATE TABLE " + st.table);
    }
    case Statement::Kind::CreateIndex: {
      TableInfo& t = catalog_.require(st.table);
      IndexInfo* ix = catalog_.createIndex(t, st.column, st.indexName);
      return status("CREATE INDEX " + ix->name + " (height " + std::to_string(ix->tree->height()) + ", " +
                    std::to_string(ix->tree->numPages()) + " pages)");
    }
    case Statement::Kind::Insert: {
      TableInfo& t = catalog_.require(st.table);
      for (const auto& row : st.rows) {
        Tuple tuple;
        for (const auto& e : row) tuple.push_back(literalValue(e));
        catalog_.insert(t, std::move(tuple));
      }
      return status("INSERT " + std::to_string(st.rows.size()));
    }
    case Statement::Kind::Analyze: {
      if (st.table.empty()) analyzeAll();
      else catalog_.analyze(catalog_.require(st.table));
      return status("ANALYZE");
    }
    case Statement::Kind::Set: return runSet(st);
    case Statement::Kind::ShowTables: return showTables();
    case Statement::Kind::ShowStats: return showStats(st.table);
  }
  throw DbError("unsupported statement");
}

TableInfo* Database::createTable(const std::string& name, const std::vector<Column>& cols) {
  Schema s;
  s.cols = cols;
  return catalog_.createTable(name, s);
}

void Database::insert(const std::string& table, Tuple row) { catalog_.insert(catalog_.require(table), std::move(row)); }

void Database::analyzeAll() {
  for (TableInfo* t : catalog_.tables()) catalog_.analyze(*t);
}

void Database::createIndex(const std::string& table, const std::string& column) {
  catalog_.createIndex(catalog_.require(table), column);
}

QueryResult Database::runSelect(const SelectStmt& s, bool explain, bool analyze) {
  // Statistics are refreshed lazily: a table modified since its last ANALYZE is re-analyzed
  // before planning (the physical-order statistic must be exact for merge joins).
  for (const TableRef& tr : s.from) {
    TableInfo* t = catalog_.find(tr.table);
    if (t && t->statsStale) catalog_.analyze(*t);
  }

  auto t0 = Clock::now();
  BoundQuery q = bindSelect(s, catalog_);
  PlanResult pr = optimize(q, cfg_);
  QueryResult res;
  res.planningMs = msSince(t0);
  res.optimizer = pr.stats;
  res.estimated = pr.root->total;
  res.estimatedMs = pr.root->totalMs;
  res.columns = q.outputNames;
  res.plan = explainPlan(*pr.root, q, cfg_.cost);

  std::ostringstream header;
  header << "Optimizer: " << (cfg_.bushy ? "bushy" : "left-deep") << " DP, interesting orders "
         << (cfg_.interestingOrders ? "on" : "off") << ", estimator=" << estimatorName(cfg_.estimator)
         << ", M=" << cfg_.cost.memPages << " pages | " << pr.stats.subsetsPlanned << " relation sets, "
         << pr.stats.splitsEvaluated << " splits, " << pr.stats.plansConsidered << " plans costed, "
         << pr.stats.plansKept << " kept | planning " << fixed(res.planningMs, 3) << " ms\n";

  if (explain && !analyze) {
    res.message = header.str() + res.plan;
    return res;
  }

  if (analyze || coldCache) pool_.evictAll();
  const IOStats io0 = disk_.stats();
  const BufferStats b0 = pool_.stats();
  ExecContext ctx{&catalog_, &pool_, &disk_, &q, cfg_.cost};
  auto t1 = Clock::now();
  OpPtr root = buildOperator(*pr.root, ctx);
  root->open();
  Tuple t;
  while (root->next(t)) res.rows.push_back(t);
  root->close();
  res.executionMs = msSince(t1);
  res.io = disk_.stats() - io0;
  const BufferStats& b1 = pool_.stats();
  res.buffer = BufferStats{b1.hits - b0.hits, b1.misses - b0.misses, b1.evictions - b0.evictions,
                           b1.writebacks - b0.writebacks};

  if (!analyze) {
    res.hasRows = true;
    return res;
  }

  std::map<const PhysNode*, const Operator*> ops;
  std::vector<const Operator*> stack{root.get()};
  while (!stack.empty()) {
    const Operator* op = stack.back();
    stack.pop_back();
    ops[op->node()] = op;
    for (const Operator* c : op->children) stack.push_back(c);
  }
  auto annotate = [&](const PhysNode& p) {
    auto it = ops.find(&p);
    if (it == ops.end()) return std::string();
    std::string s = " actual=" + std::to_string(it->second->rowsProduced());
    if (!it->second->runtimeNote().empty()) s += "; " + it->second->runtimeNote();
    return s;
  };
  const CostParams& cp = cfg_.cost;
  std::ostringstream out;
  out << header.str() << explainPlan(*pr.root, q, cp, annotate);
  out << "Estimated I/O: " << fixed(res.estimated.transfers, 0) << " transfers, " << fixed(res.estimated.seeks, 0)
      << " seeks  -> " << fixed(res.estimated.transfers * cp.transferMs + res.estimated.seeks * cp.seekMs) << " ms\n";
  out << "Measured  I/O: " << res.io.transfers() << " transfers (" << res.io.reads << " reads, " << res.io.writes
      << " writes), " << res.io.seeks << " seeks  -> "
      << fixed(static_cast<double>(res.io.transfers()) * cp.transferMs + static_cast<double>(res.io.seeks) * cp.seekMs)
      << " ms\n";
  out << "Buffer pool: " << res.buffer.hits << " hits, " << res.buffer.misses << " misses, " << res.buffer.evictions
      << " evictions\n";
  out << "Execution: " << res.rows.size() << " rows in " << fixed(res.executionMs) << " ms\n";
  res.message = out.str();
  return res;
}

QueryResult Database::runSet(const Statement& st) {
  const std::string& n = st.setName;
  const std::string& v = st.setValue;
  auto positive = [&](size_t minimum) {
    long long x;
    try { x = std::stoll(v); } catch (...) { throw DbError("SET " + n + ": expected an integer"); }
    if (x < static_cast<long long>(minimum)) throw DbError("SET " + n + ": must be at least " + std::to_string(minimum));
    return static_cast<size_t>(x);
  };
  if (n == "bushy") cfg_.bushy = parseBool(n, v);
  else if (n == "interesting_orders") cfg_.interestingOrders = parseBool(n, v);
  else if (n == "enable_hashjoin") cfg_.enableHashJoin = parseBool(n, v);
  else if (n == "enable_mergejoin") cfg_.enableMergeJoin = parseBool(n, v);
  else if (n == "enable_nestloop") cfg_.enableNestedLoop = parseBool(n, v);
  else if (n == "enable_indexnlj") cfg_.enableIndexNLJ = parseBool(n, v);
  else if (n == "enable_indexscan") cfg_.enableIndexScan = parseBool(n, v);
  else if (n == "cold_cache") coldCache = parseBool(n, v);
  else if (n == "memory_pages") cfg_.cost.memPages = positive(4);
  else if (n == "io_chunk") cfg_.cost.ioChunk = positive(1);
  else if (n == "estimator") {
    if (v == "histogram") cfg_.estimator = EstimatorMode::Histogram;
    else if (v == "uniform") cfg_.estimator = EstimatorMode::Uniform;
    else if (v == "magic") cfg_.estimator = EstimatorMode::Magic;
    else throw DbError("SET estimator: expected histogram, uniform or magic");
  } else {
    throw DbError("unknown setting '" + n + "'");
  }
  return status("SET " + n + " = " + v);
}

QueryResult Database::showTables() const {
  QueryResult r;
  r.hasRows = true;
  r.columns = {"table", "rows", "pages", "columns", "indexes"};
  for (TableInfo* t : catalog_.tables()) {
    std::string cols, idx;
    for (const auto& c : t->schema.cols) cols += (cols.empty() ? "" : ", ") + c.name + " " + typeName(c.type);
    for (const auto& ix : t->indexes)
      idx += (idx.empty() ? "" : ", ") + ix->name + (ix->clustered ? " (clustered)" : "");
    r.rows.push_back({Value::String(t->name), Value::Int(static_cast<int64_t>(t->heap->numTuples())),
                      Value::Int(static_cast<int64_t>(t->heap->numPages())), Value::String(cols), Value::String(idx)});
  }
  return r;
}

QueryResult Database::showStats(const std::string& table) const {
  TableInfo& t = catalog_.require(table);
  if (t.statsStale) const_cast<Catalog&>(catalog_).analyze(t);
  QueryResult r;
  r.hasRows = true;
  r.columns = {"column", "type", "ndv", "min", "max", "correlation", "mcv (top 3)", "histogram"};
  for (size_t c = 0; c < t.schema.size(); ++c) {
    const ColumnStats& cs = t.stats.cols[c];
    std::string mcv;
    for (size_t i = 0; i < cs.mcv.size() && i < 3; ++i)
      mcv += (i ? ", " : "") + cs.mcv[i].first.toString() + "=" + fixed(cs.mcv[i].second * 100, 1) + "%";
    if (cs.mcv.size() > 3) mcv += ", ... (" + std::to_string(cs.mcv.size()) + ")";
    r.rows.push_back({Value::String(t.schema.cols[c].name), Value::String(typeName(cs.type)),
                      Value::Int(static_cast<int64_t>(cs.ndv)), cs.min, cs.max, Value::String(fixed(cs.correlation, 3)),
                      Value::String(mcv), Value::String(std::to_string(cs.hist.size()) + " buckets")});
  }
  return r;
}

}  // namespace sysr
