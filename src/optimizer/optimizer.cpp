#include "optimizer/optimizer.h"

#include <chrono>
#include <cmath>
#include <limits>

namespace sysr {

namespace {

int popcount(uint32_t x) {
  int n = 0;
  while (x) { x &= x - 1; ++n; }
  return n;
}
int lowestBit(uint32_t x) {
  int i = 0;
  while (!(x & 1u)) { x >>= 1; ++i; }
  return i;
}

int64_t clampBound(double d) {
  if (d <= -9.0e18) return std::numeric_limits<int64_t>::min();
  if (d >= 9.0e18) return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(d);
}

class Planner {
 public:
  Planner(const BoundQuery& q, const OptimizerConfig& cfg)
      : q_(q), cfg_(cfg), est_(q, cfg.estimator), n_(static_cast<int>(q.rels.size())) {
    if (cfg_.interestingOrders && !q_.hasAgg && q_.orderBy.size() == 1 && !q_.orderBy[0].desc &&
        q_.orderBy[0].expr->kind == ExprKind::Col)
      orderClass_ = q_.classOf(q_.orderBy[0].expr->ref);
  }

  PlanResult run() {
    auto t0 = std::chrono::steady_clock::now();
    if (n_ == 0) throw DbError("query has no relations");
    computeConnectivity();
    const uint32_t full = (1u << n_) - 1;
    allowCross_ = !connectedSet_[full];
    enumerate();
    if (best_[full].empty()) {  // only possible with predicates over 3+ relations; retry allowing cross products
      allowCross_ = true;
      enumerate();
    }
    PhysPtr root;
    for (const PhysPtr& cand : best_[full]) {
      PhysPtr r = finishPlan(cand);
      if (!root || r->totalMs < root->totalMs) root = r;
    }
    for (const auto& v : best_) stats_.plansKept += v.size();
    stats_.relations = static_cast<size_t>(n_);
    stats_.planningMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return PlanResult{root, stats_};
  }

 private:
  // ------------------------------------------------------------------ helpers
  static PhysPtr node(PhysKind k) {
    auto p = std::make_shared<PhysNode>();
    p->kind = k;
    return p;
  }

  void price(PhysNode& p) const {
    Cost t = p.self;
    for (const auto& k : p.kids) t += k->total;
    p.total = t;
    p.totalMs = t.ms(cfg_.cost);
  }

  Layout relLayout(int r) const {
    Layout l;
    for (int c : q_.rels[static_cast<size_t>(r)].needed) l.push_back(ColRef{r, c});
    return l;
  }

  ExprPtr colExpr(ColRef c) const { return makeCol(c, q_.colType(c), q_.colName(c)); }

  // An order is worth keeping if a later merge join can use it, or ORDER BY asks for it.
  bool interesting(uint32_t S, int cls) const {
    if (!cfg_.interestingOrders || cls < 0) return false;
    if (cls == orderClass_) return true;
    uint32_t m = q_.classes[static_cast<size_t>(cls)].relMask;
    return popcount(m) >= 2 && (m & ~S) != 0;
  }

  void addPlan(uint32_t S, const PhysPtr& p) {
    ++stats_.plansConsidered;
    if (!interesting(S, p->order)) p->order = -1;
    auto& v = best_[S];
    if (cfg_.exhaustive) { v.push_back(p); return; }
    for (auto& existing : v) {
      if (existing->order == p->order) {
        if (p->totalMs < existing->totalMs) existing = p;
        return;
      }
    }
    v.push_back(p);
  }

