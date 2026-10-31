#include "planner/binder.h"

#include <algorithm>
#include <functional>
#include <set>

namespace sysr {

namespace {

bool isAggName(const std::string& n) {
  return n == "count" || n == "sum" || n == "avg" || n == "min" || n == "max";
}

bool containsAgg(const AstPtr& a) {
  if (!a) return false;
  if (a->kind == AstExpr::Kind::Func && isAggName(a->name)) return true;
  for (const auto& k : a->args)
    if (containsAgg(k)) return true;
  return false;
}

bool comparable(TypeId a, TypeId b) {
  auto numeric = [](TypeId t) { return t == TypeId::Int || t == TypeId::Double; };
  return a == TypeId::Null || b == TypeId::Null || (numeric(a) && numeric(b)) || (a == b);
}

void splitConjuncts(const ExprPtr& e, std::vector<ExprPtr>& out) {
  if (e->kind == ExprKind::And) {
    for (const auto& k : e->kids) splitConjuncts(k, out);
  } else {
    out.push_back(e);
  }
}

class Binder {
 public:
  explicit Binder(const Catalog& cat) : cat_(cat) {}

  BoundQuery bind(const SelectStmt& s) {
    if (s.from.size() > kMaxRelations) throw DbError("at most 16 relations per query are supported");
    for (const TableRef& tr : s.from) {
      for (const Relation& r : q_.rels)
        if (r.alias == tr.alias) throw DbError("table name '" + tr.alias + "' specified more than once (use an alias)");
      Relation r;
      r.alias = tr.alias;
      r.table = &cat_.require(tr.table);
      q_.rels.push_back(std::move(r));
    }

    // Predicates: WHERE and ON conjuncts form one pool (inner joins only).
    std::vector<ExprPtr> conjuncts;
    for (const auto& cond : s.joinConditions) splitConjuncts(bindScalar(cond, "JOIN ... ON"), conjuncts);
    if (s.where) splitConjuncts(bindScalar(s.where, "WHERE"), conjuncts);
    for (const auto& c : conjuncts) classify(c);
    buildClasses();

    // Projection, aggregation, ordering.
    q_.hasAgg = !s.groupBy.empty();
    for (const auto& item : s.items) q_.hasAgg = q_.hasAgg || containsAgg(item.expr);
    for (const auto& o : s.orderBy) q_.hasAgg = q_.hasAgg || containsAgg(o.expr);
    if (s.selectStar && q_.hasAgg) throw DbError("SELECT * cannot be combined with GROUP BY or aggregates");

    if (s.selectStar) {
      for (size_t r = 0; r < q_.rels.size(); ++r) {
        const Schema& sc = q_.rels[r].table->schema;
        for (size_t c = 0; c < sc.size(); ++c) {
          ColRef ref{static_cast<int>(r), static_cast<int>(c)};
          q_.output.push_back(makeCol(ref, sc.cols[c].type, q_.colName(ref)));
          q_.outputNames.push_back(q_.colName(ref));
        }
      }
    } else if (q_.hasAgg) {
      for (const auto& g : s.groupBy) q_.groupKeys.push_back(bindScalar(g, "GROUP BY"));
      for (const auto& item : s.items) {
        q_.output.push_back(rewriteAgg(item.expr));
        q_.outputNames.push_back(item.alias.empty() ? item.expr->toString() : item.alias);
      }
    } else {
      for (const auto& item : s.items) {
        q_.output.push_back(bindScalar(item.expr, "SELECT"));
        q_.outputNames.push_back(item.alias.empty() ? item.expr->toString() : item.alias);
      }
    }

    for (const auto& o : s.orderBy) {
      SortKey key;
      key.desc = o.desc;
      key.expr = bindOrderItem(o.expr, s);
      q_.orderBy.push_back(key);
    }
    if (s.limit < -1) throw DbError("LIMIT must be non-negative");
    q_.limit = s.limit;

    // A single ascending ORDER BY column gets an equivalence class so plans can carry it as
    // an "interesting order".
    if (!q_.hasAgg && q_.orderBy.size() == 1 && !q_.orderBy[0].desc && q_.orderBy[0].expr->kind == ExprKind::Col)
      ensureClass(q_.orderBy[0].expr->ref);

    computeNeeded(s.selectStar);
    return std::move(q_);
  }

