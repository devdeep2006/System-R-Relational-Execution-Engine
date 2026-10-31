#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/value.h"

namespace sysr {

// A column of a FROM-clause relation: relation index in the bound query, column index in its table.
struct ColRef {
  int rel = -1;
  int col = -1;
  bool operator==(const ColRef& o) const { return rel == o.rel && col == o.col; }
  bool operator!=(const ColRef& o) const { return !(*this == o); }
  bool operator<(const ColRef& o) const { return rel != o.rel ? rel < o.rel : col < o.col; }
};

// The columns an operator emits, in order. Expressions are resolved against a layout to turn
// ColRefs into tuple positions.
using Layout = std::vector<ColRef>;

enum class ExprKind { Col, Const, Slot, Cmp, And, Or, Not, Arith, Neg };
enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge };
enum class ArithOp { Add, Sub, Mul, Div };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::Const;
  TypeId type = TypeId::Null;
  ColRef ref;         // Col
  Value value;        // Const
  CmpOp cmp = CmpOp::Eq;
  ArithOp arith = ArithOp::Add;
  std::vector<ExprPtr> kids;
  int slot = -1;      // tuple position (Col after resolution, Slot always)
  std::string text;   // display name for Col / Slot
};

inline const char* cmpSymbol(CmpOp op) {
  switch (op) {
    case CmpOp::Eq: return "=";
    case CmpOp::Ne: return "!=";
    case CmpOp::Lt: return "<";
    case CmpOp::Le: return "<=";
    case CmpOp::Gt: return ">";
    case CmpOp::Ge: return ">=";
  }
  return "?";
}

// a op b  <=>  b flip(op) a
inline CmpOp flipCmp(CmpOp op) {
  switch (op) {
    case CmpOp::Lt: return CmpOp::Gt;
    case CmpOp::Le: return CmpOp::Ge;
    case CmpOp::Gt: return CmpOp::Lt;
    case CmpOp::Ge: return CmpOp::Le;
    default: return op;
  }
}

inline ExprPtr makeCol(ColRef ref, TypeId type, std::string text) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Col;
  e->ref = ref;
  e->type = type;
  e->text = std::move(text);
  return e;
}
inline ExprPtr makeConst(Value v) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Const;
  e->type = v.type();
  e->value = std::move(v);
  return e;
}
inline ExprPtr makeSlot(int slot, TypeId type, std::string text) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Slot;
  e->slot = slot;
  e->type = type;
  e->text = std::move(text);
  return e;
}
inline ExprPtr makeCmp(CmpOp op, ExprPtr l, ExprPtr r) {
  auto e = std::make_shared<Expr>();
  e->kind = ExprKind::Cmp;
  e->cmp = op;
  e->type = TypeId::Int;
  e->kids = {std::move(l), std::move(r)};
  return e;
}
inline ExprPtr makeLogic(ExprKind kind, std::vector<ExprPtr> kids) {
  auto e = std::make_shared<Expr>();
  e->kind = kind;
  e->type = TypeId::Int;
  e->kids = std::move(kids);
  return e;
}

// Conjunction of predicates (nullptr when empty).
inline ExprPtr makeAnd(const std::vector<ExprPtr>& preds) {
  if (preds.empty()) return nullptr;
  if (preds.size() == 1) return preds[0];
  return makeLogic(ExprKind::And, preds);
}

Value eval(const Expr& e, const Tuple& t);
inline bool evalBool(const Expr& e, const Tuple& t) { return eval(e, t).truthy(); }
inline bool evalAll(const std::vector<ExprPtr>& preds, const Tuple& t) {
  for (const auto& p : preds)
    if (!evalBool(*p, t)) return false;
  return true;
}

// Matches `col op const` or `const op col`, normalizing so the column is on the left.
bool splitColConst(const Expr& e, ColRef* col, CmpOp* op, Value* v);

// Bitmask of relations referenced by Col nodes.
uint32_t relMask(const Expr& e);
void collectCols(const Expr& e, std::vector<ColRef>& out);

// Deep copy with every Col bound to its position in `layout`. Throws if a column is missing.
ExprPtr resolve(const ExprPtr& e, const Layout& layout);
std::vector<ExprPtr> resolveAll(const std::vector<ExprPtr>& es, const Layout& layout);

std::string exprToString(const Expr& e);

}  // namespace sysr
