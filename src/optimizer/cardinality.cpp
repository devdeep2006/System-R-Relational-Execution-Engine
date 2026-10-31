#include "optimizer/cardinality.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "storage/page.h"

namespace sysr {

const char* estimatorName(EstimatorMode m) {
  switch (m) {
    case EstimatorMode::Histogram: return "histogram";
    case EstimatorMode::Uniform: return "uniform";
    case EstimatorMode::Magic: return "magic";
  }
  return "?";
}

namespace {

bool colConst(const Expr& e, ColRef* col, CmpOp* op, Value* v) { return splitColConst(e, col, op, v); }

// Interval bounds as inclusive doubles; strict integer bounds become inclusive (x > 5 -> x >= 6).
void normalize(const Interval& iv, bool integral, double* lo, double* hi) {
  const double inf = std::numeric_limits<double>::infinity();
  *lo = -inf;
  *hi = inf;
  if (iv.hasLo) *lo = integral ? (iv.loInc ? std::ceil(iv.lo) : std::floor(iv.lo) + 1) : iv.lo;
  if (iv.hasHi) *hi = integral ? (iv.hiInc ? std::floor(iv.hi) : std::ceil(iv.hi) - 1) : iv.hi;
}

// Fraction of the column's [min, max] value domain covered by the interval.
double domainFraction(const ColumnStats& s, const Interval& iv) {
  const bool integral = s.type == TypeId::Int;
  double lo, hi;
  normalize(iv, integral, &lo, &hi);
  double mn = s.min.asDouble(), mx = s.max.asDouble();
  double a = std::max(lo, mn), z = std::min(hi, mx);
  if (z < a) return 0.0;
  if (integral) return (z - a + 1.0) / (mx - mn + 1.0);
  return mx == mn ? 1.0 : (z - a) / (mx - mn);
}

void tighten(Interval& iv, CmpOp op, double v) {
  if (op == CmpOp::Gt || op == CmpOp::Ge) {
    bool inc = op == CmpOp::Ge;
    if (!iv.hasLo || v > iv.lo || (v == iv.lo && !inc)) { iv.hasLo = true; iv.lo = v; iv.loInc = inc; }
  } else {
    bool inc = op == CmpOp::Le;
    if (!iv.hasHi || v < iv.hi || (v == iv.hi && !inc)) { iv.hasHi = true; iv.hi = v; iv.hiInc = inc; }
  }
}

}  // namespace

CardinalityEstimator::CardinalityEstimator(const BoundQuery& q, EstimatorMode mode) : q_(q), mode_(mode) {
  for (const Relation& r : q.rels) {
    const TableStats& ts = r.table->stats;
    base_.push_back(ts.valid ? ts.rows : static_cast<double>(r.table->heap->numTuples()));
    for (const auto& f : r.filters) {
      ColRef c;
      CmpOp op;
      Value v;
      if (!colConst(*f, &c, &op, &v)) continue;
      if (op == CmpOp::Eq) constCols_.insert(c);
      else if (v.isNumeric() && op != CmpOp::Ne) tighten(rangeCols_[c], op, v.asDouble());
    }
  }
  for (size_t i = 0; i < q.rels.size(); ++i) {
    std::vector<ExprPtr> own;
    for (size_t f = 0; f < q.rels[i].filters.size(); ++f)
      if (!q.rels[i].filterInClass[f]) own.push_back(q.rels[i].filters[f]);
    filtered_.push_back(std::max(1.0, base_[i] * conjunctionSelectivity(own)));
  }
}

const ColumnStats* CardinalityEstimator::stats(ColRef c) const {
  const TableStats& ts = q_.rels[static_cast<size_t>(c.rel)].table->stats;
  return ts.valid ? &ts.cols[static_cast<size_t>(c.col)] : nullptr;
}

double CardinalityEstimator::baseNdv(ColRef c) const {
  const ColumnStats* s = stats(c);
  return std::max(1.0, s ? s->ndv : base_[static_cast<size_t>(c.rel)]);
}

// Distinct values that survive the relation's filters. Filters on other columns thin rows out
// uniformly (expected distinct values among f of b rows drawn from d values); a range filter
// on the column itself removes whole values, keeping the covered fraction of its domain.
double CardinalityEstimator::filteredNdv(ColRef c) const {
  if (constCols_.count(c)) return 1.0;
  double d = baseNdv(c);
  double b = base_[static_cast<size_t>(c.rel)];
  double f = filtered_[static_cast<size_t>(c.rel)];
  double kept = d;
  if (b > 0 && f < b) kept = d * (1.0 - std::pow(1.0 - f / b, b / d));
  auto it = rangeCols_.find(c);
  const ColumnStats* s = stats(c);
  if (it != rangeCols_.end() && s && (s->type == TypeId::Int || s->type == TypeId::Double) && !s->min.isNull())
    kept = std::min(kept, std::max(1.0, d * domainFraction(*s, it->second)));
  return std::max(1.0, std::min(kept, f));
}

double CardinalityEstimator::eqSelectivity(ColRef c, const Value& v) const {
  const ColumnStats* s = stats(c);
  if (mode_ == EstimatorMode::Magic || !s) return 0.1;
  bool numeric = v.isNumeric() && (s->type == TypeId::Int || s->type == TypeId::Double);
  if (numeric && !s->min.isNull() && (v.compare(s->min) < 0 || v.compare(s->max) > 0)) return 0.0;
  if (mode_ == EstimatorMode::Uniform) return 1.0 / std::max(1.0, s->ndv);
  for (const auto& m : s->mcv)
    if (m.first.compare(v) == 0) return m.second;
  double rest = s->ndv - static_cast<double>(s->mcv.size());
  if (rest < 1.0) return 0.0;
  return std::max(0.0, 1.0 - s->mcvFraction) / rest;
}

double CardinalityEstimator::rangeSelectivity(ColRef c, const Interval& iv) const {
  const ColumnStats* s = stats(c);
  bool numeric = s && (s->type == TypeId::Int || s->type == TypeId::Double) && !s->min.isNull();
  if (mode_ == EstimatorMode::Magic || !numeric) return (iv.hasLo && iv.hasHi) ? 0.25 : 1.0 / 3.0;

  const bool integral = s->type == TypeId::Int;
  double lo, hi;
  normalize(iv, integral, &lo, &hi);
  if (lo > hi) return 0.0;

  auto overlap = [&](double blo, double bhi) {
    double a = std::max(lo, blo), z = std::min(hi, bhi);
    if (z < a) return 0.0;
    if (integral) return (z - a + 1.0) / (bhi - blo + 1.0);
    if (bhi == blo) return 1.0;
    return (z - a) / (bhi - blo);
  };

  if (mode_ == EstimatorMode::Uniform) return std::min(1.0, overlap(s->min.asDouble(), s->max.asDouble()));

  double rows = base_[static_cast<size_t>(c.rel)];
  double sel = 0;
  for (const auto& m : s->mcv) {
    double x = m.first.asDouble();
    if (x >= lo && x <= hi) sel += m.second;
  }
  if (rows > 0)
    for (const Bucket& b : s->hist) sel += b.rows * overlap(b.lo, b.hi) / rows;
  return std::min(1.0, std::max(0.0, sel));
}

double CardinalityEstimator::selectivity(const Expr& e) const {
  switch (e.kind) {
    case ExprKind::Const:
      return e.value.truthy() ? 1.0 : 0.0;
    case ExprKind::And:
      return conjunctionSelectivity(e.kids);
    case ExprKind::Or: {
      double s = 0;
      for (const auto& k : e.kids) {
        double sk = selectivity(*k);
        s = s + sk - s * sk;
      }
      return s;
    }
    case ExprKind::Not:
      return 1.0 - selectivity(*e.kids[0]);
    case ExprKind::Cmp: {
      ColRef c;
      CmpOp op;
      Value v;
      if (colConst(e, &c, &op, &v)) {
        if (op == CmpOp::Eq) return eqSelectivity(c, v);
        if (op == CmpOp::Ne) return mode_ == EstimatorMode::Magic ? 0.9 : 1.0 - eqSelectivity(c, v);
        if (!v.isNumeric()) return 1.0 / 3.0;
        Interval iv;
        tighten(iv, op, v.asDouble());
        return rangeSelectivity(c, iv);
      }
      const Expr& l = *e.kids[0];
      const Expr& r = *e.kids[1];
      if (l.kind == ExprKind::Col && r.kind == ExprKind::Col && e.cmp == CmpOp::Eq)
        return 1.0 / std::max(baseNdv(l.ref), baseNdv(r.ref));
      return e.cmp == CmpOp::Eq ? 0.1 : 1.0 / 3.0;
    }
    default:
      return 0.5;
  }
}

double CardinalityEstimator::conjunctionSelectivity(const std::vector<ExprPtr>& preds) const {
  std::map<ColRef, Interval> ranges;
  double sel = 1.0;
  for (const auto& p : preds) {
    ColRef c;
    CmpOp op;
    Value v;
    if (colConst(*p, &c, &op, &v) && v.isNumeric() &&
        (op == CmpOp::Lt || op == CmpOp::Le || op == CmpOp::Gt || op == CmpOp::Ge)) {
      tighten(ranges[c], op, v.asDouble());
      continue;
    }
    sel *= selectivity(*p);
  }
  for (const auto& kv : ranges) sel *= rangeSelectivity(kv.first, kv.second);
  return sel;
}

double CardinalityEstimator::rows(uint32_t set) {
  auto it = memo_.find(set);
  if (it != memo_.end()) return it->second;
  double r = 1.0;
  for (size_t i = 0; i < q_.rels.size(); ++i)
    if (set & (1u << i)) r *= filtered_[i];
  // Each equivalence class with k members inside the set contributes k-1 equalities; joining
  // on the most selective (largest NDV) ones first gives 1/d_2 * ... * 1/d_k with d sorted.
  for (const EquivClass& cls : q_.classes) {
    std::vector<double> ndvs;
    for (const ColRef& m : cls.members)
      if (set & (1u << m.rel)) ndvs.push_back(filteredNdv(m));
    if (ndvs.size() < 2) continue;
    std::sort(ndvs.begin(), ndvs.end());
    for (size_t j = 1; j < ndvs.size(); ++j) r /= ndvs[j];
  }
  for (const ComplexPred& p : q_.complex)
    if ((p.rels & ~set) == 0) r *= selectivity(*p.expr);
  r = std::max(1.0, r);
  memo_[set] = r;
  return r;
}

double CardinalityEstimator::tupleBytes(uint32_t set) const {
  double bytes = 2.0;
  for (size_t i = 0; i < q_.rels.size(); ++i) {
    if (!(set & (1u << i))) continue;
    for (int c : q_.rels[i].needed) {
      const ColumnStats* s = stats(ColRef{static_cast<int>(i), c});
      bytes += s ? s->avgWidth : 9.0;
    }
  }
  return bytes;
}

double CardinalityEstimator::pages(uint32_t set) {
  double perTuple = slotted::footprint(static_cast<size_t>(std::ceil(tupleBytes(set))));
  return std::max(1.0, std::ceil(rows(set) * perTuple / static_cast<double>(slotted::kUsable)));
}

}  // namespace sysr