 private:
  ColRef resolveColumn(const AstExpr& a, TypeId* type) {
    if (!a.table.empty()) {
      for (size_t r = 0; r < q_.rels.size(); ++r) {
        if (q_.rels[r].alias != a.table) continue;
        int c = q_.rels[r].table->schema.indexOf(a.name);
        if (c < 0) throw DbError("column '" + a.name + "' does not exist in " + a.table);
        *type = q_.rels[r].table->schema.cols[static_cast<size_t>(c)].type;
        return ColRef{static_cast<int>(r), c};
      }
      throw DbError("unknown table or alias '" + a.table + "'");
    }
    ColRef found;
    for (size_t r = 0; r < q_.rels.size(); ++r) {
      int c = q_.rels[r].table->schema.indexOf(a.name);
      if (c < 0) continue;
      if (found.rel >= 0) throw DbError("column reference '" + a.name + "' is ambiguous");
      found = ColRef{static_cast<int>(r), c};
      *type = q_.rels[r].table->schema.cols[static_cast<size_t>(c)].type;
    }
    if (found.rel < 0) throw DbError("column '" + a.name + "' does not exist");
    return found;
  }

  ExprPtr bindScalar(const AstPtr& a, const char* clause) {
    return bindWith(a, [&](const AstPtr& k) { return bindScalar(k, clause); }, clause);
  }

  // Binds one AST node; children are bound through `sub` so the same code serves plain
  // expressions and post-aggregation rewriting.
  ExprPtr bindWith(const AstPtr& a, const std::function<ExprPtr(const AstPtr&)>& sub, const char* clause) {
    using K = AstExpr::Kind;
    switch (a->kind) {
      case K::Column: {
        TypeId t;
        ColRef ref = resolveColumn(*a, &t);
        return makeCol(ref, t, q_.colName(ref));
      }
      case K::Int: return makeConst(Value::Int(a->ival));
      case K::Double: return makeConst(Value::Double(a->dval));
      case K::Str: return makeConst(Value::String(a->sval));
      case K::Binary: {
        ExprPtr l = sub(a->args[0]), r = sub(a->args[1]);
        const std::string& op = a->op;
        if (op == "AND" || op == "OR") return makeLogic(op == "AND" ? ExprKind::And : ExprKind::Or, {l, r});
        if (op == "+" || op == "-" || op == "*" || op == "/") {
          auto numeric = [](TypeId t) { return t == TypeId::Int || t == TypeId::Double; };
          if (!numeric(l->type) || !numeric(r->type))
            throw DbError("operator " + op + " requires numeric operands in " + a->toString());
          auto e = std::make_shared<Expr>();
          e->kind = ExprKind::Arith;
          e->arith = op == "+" ? ArithOp::Add : op == "-" ? ArithOp::Sub : op == "*" ? ArithOp::Mul : ArithOp::Div;
          e->type = (l->type == TypeId::Int && r->type == TypeId::Int) ? TypeId::Int : TypeId::Double;
          e->kids = {l, r};
          return e;
        }
        if (!comparable(l->type, r->type))
          throw DbError(std::string("cannot compare ") + typeName(l->type) + " with " + typeName(r->type) + " in " +
                        a->toString());
        CmpOp cmp = op == "=" ? CmpOp::Eq : op == "!=" ? CmpOp::Ne : op == "<" ? CmpOp::Lt
                  : op == "<=" ? CmpOp::Le : op == ">" ? CmpOp::Gt : CmpOp::Ge;
        return makeCmp(cmp, l, r);
      }
      case K::Unary: {
        ExprPtr x = sub(a->args[0]);
        if (a->op == "NOT") return makeLogic(ExprKind::Not, {x});
        auto e = std::make_shared<Expr>();
        e->kind = ExprKind::Neg;
        e->type = x->type;
        e->kids = {x};
        return e;
      }
      case K::Between: {
        ExprPtr x = sub(a->args[0]), lo = sub(a->args[1]), hi = sub(a->args[2]);
        if (!comparable(x->type, lo->type) || !comparable(x->type, hi->type))
          throw DbError("type mismatch in " + a->toString());
        ExprPtr both = makeLogic(ExprKind::And, {makeCmp(CmpOp::Ge, x, lo), makeCmp(CmpOp::Le, x, hi)});
        return a->negated ? makeLogic(ExprKind::Not, {both}) : both;
      }
      case K::In: {
        ExprPtr x = sub(a->args[0]);
        std::vector<ExprPtr> alts;
        for (size_t i = 1; i < a->args.size(); ++i) {
          ExprPtr v = sub(a->args[i]);
          if (!comparable(x->type, v->type)) throw DbError("type mismatch in " + a->toString());
          alts.push_back(makeCmp(CmpOp::Eq, x, v));
        }
        ExprPtr any = alts.size() == 1 ? alts[0] : makeLogic(ExprKind::Or, alts);
        return a->negated ? makeLogic(ExprKind::Not, {any}) : any;
      }
      case K::Func:
        if (isAggName(a->name))
          throw DbError("aggregate " + a->toString() + " is not allowed in " + clause);
        throw DbError("unknown function '" + a->name + "'");
    }
    throw DbError("unsupported expression");
  }

