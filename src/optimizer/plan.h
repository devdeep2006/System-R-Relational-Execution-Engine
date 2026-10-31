#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "optimizer/cost_model.h"
#include "planner/binder.h"

namespace sysr {

enum class PhysKind {
  SeqScan, IndexScan, Filter, HashJoin, MergeJoin, NestedLoop, IndexNLJ, Sort, HashAgg, TopN, Limit, Project
};

struct JoinKey {
  ColRef left, right;  // left column comes from kids[0], right from kids[1] (or the inner relation)
  int cls = -1;        // equivalence class the pair belongs to
};

struct PhysNode {
  PhysKind kind = PhysKind::SeqScan;
  std::vector<std::shared_ptr<PhysNode>> kids;

  // Scans; IndexNLJ inner relation.
  int rel = -1;
  const IndexInfo* index = nullptr;
  bool hasLo = false, hasHi = false;
  int64_t lo = 0, hi = 0;
  std::vector<ExprPtr> filters;

  // Joins.
  std::vector<JoinKey> keys;
  std::vector<ExprPtr> residual;
  bool buildRight = true;  // HashJoin: hash kids[1] and probe with kids[0]

  // Sort / TopN / Limit.
  std::vector<SortKey> sortKeys;
  int64_t limit = -1;

  // Project.
  std::vector<ExprPtr> exprs;
  std::vector<std::string> names;

  // Logical and physical properties.
  uint32_t rels = 0;
  int order = -1;  // equivalence class the output is sorted on (ascending), -1 if none
  double rows = 0, pages = 0;
  Cost self, total;
  double totalMs = 0;
  Layout layout;
};
using PhysPtr = std::shared_ptr<PhysNode>;

const char* physKindName(PhysKind k);

// Renders the plan tree. `annotate` may append runtime details (EXPLAIN ANALYZE).
std::string explainPlan(const PhysNode& root, const BoundQuery& q, const CostParams& params,
                        const std::function<std::string(const PhysNode&)>& annotate = nullptr);

}  // namespace sysr