  void computeConnectivity() {
    std::vector<uint32_t> adj(static_cast<size_t>(n_), 0);
    auto link = [&](uint32_t mask) {
      for (int i = 0; i < n_; ++i)
        if (mask & (1u << i)) adj[static_cast<size_t>(i)] |= mask & ~(1u << i);
    };
    for (const EquivClass& c : q_.classes)
      if (popcount(c.relMask) >= 2) link(c.relMask);
    for (const ComplexPred& p : q_.complex) link(p.rels);
    connectedSet_.assign(size_t{1} << n_, false);
    for (uint32_t S = 1; S < (1u << n_); ++S) {
      uint32_t seen = 1u << lowestBit(S), frontier = seen;
      while (frontier) {
        int i = lowestBit(frontier);
        frontier &= frontier - 1;
        uint32_t nb = adj[static_cast<size_t>(i)] & S & ~seen;
        seen |= nb;
        frontier |= nb;
      }
      connectedSet_[S] = seen == S;
    }
  }

  bool connected(uint32_t S1, uint32_t S2) const {
    for (const EquivClass& c : q_.classes)
      if ((c.relMask & S1) && (c.relMask & S2)) return true;
    for (const ComplexPred& p : q_.complex)
      if ((p.rels & ~(S1 | S2)) == 0 && (p.rels & S1) && (p.rels & S2)) return true;
    return false;
  }

  // ------------------------------------------------------------- enumeration
  void enumerate() {
    best_.assign(size_t{1} << n_, {});
    stats_ = OptimizerStats{};
    for (uint32_t S = 1; S < (1u << n_); ++S) {
      if (popcount(S) == 1) {
        planBase(lowestBit(S));
      } else {
        if (!allowCross_ && !connectedSet_[S]) continue;
        // Every split of S into two non-empty halves: O(3^n) over all subsets (bushy);
        // left-deep restricts the right (inner) side to a single base relation. Disabled
        // algorithms are a last resort: the fallback pass runs only if S got no plan.
        for (bool fallback : {false, true}) {
          for (uint32_t S1 = (S - 1) & S; S1 > 0; S1 = (S1 - 1) & S) {
            uint32_t S2 = S ^ S1;
            if (!cfg_.bushy && popcount(S2) != 1) continue;
            if (best_[S1].empty() || best_[S2].empty()) continue;
            if (!allowCross_ && !connected(S1, S2)) continue;
            ++stats_.splitsEvaluated;
            planJoins(S1, S2, fallback);
          }
          if (!best_[S].empty()) break;
        }
      }
      if (!best_[S].empty()) ++stats_.subsetsPlanned;
    }
  }

  // Sorted on a column of a multi-relation class or the ORDER BY class, if the heap is
  // physically in that order (correlation exactly 1 after ANALYZE).
  int physicalOrder(int r, uint32_t S) const {
    const TableStats& ts = q_.rels[static_cast<size_t>(r)].table->stats;
    if (!ts.valid) return -1;
    for (size_t k = 0; k < q_.classes.size(); ++k) {
      for (const ColRef& m : q_.classes[k].members) {
        if (m.rel != r || ts.cols[static_cast<size_t>(m.col)].correlation < 1.0) continue;
        if (interesting(S, static_cast<int>(k))) return static_cast<int>(k);
      }
    }
    return -1;
  }