  ExprPtr aggSlot(const AstPtr& a) {
    AggSpec spec;
    if (a->star) {
      if (a->name != "count") throw DbError(a->name + "(*) is not valid");
      spec.fn = AggFn::CountStar;
      spec.type = TypeId::Int;
      spec.text = "count(*)";
    } else {
      if (a->args.size() != 1) throw DbError(a->name + "() takes exactly one argument");
      if (containsAgg(a->args[0])) throw DbError("nested aggregates are not allowed");
      spec.arg = bindScalar(a->args[0], "aggregate argument");
      TypeId t = spec.arg->type;
      bool numeric = t == TypeId::Int || t == TypeId::Double;
      if (a->name == "count") { spec.fn = AggFn::Count; spec.type = TypeId::Int; }
      else if (a->name == "sum") { spec.fn = AggFn::Sum; spec.type = t; }
      else if (a->name == "avg") { spec.fn = AggFn::Avg; spec.type = TypeId::Double; }
      else if (a->name == "min") { spec.fn = AggFn::Min; spec.type = t; }
      else { spec.fn = AggFn::Max; spec.type = t; }
      if ((spec.fn == AggFn::Sum || spec.fn == AggFn::Avg) && !numeric)
        throw DbError(a->name + "() requires a numeric argument");
      spec.text = a->name + "(" + exprToString(*spec.arg) + ")";
    }
    size_t base = q_.groupKeys.size();
    for (size_t i = 0; i < q_.aggs.size(); ++i)
      if (q_.aggs[i].text == spec.text) return makeSlot(static_cast<int>(base + i), spec.type, spec.text);
    q_.aggs.push_back(spec);
    return makeSlot(static_cast<int>(base + q_.aggs.size() - 1), spec.type, spec.text);
  }

  // Rewrites a SELECT / ORDER BY expression of an aggregate query over the aggregate output.
  ExprPtr rewriteAgg(const AstPtr& a) {
    if (a->kind == AstExpr::Kind::Func && isAggName(a->name)) return aggSlot(a);
    if (!containsAgg(a)) {
      ExprPtr b = bindScalar(a, "SELECT");
      std::string s = exprToString(*b);
      for (size_t i = 0; i < q_.groupKeys.size(); ++i)
        if (exprToString(*q_.groupKeys[i]) == s) return makeSlot(static_cast<int>(i), b->type, s);
      if (b->kind == ExprKind::Const) return b;
      if (a->kind == AstExpr::Kind::Column)
        throw DbError("column " + s + " must appear in the GROUP BY clause or be used in an aggregate function");
    }
    return bindWith(a, [&](const AstPtr& k) { return rewriteAgg(k); }, "SELECT");
  }

