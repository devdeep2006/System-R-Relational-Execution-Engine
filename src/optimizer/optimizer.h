#pragma once

#include "optimizer/cardinality.h"
#include "optimizer/plan.h"

namespace sysr {

struct OptimizerConfig {
  bool bushy = false;              // search bushy trees (all O(3^n) splits) instead of left-deep only
  bool interestingOrders = true;   // keep the best plan per (relation set, sort order)
  bool exhaustive = false;         // keep every plan (no pruning); test oracle for the DP
  bool enableHashJoin = true;
  bool enableMergeJoin = true;
  bool enableNestedLoop = true;
  bool enableIndexNLJ = true;
  bool enableIndexScan = true;
  EstimatorMode estimator = EstimatorMode::Histogram;
  CostParams cost;
};

struct OptimizerStats {
  size_t relations = 0;
  size_t subsetsPlanned = 0;   // relation sets that received at least one plan
  size_t splitsEvaluated = 0;  // (S1, S2) pairs enumerated
  size_t plansConsidered = 0;  // candidate physical plans costed
  size_t plansKept = 0;        // plans retained in the DP table
  double planningMs = 0;
};

struct PlanResult {
  PhysPtr root;
  OptimizerStats stats;
};

// System R style bottom-up dynamic programming over relation subsets with interesting
// orders, access path selection and four join algorithms.
PlanResult optimize(const BoundQuery& q, const OptimizerConfig& cfg);

}  // namespace sysr
