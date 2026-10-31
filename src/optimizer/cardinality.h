#pragma once

#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "planner/binder.h"

namespace sysr {

// How single-table predicates are estimated. Join selectivity always uses NDV.
enum class EstimatorMode {
  Histogram,  // MCV lists + equi-depth histograms (+ HyperLogLog NDV)
  Uniform,    // uniform distribution between min and max, 1/NDV for equality
  Magic       // System R's fixed default selectivities (1/10 equality, 1/3 range)
};

const char* estimatorName(EstimatorMode m);

class CardinalityEstimator {
 public:
  CardinalityEstimator(const BoundQuery& q, EstimatorMode mode);

  // Estimated output rows of joining the relations in `set` with every applicable predicate.
  // Depends only on the set, never on the join order, so all plans for a set agree.
  double rows(uint32_t set);
  // Pages occupied by rows(set) tuples carrying only the needed columns.
  double pages(uint32_t set);
  double tupleBytes(uint32_t set) const;

  double baseRows(int rel) const { return base_[static_cast<size_t>(rel)]; }
  double baseNdv(ColRef c) const;
  double filteredNdv(ColRef c) const;

  // Selectivity of a conjunction. Range predicates on the same column are merged into one
  // interval first, so x > 5 AND x < 10 is estimated as a single range, not as a product.
  double conjunctionSelectivity(const std::vector<ExprPtr>& preds) const;
  double selectivity(const Expr& e) const;

 private:
  const ColumnStats* stats(ColRef c) const;
  double eqSelectivity(ColRef c, const Value& v) const;
  double rangeSelectivity(ColRef c, const Interval& iv) const;

  const BoundQuery& q_;
  EstimatorMode mode_;
  std::vector<double> base_, filtered_;
  std::set<ColRef> constCols_;              // columns with an equality-to-constant filter
  std::map<ColRef, Interval> rangeCols_;    // columns restricted by a range filter
  std::unordered_map<uint32_t, double> memo_;
};

}  // namespace sysr
