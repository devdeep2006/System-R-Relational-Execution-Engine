#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/value.h"

namespace sysr {

struct AstExpr;
using AstPtr = std::shared_ptr<AstExpr>;

struct AstExpr {
  enum class Kind { Column, Int, Double, Str, Binary, Unary, Between, In, Func };
  Kind kind = Kind::Column;
  std::string table, name;  // Column: [table.]name; Func: lower-case function name
  std::string op;           // Binary: = != < <= > >= AND OR + - * / ; Unary: NOT, -
  int64_t ival = 0;
  double dval = 0;
  std::string sval;
  std::vector<AstPtr> args;  // Binary [l, r]; Unary [x]; Between [x, lo, hi]; In [x, items...]; Func args
  bool negated = false;      // NOT BETWEEN / NOT IN
  bool star = false;         // COUNT(*)

  std::string toString() const {
    switch (kind) {
      case Kind::Column: return table.empty() ? name : table + "." + name;
      case Kind::Int: return std::to_string(ival);
      case Kind::Double: return Value::Double(dval).toString();
      case Kind::Str: return "'" + sval + "'";
      case Kind::Binary: return "(" + args[0]->toString() + " " + op + " " + args[1]->toString() + ")";
      case Kind::Unary: return op == "NOT" ? "NOT " + args[0]->toString() : "-" + args[0]->toString();
      case Kind::Between:
        return args[0]->toString() + (negated ? " NOT" : "") + " BETWEEN " + args[1]->toString() + " AND " +
               args[2]->toString();
      case Kind::In: {
        std::string s = args[0]->toString() + (negated ? " NOT IN (" : " IN (");
        for (size_t i = 1; i < args.size(); ++i) s += (i > 1 ? ", " : "") + args[i]->toString();
        return s + ")";
      }
      case Kind::Func: {
        if (star) return name + "(*)";
        std::string s = name + "(";
        for (size_t i = 0; i < args.size(); ++i) s += (i ? ", " : "") + args[i]->toString();
        return s + ")";
      }
    }
    return "?";
  }
};

struct SelectItem {
  AstPtr expr;
  std::string alias;
};

struct TableRef {
  std::string table, alias;
};

struct OrderItem {
  AstPtr expr;
  bool desc = false;
};

struct SelectStmt {
  bool selectStar = false;
  std::vector<SelectItem> items;
  std::vector<TableRef> from;
  std::vector<AstPtr> joinConditions;  // ON clauses; inner joins only, so they join the WHERE conjuncts
  AstPtr where;
  std::vector<AstPtr> groupBy;
  std::vector<OrderItem> orderBy;
  int64_t limit = -1;
};

struct ColumnDef {
  std::string name;
  TypeId type;
};

struct Statement {
  enum class Kind { Select, Explain, CreateTable, CreateIndex, Insert, Analyze, Set, ShowTables, ShowStats };
  Kind kind = Kind::Select;
  bool explainAnalyze = false;
  std::shared_ptr<SelectStmt> select;
  std::string table;               // CREATE TABLE / INDEX, INSERT, ANALYZE, SHOW STATS
  std::vector<ColumnDef> columns;  // CREATE TABLE
  std::string column, indexName;   // CREATE INDEX
  std::vector<std::vector<AstPtr>> rows;  // INSERT
  std::string setName, setValue;   // SET
  std::string text;                // original statement text
};

}  // namespace sysr
