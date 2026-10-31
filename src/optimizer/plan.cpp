#include "optimizer/plan.h"

#include <cstdio>
#include <sstream>

namespace sysr {

const char* physKindName(PhysKind k) {
  switch (k) {
    case PhysKind::SeqScan: return "SeqScan";
    case PhysKind::IndexScan: return "IndexScan";
    case PhysKind::Filter: return "Filter";
    case PhysKind::HashJoin: return "HashJoin";
    case PhysKind::MergeJoin: return "MergeJoin";
    case PhysKind::NestedLoop: return "BlockNestedLoopJoin";
    case PhysKind::IndexNLJ: return "IndexNestedLoopJoin";
    case PhysKind::Sort: return "Sort";
    case PhysKind::HashAgg: return "HashAggregate";
    case PhysKind::TopN: return "TopN";
    case PhysKind::Limit: return "Limit";
    case PhysKind::Project: return "Project";
  }
  return "?";
}

namespace {

std::string joinExprs(const std::vector<ExprPtr>& es, const char* sep) {
  std::string s;
  for (size_t i = 0; i < es.size(); ++i) s += (i ? sep : "") + exprToString(*es[i]);
  return s;
}

std::string fmt(double v) {
  char buf[64];
  if (v >= 100 || v == static_cast<double>(static_cast<int64_t>(v))) std::snprintf(buf, sizeof buf, "%.0f", v);
  else std::snprintf(buf, sizeof buf, "%.2f", v);
  return buf;
}

std::string relName(const BoundQuery& q, int r) {
  const Relation& rel = q.rels[static_cast<size_t>(r)];
  return rel.alias == rel.table->name ? rel.table->name : rel.table->name + " AS " + rel.alias;
}

std::string keysText(const BoundQuery& q, const std::vector<JoinKey>& keys) {
  std::string s;
  for (size_t i = 0; i < keys.size(); ++i)
    s += (i ? " AND " : "") + q.colName(keys[i].left) + " = " + q.colName(keys[i].right);
  return s;
}

std::string sortText(const std::vector<SortKey>& keys) {
  std::string s;
  for (size_t i = 0; i < keys.size(); ++i) s += (i ? ", " : "") + exprToString(*keys[i].expr) + (keys[i].desc ? " DESC" : "");
  return s;
}

std::string describe(const PhysNode& p, const BoundQuery& q, const CostParams& params) {
  std::string s = physKindName(p.kind);
  switch (p.kind) {
    case PhysKind::SeqScan:
      s += " " + relName(q, p.rel);
      if (!p.filters.empty()) s += "  filter: " + joinExprs(p.filters, " AND ");
      break;
    case PhysKind::IndexScan: {
      s += " " + relName(q, p.rel) + " using " + p.index->name;
      std::string lo = p.hasLo ? std::to_string(p.lo) : "-inf";
      std::string hi = p.hasHi ? std::to_string(p.hi) : "+inf";
      s += (p.hasLo || p.hasHi) ? " range [" + lo + ", " + hi + "]" : " (full, for order)";
      s += p.index->clustered ? " clustered" : " unclustered";
      if (!p.filters.empty()) s += "  filter: " + joinExprs(p.filters, " AND ");
      break;
    }
    case PhysKind::Filter:
      s += " " + joinExprs(p.filters, " AND ");
      break;
    case PhysKind::HashJoin: {
      const PhysNode& build = *p.kids[p.buildRight ? 1 : 0];
      s += std::string(" build=") + (p.buildRight ? "right" : "left") +
           (costmodel::hashFits(build.pages, params) ? " in-memory" : " grace") + "  on " + keysText(q, p.keys);
      break;
    }
    case PhysKind::MergeJoin:
      s += "  on " + keysText(q, p.keys);
      break;
    case PhysKind::NestedLoop:
      s += p.keys.empty() ? "  cross/theta" : "  on " + keysText(q, p.keys) + " (hashed blocks)";
      break;
    case PhysKind::IndexNLJ:
      s += " inner=" + relName(q, p.rel) + " using " + p.index->name + "  on " + keysText(q, p.keys);
      if (!p.filters.empty()) s += "  inner filter: " + joinExprs(p.filters, " AND ");
      break;
    case PhysKind::Sort: {
      int passes = costmodel::sortPasses(p.kids[0]->pages, params);
      s += " by " + sortText(p.sortKeys) + (passes ? " (external, " + std::to_string(passes) + " merge pass" +
                                                         (passes > 1 ? "es)" : ")")
                                                   : " (in-memory)");
      break;
    }
    case PhysKind::HashAgg: {
      std::vector<std::string> aggs;
      s += q.groupKeys.empty() ? "" : " group by " + joinExprs(q.groupKeys, ", ");
      s += "  aggs: ";
      for (size_t i = 0; i < q.aggs.size(); ++i) s += (i ? ", " : "") + q.aggs[i].text;
      break;
    }
    case PhysKind::TopN:
      s += " " + std::to_string(p.limit) + " by " + sortText(p.sortKeys);
      break;
    case PhysKind::Limit:
      s += " " + std::to_string(p.limit);
      break;
    case PhysKind::Project: {
      s += " ";
      for (size_t i = 0; i < p.names.size(); ++i) s += (i ? ", " : "") + p.names[i];
      break;
    }
  }
  if (!p.residual.empty()) s += "  residual: " + joinExprs(p.residual, " AND ");
  if (p.order >= 0 && p.kind != PhysKind::Sort) {
    // Name the class member this subtree actually produces.
    const auto& members = q.classes[static_cast<size_t>(p.order)].members;
    ColRef shown = members[0];
    for (const ColRef& m : members)
      if ((p.rels >> m.rel) & 1u) { shown = m; break; }
    s += "  [sorted: " + q.colName(shown) + "]";
  }
  return s;
}

void render(const PhysNode& p, const BoundQuery& q, const CostParams& params, int depth, std::ostringstream& out,
            const std::function<std::string(const PhysNode&)>& annotate) {
  out << std::string(static_cast<size_t>(depth) * 3, ' ') << "-> " << describe(p, q, params) << "  (rows=" << fmt(p.rows)
      << " cost=" << fmt(p.totalMs) << "ms";
  if (annotate) out << annotate(p);
  out << ")\n";
  for (const auto& k : p.kids) render(*k, q, params, depth + 1, out, annotate);
}

}  // namespace

std::string explainPlan(const PhysNode& root, const BoundQuery& q, const CostParams& params,
                        const std::function<std::string(const PhysNode&)>& annotate) {
  std::ostringstream out;
  render(root, q, params, 0, out, annotate);
  return out.str();
}

}  // namespace sysr