  ExprPtr bindOrderItem(const AstPtr& a, const SelectStmt& s) {
    if (a->kind == AstExpr::Kind::Int) {
      if (a->ival < 1 || static_cast<size_t>(a->ival) > q_.output.size())
        throw DbError("ORDER BY position " + std::to_string(a->ival) + " is not in the select list");
      return q_.output[static_cast<size_t>(a->ival - 1)];
    }
    if (a->kind == AstExpr::Kind::Column && a->table.empty()) {
      for (size_t i = 0; i < s.items.size(); ++i)
        if (s.items[i].alias == a->name) return q_.output[i];
    }
    return q_.hasAgg ? rewriteAgg(a) : bindScalar(a, "ORDER BY");
  }

  void classify(const ExprPtr& e) {
    uint32_t mask = relMask(*e);
    if (mask == 0) {
      if (!eval(*e, Tuple{}).truthy()) q_.alwaysFalse = true;
      return;
    }
    if (e->kind == ExprKind::Cmp && e->cmp == CmpOp::Eq) {
      const ExprPtr& l = e->kids[0];
      const ExprPtr& r = e->kids[1];
      if (l->kind == ExprKind::Col && r->kind == ExprKind::Col) {
        if (l->ref != r->ref) unite(l->ref, r->ref);
        return;  // a.x = a.x is always true for non-null data
      }
      if (l->kind == ExprKind::Col && r->kind == ExprKind::Const) constBinding_[l->ref].push_back(r->value);
      if (r->kind == ExprKind::Col && l->kind == ExprKind::Const) constBinding_[r->ref].push_back(l->value);
    }
    {
      ColRef c;
      CmpOp op;
      Value v;
      if (splitColConst(*e, &c, &op, &v) && v.isNumeric() &&
          (op == CmpOp::Lt || op == CmpOp::Le || op == CmpOp::Gt || op == CmpOp::Ge))
        rangeBinding_[c].push_back({op, v});
    }
    int bits = 0;
    int only = 0;
    for (int i = 0; i < 32; ++i)
      if (mask & (1u << i)) { ++bits; only = i; }
    if (bits == 1) addFilter(only, e, false, false);
    else q_.complex.push_back(ComplexPred{e, mask});
  }

  void addFilter(int rel, ExprPtr e, bool derived, bool inClass) {
    Relation& r = q_.rels[static_cast<size_t>(rel)];
    r.filters.push_back(std::move(e));
    r.filterDerived.push_back(derived);
    r.filterInClass.push_back(inClass);
  }

  int ufId(ColRef c) {
    auto it = ufIndex_.find(c);
    if (it != ufIndex_.end()) return it->second;
    int id = static_cast<int>(ufRefs_.size());
    ufIndex_[c] = id;
    ufRefs_.push_back(c);
    parent_.push_back(id);
    return id;
  }
  int find(int x) {
    while (parent_[static_cast<size_t>(x)] != x) {
      parent_[static_cast<size_t>(x)] = parent_[static_cast<size_t>(parent_[static_cast<size_t>(x)])];
      x = parent_[static_cast<size_t>(x)];
    }
    return x;
  }
  void unite(ColRef a, ColRef b) {
    int x = find(ufId(a)), y = find(ufId(b));
    if (x != y) parent_[static_cast<size_t>(x)] = y;
  }