  void planBase(int r) {
    const uint32_t S = 1u << r;
    const Relation& rel = q_.rels[static_cast<size_t>(r)];
    const TableInfo& t = *rel.table;
    const double heapPages = static_cast<double>(t.heap->numPages());
    const double baseRows = est_.baseRows(r);
    const double outRows = est_.rows(S), outPages = est_.pages(S);

    auto seq = node(PhysKind::SeqScan);
    seq->rel = r;
    seq->filters = rel.filters;
    seq->rels = S;
    seq->rows = outRows;
    seq->pages = outPages;
    seq->layout = relLayout(r);
    seq->order = physicalOrder(r, S);
    seq->self = costmodel::seqScan(heapPages, baseRows);
    price(*seq);
    addPlan(S, seq);

    if (!cfg_.enableIndexScan) return;
    for (const auto& ix : t.indexes) {
      const ColRef key{r, ix->column};
      int64_t lo = std::numeric_limits<int64_t>::min(), hi = std::numeric_limits<int64_t>::max();
      bool hasLo = false, hasHi = false;
      std::vector<ExprPtr> sarg, rest;
      for (const auto& f : rel.filters) {
        ColRef c;
        CmpOp op;
        Value v;
        if (splitColConst(*f, &c, &op, &v) && c == key && v.isNumeric() && op != CmpOp::Ne) {
          double d = v.asDouble();
          if (op == CmpOp::Eq || op == CmpOp::Ge) { lo = std::max(lo, clampBound(std::ceil(d))); hasLo = true; }
          if (op == CmpOp::Gt) { lo = std::max(lo, clampBound(std::floor(d) + 1)); hasLo = true; }
          if (op == CmpOp::Eq || op == CmpOp::Le) { hi = std::min(hi, clampBound(std::floor(d))); hasHi = true; }
          if (op == CmpOp::Lt) { hi = std::min(hi, clampBound(std::ceil(d) - 1)); hasHi = true; }
          sarg.push_back(f);
        } else {
          rest.push_back(f);
        }
      }
      const int cls = q_.classOf(key);
      if (sarg.empty() && !interesting(S, cls)) continue;  // a full unordered index scan never beats a seq scan
      const double sel = sarg.empty() ? 1.0 : est_.conjunctionSelectivity(sarg);
      auto ixs = node(PhysKind::IndexScan);
      ixs->rel = r;
      ixs->index = ix.get();
      ixs->hasLo = hasLo;
      ixs->hasHi = hasHi;
      ixs->lo = lo;
      ixs->hi = hi;
      ixs->filters = rest;
      ixs->rels = S;
      ixs->rows = outRows;
      ixs->pages = outPages;
      ixs->layout = relLayout(r);
      ixs->order = cls;
      ixs->self = costmodel::indexScan(sel, baseRows * sel, ix->tree->height(), static_cast<double>(ix->tree->leafPages()),
                                       heapPages, ix->clustered, cfg_.cost);
      price(*ixs);
      addPlan(S, ixs);
    }
  }

  PhysPtr makeJoin(PhysKind k, const PhysPtr& L, const PhysPtr& R, uint32_t S, double rows, double pages) const {
    auto j = node(k);
    j->kids = {L, R};
    j->rels = S;
    j->rows = rows;
    j->pages = pages;
    j->layout = L->layout;
    j->layout.insert(j->layout.end(), R->layout.begin(), R->layout.end());
    return j;
  }

  PhysPtr sortNode(const PhysPtr& child, const std::vector<SortKey>& keys, int order) const {
    auto s = node(PhysKind::Sort);
    s->kids = {child};
    s->sortKeys = keys;
    s->rels = child->rels;
    s->rows = child->rows;
    s->pages = child->pages;
    s->layout = child->layout;
    s->order = order;
    s->self = costmodel::externalSort(child->pages, child->rows, cfg_.cost);
    price(*s);
    return s;
  }

