#pragma once

#include <map>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "planner/expr.h"
#include "sql/ast.h"

namespace sysr {

struct Relation {
  std::string alias;
  TableInfo* table = nullptr;
  std::vector<ExprPtr> filters;     // single-relation predicates, evaluated inside the scan
  std::vector<bool> filterDerived;  // inferred through an equivalence class (a.x = b.y AND b.y = 5 => a.x = 5)
  std::vector<bool> filterInClass;  // a.x = a.y; its selectivity is accounted for by the class
  std::vector<int> needed;          // columns carried above the scan, ascending
};

// Columns proven equal by the WHERE/ON equalities (union-find closure). Every pair of members
// in different relations is a usable join predicate, so A.x = B.y AND B.y = C.z also lets the
// optimizer join A with C directly.
struct EquivClass {
  std::vector<ColRef> members;
  uint32_t relMask = 0;
  bool hasConst = false;
  Value constant;
};

// Predicate spanning several relations that is not a column equality (e.g. a.x < b.y).
struct ComplexPred {
  ExprPtr expr;
  uint32_t rels = 0;
};

enum class AggFn { CountStar, Count, Sum, Avg, Min, Max };

struct AggSpec {
  AggFn fn = AggFn::CountStar;
  ExprPtr arg;  // over the join layout; null for COUNT(*)
  TypeId type = TypeId::Int;
  std::string text;
};

struct SortKey {
  ExprPtr expr;
  bool desc = false;
};

struct BoundQuery {
  std::vector<Relation> rels;
  std::vector<EquivClass> classes;
  std::vector<ComplexPred> complex;
  std::map<ColRef, int> classOfCol;
  bool alwaysFalse = false;

  bool hasAgg = false;
  std::vector<ExprPtr> groupKeys;  // over the join layout
  std::vector<AggSpec> aggs;

  // Without aggregation these are over the join layout; with aggregation they use Slot nodes
  // over the aggregate output [group keys..., aggregates...].
  std::vector<ExprPtr> output;
  std::vector<std::string> outputNames;
  std::vector<SortKey> orderBy;
  int64_t limit = -1;

  int classOf(ColRef c) const {
    auto it = classOfCol.find(c);
    return it == classOfCol.end() ? -1 : it->second;
  }
  std::string colName(ColRef c) const {
    return rels[static_cast<size_t>(c.rel)].alias + "." +
           rels[static_cast<size_t>(c.rel)].table->schema.cols[static_cast<size_t>(c.col)].name;
  }
  TypeId colType(ColRef c) const {
    return rels[static_cast<size_t>(c.rel)].table->schema.cols[static_cast<size_t>(c.col)].type;
  }
};

constexpr size_t kMaxRelations = 16;

BoundQuery bindSelect(const SelectStmt& stmt, const Catalog& catalog);

}  // namespace sysr