  void buildClasses() {
    std::map<int, std::vector<ColRef>> groups;
    for (size_t i = 0; i < ufRefs_.size(); ++i) groups[find(static_cast<int>(i))].push_back(ufRefs_[i]);
    for (auto& kv : groups) {
      EquivClass cls;
      cls.members = kv.second;
      std::sort(cls.members.begin(), cls.members.end());
      for (const ColRef& m : cls.members) cls.relMask |= 1u << m.rel;
      // Constant propagation: x = 5 and x = y imply y = 5.
      for (const ColRef& m : cls.members) {
        auto it = constBinding_.find(m);
        if (it == constBinding_.end()) continue;
        for (const Value& v : it->second) {
          if (!cls.hasConst) { cls.hasConst = true; cls.constant = v; }
          else if (cls.constant.compare(v) != 0) q_.alwaysFalse = true;
        }
      }
      int id = static_cast<int>(q_.classes.size());
      for (const ColRef& m : cls.members) {
        q_.classOfCol[m] = id;
        if (cls.hasConst && !constBinding_.count(m))
          addFilter(m.rel, makeCmp(CmpOp::Eq, makeCol(m, q_.colType(m), q_.colName(m)), makeConst(cls.constant)), true, false);
      }
      // Range propagation: x < 50 and x = y imply y < 50, so the other side is filtered before
      // the join and estimated with its own histogram.
      if (!cls.hasConst) {
        for (const ColRef& from : cls.members) {
          auto it = rangeBinding_.find(from);
          if (it == rangeBinding_.end()) continue;
          for (const ColRef& to : cls.members) {
            if (to == from) continue;
            for (const auto& rb : it->second) {
              bool present = false;
              auto own = rangeBinding_.find(to);
              if (own != rangeBinding_.end())
                for (const auto& x : own->second) present = present || (x.first == rb.first && x.second.compare(rb.second) == 0);
              if (present) continue;
              addFilter(to.rel, makeCmp(rb.first, makeCol(to, q_.colType(to), q_.colName(to)), makeConst(rb.second)), true, false);
              rangeBinding_[to].push_back(rb);
            }
          }
        }
      }
      // Equalities between two columns of the same relation are checked during its scan.
      for (size_t i = 0; i < cls.members.size(); ++i) {
        for (size_t j = i + 1; j < cls.members.size(); ++j) {
          const ColRef& a = cls.members[i];
          const ColRef& b = cls.members[j];
          if (a.rel != b.rel) continue;
          bool first = true;
          for (size_t k = 0; k < i; ++k)
            if (cls.members[k].rel == a.rel) first = false;
          if (!first) continue;  // chain every same-relation member to the first one
          addFilter(a.rel, makeCmp(CmpOp::Eq, makeCol(a, q_.colType(a), q_.colName(a)), makeCol(b, q_.colType(b), q_.colName(b))),
                    false, true);
        }
      }
      q_.classes.push_back(std::move(cls));
    }
  }

  void ensureClass(ColRef c) {
    if (q_.classOf(c) >= 0) return;
    EquivClass cls;
    cls.members = {c};
    cls.relMask = 1u << c.rel;
    q_.classOfCol[c] = static_cast<int>(q_.classes.size());
    q_.classes.push_back(std::move(cls));
  }

  void computeNeeded(bool star) {
    std::vector<std::set<int>> need(q_.rels.size());
    if (star) {
      for (size_t r = 0; r < q_.rels.size(); ++r)
        for (size_t c = 0; c < q_.rels[r].table->schema.size(); ++c) need[r].insert(static_cast<int>(c));
    }
    std::vector<ColRef> refs;
    for (const EquivClass& cls : q_.classes) {
      bool multi = (cls.relMask & (cls.relMask - 1)) != 0;
      if (multi) refs.insert(refs.end(), cls.members.begin(), cls.members.end());
    }
    for (const ComplexPred& p : q_.complex) collectCols(*p.expr, refs);
    if (q_.hasAgg) {
      for (const auto& g : q_.groupKeys) collectCols(*g, refs);
      for (const auto& a : q_.aggs)
        if (a.arg) collectCols(*a.arg, refs);
    } else {
      for (const auto& o : q_.output) collectCols(*o, refs);
      for (const auto& k : q_.orderBy) collectCols(*k.expr, refs);
    }
    for (const ColRef& c : refs) need[static_cast<size_t>(c.rel)].insert(c.col);
    for (size_t r = 0; r < q_.rels.size(); ++r) q_.rels[r].needed.assign(need[r].begin(), need[r].end());
  }

  const Catalog& cat_;
  BoundQuery q_;
  std::map<ColRef, int> ufIndex_;
  std::vector<ColRef> ufRefs_;
  std::vector<int> parent_;
  std::map<ColRef, std::vector<Value>> constBinding_;
  std::map<ColRef, std::vector<std::pair<CmpOp, Value>>> rangeBinding_;
};

}  // namespace

BoundQuery bindSelect(const SelectStmt& stmt, const Catalog& catalog) {
  Binder b(catalog);
  return b.bind(stmt);
}

}  // namespace sysr