  // With `fallback`, only a nested loop join is generated (it handles any predicate).
  void planJoins(uint32_t S1, uint32_t S2, bool fallback) {
    const uint32_t S = S1 | S2;
    std::vector<JoinKey> keys;
    for (size_t k = 0; k < q_.classes.size(); ++k) {
      const EquivClass& cls = q_.classes[k];
      if (!(cls.relMask & S1) || !(cls.relMask & S2)) continue;
      JoinKey jk;
      jk.cls = static_cast<int>(k);
      for (const ColRef& m : cls.members) {
        if ((S1 >> m.rel) & 1u) { if (jk.left.rel < 0) jk.left = m; }
        else if ((S2 >> m.rel) & 1u) { if (jk.right.rel < 0) jk.right = m; }
      }
      keys.push_back(jk);
    }
    std::vector<ExprPtr> residual;
    for (const ComplexPred& p : q_.complex)
      if ((p.rels & ~S) == 0 && (p.rels & ~S1) != 0 && (p.rels & ~S2) != 0) residual.push_back(p.expr);
    auto keyEq = [&](const JoinKey& k) { return makeCmp(CmpOp::Eq, colExpr(k.left), colExpr(k.right)); };

    const double rows = est_.rows(S), pages = est_.pages(S);
    const CostParams& cp = cfg_.cost;

    // Index nested loop: the inner side must be one base relation with an index on its key.
    if (!fallback && cfg_.enableIndexNLJ && popcount(S2) == 1 && !keys.empty()) {
      const int r = lowestBit(S2);
      const Relation& inner = q_.rels[static_cast<size_t>(r)];
      for (size_t ki = 0; ki < keys.size(); ++ki) {
        const JoinKey& k = keys[ki];
        const IndexInfo* ix = inner.table->indexOn(k.right.col);
        if (!ix) continue;
        std::vector<ExprPtr> res = residual;
        for (size_t kj = 0; kj < keys.size(); ++kj)
          if (kj != ki) res.push_back(keyEq(keys[kj]));
        const double fetchedPerOuter =
            est_.baseRows(r) / std::max(est_.filteredNdv(k.left), est_.baseNdv(k.right));
        // Probe order: sorted if the outer plan carries the key's order; for a single base
        // relation, its heap's physical correlation on the key (a property of S1 alone, so
        // the DP's principle of optimality still holds).
        double heapCorr = 0;
        if (popcount(S1) == 1) {
          const TableStats& ts = q_.rels[static_cast<size_t>(k.left.rel)].table->stats;
          if (ts.valid) heapCorr = ts.cols[static_cast<size_t>(k.left.col)].correlation;
        }
        for (const PhysPtr& L : best_[S1]) {
          double corr = L->order == k.cls ? 1.0 : heapCorr;
          double runs = corr >= 0.9 ? 1.0 + (1.0 - corr) * L->rows : L->rows;
          auto j = node(PhysKind::IndexNLJ);
          j->kids = {L};
          j->rel = r;
          j->index = ix;
          j->keys = {k};
          j->filters = inner.filters;
          j->residual = res;
          j->rels = S;
          j->rows = rows;
          j->pages = pages;
          j->layout = L->layout;
          Layout il = relLayout(r);
          j->layout.insert(j->layout.end(), il.begin(), il.end());
          j->order = L->order;
          j->self = costmodel::indexNestedLoop(L->rows, L->pages, L->rows * fetchedPerOuter, runs, ix->tree->height(),
                                               static_cast<double>(ix->tree->leafPages()),
                                               static_cast<double>(inner.table->heap->numPages()), cp);
          price(*j);
          addPlan(S, j);
        }
      }
    }

    const bool useNL = cfg_.enableNestedLoop || fallback;
    for (const PhysPtr& L : best_[S1]) {
      for (const PhysPtr& R : best_[S2]) {
        if (!fallback && !keys.empty() && cfg_.enableHashJoin) {
          for (bool buildRight : {true, false}) {
            auto j = makeJoin(PhysKind::HashJoin, L, R, S, rows, pages);
            j->keys = keys;
            j->residual = residual;
            j->buildRight = buildRight;
            const PhysPtr& build = buildRight ? R : L;
            const PhysPtr& probe = buildRight ? L : R;
            j->self = costmodel::hashJoin(build->pages, build->rows, probe->pages, probe->rows, cp);
            j->order = costmodel::hashFits(build->pages, cp) ? probe->order : -1;  // in-memory probe streams in order
            price(*j);
            addPlan(S, j);
          }
        }
        if (!fallback && !keys.empty() && cfg_.enableMergeJoin) {
          const JoinKey& k0 = keys[0];
          PhysPtr l = L->order == k0.cls ? L : sortNode(L, {SortKey{colExpr(k0.left), false}}, k0.cls);
          PhysPtr r = R->order == k0.cls ? R : sortNode(R, {SortKey{colExpr(k0.right), false}}, k0.cls);
          auto j = makeJoin(PhysKind::MergeJoin, l, r, S, rows, pages);
          j->keys = {k0};
          j->residual = residual;
          for (size_t i = 1; i < keys.size(); ++i) j->residual.push_back(keyEq(keys[i]));
          j->order = k0.cls;
          j->self = Cost{0, 0, l->rows + r->rows};
          price(*j);
          addPlan(S, j);
        }
        if (useNL) {
          auto j = makeJoin(PhysKind::NestedLoop, L, R, S, rows, pages);
          j->keys = keys;
          j->residual = residual;
          j->self = costmodel::nestedLoop(L->pages, L->rows, R->pages, R->rows, !keys.empty(), cp);
          j->order = R->pages <= static_cast<double>(cp.memPages) - 2.0 ? L->order : -1;
          price(*j);
          addPlan(S, j);
        }
      }
    }
  }

