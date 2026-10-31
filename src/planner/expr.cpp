#include "planner/expr.h"

namespace sysr {

Value eval(const Expr& e, const Tuple& t) {
  switch (e.kind) {
    case ExprKind::Col:
    case ExprKind::Slot:
      return t[static_cast<size_t>(e.slot)];
    case ExprKind::Const:
      return e.value;
    case ExprKind::Cmp: {
      Value l = eval(*e.kids[0], t), r = eval(*e.kids[1], t);
      if (l.isNull() || r.isNull()) return Value();
      int c = l.compare(r);
      bool res = false;
      switch (e.cmp) {
        case CmpOp::Eq: res = c == 0; break;
        case CmpOp::Ne: res = c != 0; break;
        case CmpOp::Lt: res = c < 0; break;
        case CmpOp::Le: res = c <= 0; break;
        case CmpOp::Gt: res = c > 0; break;
        case CmpOp::Ge: res = c >= 0; break;
      }
      return Value::Int(res);
    }
    case ExprKind::And:
      for (const auto& k : e.kids)
        if (!eval(*k, t).truthy()) return Value::Int(0);
      return Value::Int(1);
    case ExprKind::Or:
      for (const auto& k : e.kids)
        if (eval(*k, t).truthy()) return Value::Int(1);
      return Value::Int(0);
    case ExprKind::Not:
      return Value::Int(!eval(*e.kids[0], t).truthy());
    case ExprKind::Neg: {
      Value v = eval(*e.kids[0], t);
      if (v.type() == TypeId::Int) return Value::Int(-v.asInt());
      if (v.type() == TypeId::Double) return Value::Double(-v.asDouble());
      return Value();
    }
    case ExprKind::Arith: {
      Value l = eval(*e.kids[0], t), r = eval(*e.kids[1], t);
      if (!l.isNumeric() || !r.isNumeric()) return Value();
      if (l.type() == TypeId::Int && r.type() == TypeId::Int) {
        int64_t a = l.asInt(), b = r.asInt();
        switch (e.arith) {
          case ArithOp::Add: return Value::Int(a + b);
          case ArithOp::Sub: return Value::Int(a - b);
          case ArithOp::Mul: return Value::Int(a * b);
          case ArithOp::Div: return b == 0 ? Value() : Value::Int(a / b);
        }
      }
      double a = l.asDouble(), b = r.asDouble();
      switch (e.arith) {
        case ArithOp::Add: return Value::Double(a + b);
        case ArithOp::Sub: return Value::Double(a - b);
        case ArithOp::Mul: return Value::Double(a * b);
        case ArithOp::Div: return b == 0 ? Value() : Value::Double(a / b);
      }
      return Value();
    }
  }
  return Value();
}

bool splitColConst(const Expr& e, ColRef* col, CmpOp* op, Value* v) {
  if (e.kind != ExprKind::Cmp) return false;
  const Expr& l = *e.kids[0];
  const Expr& r = *e.kids[1];
  if (l.kind == ExprKind::Col && r.kind == ExprKind::Const) { *col = l.ref; *op = e.cmp; *v = r.value; return true; }
  if (r.kind == ExprKind::Col && l.kind == ExprKind::Const) { *col = r.ref; *op = flipCmp(e.cmp); *v = l.value; return true; }
  return false;
}

uint32_t relMask(const Expr& e) {
  uint32_t m = e.kind == ExprKind::Col ? (1u << e.ref.rel) : 0u;
  for (const auto& k : e.kids) m |= relMask(*k);
  return m;
}

void collectCols(const Expr& e, std::vector<ColRef>& out) {
  if (e.kind == ExprKind::Col) out.push_back(e.ref);
  for (const auto& k : e.kids) collectCols(*k, out);
}

ExprPtr resolve(const ExprPtr& e, const Layout& layout) {
  if (!e) return nullptr;
  auto copy = std::make_shared<Expr>(*e);
  if (copy->kind == ExprKind::Col) {
    copy->slot = -1;
    for (size_t i = 0; i < layout.size(); ++i)
      if (layout[i] == copy->ref) { copy->slot = static_cast<int>(i); break; }
    if (copy->slot < 0) throw DbError("internal: column " + copy->text + " is not available in operator input");
  }
  for (auto& k : copy->kids) k = resolve(k, layout);
  return copy;
}

std::vector<ExprPtr> resolveAll(const std::vector<ExprPtr>& es, const Layout& layout) {
  std::vector<ExprPtr> out;
  out.reserve(es.size());
  for (const auto& e : es) out.push_back(resolve(e, layout));
  return out;
}

std::string exprToString(const Expr& e) {
  switch (e.kind) {
    case ExprKind::Col:
    case ExprKind::Slot:
      return e.text;
    case ExprKind::Const:
      return e.value.type() == TypeId::String ? "'" + e.value.asString() + "'" : e.value.toString();
    case ExprKind::Cmp:
      return exprToString(*e.kids[0]) + " " + cmpSymbol(e.cmp) + " " + exprToString(*e.kids[1]);
    case ExprKind::And:
    case ExprKind::Or: {
      std::string s = "(";
      for (size_t i = 0; i < e.kids.size(); ++i) {
        if (i) s += e.kind == ExprKind::And ? " AND " : " OR ";
        s += exprToString(*e.kids[i]);
      }
      return s + ")";
    }
    case ExprKind::Not:
      return "NOT (" + exprToString(*e.kids[0]) + ")";
    case ExprKind::Neg:
      return "-" + exprToString(*e.kids[0]);
    case ExprKind::Arith: {
      const char* op = e.arith == ArithOp::Add ? " + " : e.arith == ArithOp::Sub ? " - " : e.arith == ArithOp::Mul ? " * " : " / ";
      return "(" + exprToString(*e.kids[0]) + op + exprToString(*e.kids[1]) + ")";
    }
  }
  return "?";
}

}  // namespace sysr