  // --------------------------------------------------------------- finishing
  double estimateGroups(double inputRows) {
    if (q_.groupKeys.empty()) return 1.0;
    double g = 1.0;
    for (const auto& k : q_.groupKeys)
      g *= k->kind == ExprKind::Col ? est_.filteredNdv(k->ref) : std::max(1.0, inputRows / 10.0);
    return std::max(1.0, std::min(g, inputRows));
  }

  PhysPtr finishPlan(const PhysPtr& joinTree) {
    PhysPtr cur = joinTree;
    if (q_.alwaysFalse) {
      auto f = node(PhysKind::Filter);
      f->kids = {cur};
      f->filters = {makeConst(Value::Int(0))};
      f->rels = cur->rels;
      f->rows = 0;
      f->pages = 0;
      f->layout = cur->layout;
      price(*f);
      cur = f;
    }
    if (q_.hasAgg) {
      auto a = node(PhysKind::HashAgg);
      a->kids = {cur};
      a->rels = cur->rels;
      a->rows = estimateGroups(cur->rows);
      a->pages = std::max(1.0, std::ceil(a->rows * 32.0 / static_cast<double>(kPageSize)));
      a->self = costmodel::hashAggregate(cur->rows);
      price(*a);
      cur = a;
    }
    if (!q_.orderBy.empty()) {
      const bool satisfied = !q_.hasAgg && orderClass_ >= 0 && cur->order == orderClass_;
      if (!satisfied) {
        if (q_.limit >= 0) {
          auto t = node(PhysKind::TopN);
          t->kids = {cur};
          t->sortKeys = q_.orderBy;
          t->limit = q_.limit;
          t->rels = cur->rels;
          t->rows = std::min(cur->rows, static_cast<double>(q_.limit));
          t->pages = cur->pages;
          t->layout = cur->layout;
          t->self = costmodel::topN(cur->rows, static_cast<double>(q_.limit));
          price(*t);
          cur = t;
        } else {
          cur = sortNode(cur, q_.orderBy, orderClass_);
        }
      }
    }
    if (q_.limit >= 0 && cur->kind != PhysKind::TopN) {
      auto l = node(PhysKind::Limit);
      l->kids = {cur};
      l->limit = q_.limit;
      l->rels = cur->rels;
      l->rows = std::min(cur->rows, static_cast<double>(q_.limit));
      l->pages = cur->pages;
      l->layout = cur->layout;
      price(*l);
      cur = l;
    }
    auto p = node(PhysKind::Project);
    p->kids = {cur};
    p->exprs = q_.output;
    p->names = q_.outputNames;
    p->rels = cur->rels;
    p->rows = cur->rows;
    p->pages = cur->pages;
    p->self = Cost{0, 0, cur->rows};
    price(*p);
    return p;
  }

  const BoundQuery& q_;
  const OptimizerConfig& cfg_;
  CardinalityEstimator est_;
  int n_;
  int orderClass_ = -1;
  bool allowCross_ = false;
  std::vector<bool> connectedSet_;
  std::vector<std::vector<PhysPtr>> best_;
  OptimizerStats stats_;
};

}  // namespace

PlanResult optimize(const BoundQuery& q, const OptimizerConfig& cfg) {
  Planner p(q, cfg);
  return p.run();
}

}  // namespace sysr
