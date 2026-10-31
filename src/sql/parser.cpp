#include "sql/parser.h"

#include <cctype>
#include <set>

namespace sysr {

namespace {

const std::set<std::string>& keywords() {
  static const std::set<std::string> kw = {
      "SELECT", "FROM",   "WHERE",  "JOIN",   "INNER",  "CROSS",  "LEFT",    "RIGHT",   "FULL",  "OUTER",
      "ON",     "AND",    "OR",     "NOT",    "AS",     "GROUP",  "BY",      "ORDER",   "ASC",   "DESC",
      "LIMIT",  "BETWEEN", "IN",    "EXPLAIN", "ANALYZE", "CREATE", "TABLE", "INDEX",   "INSERT", "INTO",
      "VALUES", "SET",    "TO",     "SHOW",   "TABLES", "STATS",  "TRUE",    "FALSE",   "INT",   "INTEGER",
      "BIGINT", "DOUBLE", "FLOAT",  "REAL",   "TEXT",   "VARCHAR", "STRING"};
  return kw;
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::vector<Token> tokenize(const std::string& sql) {
  std::vector<Token> out;
  size_t i = 0;
  const size_t n = sql.size();
  while (i < n) {
    char c = sql[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
      while (i < n && sql[i] != '\n') ++i;
      continue;
    }
    Token t;
    t.pos = i;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i;
      while (j < n && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) ++j;
      std::string word = sql.substr(i, j - i);
      std::string up = upper(word);
      if (keywords().count(up)) { t.type = TokType::Keyword; t.text = up; }
      else { t.type = TokType::Ident; t.text = lower(word); }
      i = j;
    } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      size_t j = i;
      bool isFloat = false;
      while (j < n && (std::isdigit(static_cast<unsigned char>(sql[j])) || sql[j] == '.')) {
        if (sql[j] == '.') {
          if (isFloat) break;
          isFloat = true;
        }
        ++j;
      }
      if (j < n && (sql[j] == 'e' || sql[j] == 'E')) {
        size_t k = j + 1;
        if (k < n && (sql[k] == '+' || sql[k] == '-')) ++k;
        if (k < n && std::isdigit(static_cast<unsigned char>(sql[k]))) {
          isFloat = true;
          j = k;
          while (j < n && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
        }
      }
      t.text = sql.substr(i, j - i);
      if (isFloat) { t.type = TokType::Float; t.dval = std::stod(t.text); }
      else {
        t.type = TokType::Int;
        try { t.ival = std::stoll(t.text); } catch (...) { throw DbError("integer literal out of range: " + t.text); }
      }
      i = j;
    } else if (c == '\'') {
      std::string s;
      size_t j = i + 1;
      while (true) {
        if (j >= n) throw DbError("unterminated string literal at offset " + std::to_string(i));
        if (sql[j] == '\'') {
          if (j + 1 < n && sql[j + 1] == '\'') { s.push_back('\''); j += 2; continue; }
          break;
        }
        s.push_back(sql[j++]);
      }
      t.type = TokType::String;
      t.text = s;
      i = j + 1;
    } else {
      static const char* two[] = {"<=", ">=", "<>", "!="};
      t.type = TokType::Symbol;
      bool matched = false;
      for (const char* op : two) {
        if (i + 1 < n && sql[i] == op[0] && sql[i + 1] == op[1]) {
          t.text = op;
          if (t.text == "<>") t.text = "!=";
          i += 2;
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (std::string("(),.;*+-/=<>").find(c) == std::string::npos)
          throw DbError(std::string("unexpected character '") + c + "' at offset " + std::to_string(i));
        t.text = std::string(1, c);
        ++i;
      }
    }
    out.push_back(std::move(t));
  }
  Token end;
  end.type = TokType::End;
  end.pos = n;
  out.push_back(end);
  return out;
}

namespace {

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

  bool atEnd() const { return peek().type == TokType::End; }
  bool acceptSymbol(const char* s) {
    if (peek().type == TokType::Symbol && peek().text == s) { ++pos_; return true; }
    return false;
  }
  size_t position() const { return peek().pos; }

  Statement statement() {
    Statement st;
    if (acceptKw("EXPLAIN")) {
      st.kind = Statement::Kind::Explain;
      st.explainAnalyze = acceptKw("ANALYZE");
      expectKwPeek("SELECT");
      st.select = select();
      return st;
    }
    if (peekKw("SELECT")) {
      st.kind = Statement::Kind::Select;
      st.select = select();
      return st;
    }
    if (acceptKw("CREATE")) {
      if (acceptKw("TABLE")) {
        st.kind = Statement::Kind::CreateTable;
        st.table = ident("table name");
        expectSymbol("(");
        do {
          ColumnDef cd;
          cd.name = ident("column name");
          cd.type = columnType();
          st.columns.push_back(cd);
        } while (acceptSymbol(","));
        expectSymbol(")");
        return st;
      }
      if (acceptKw("INDEX")) {
        st.kind = Statement::Kind::CreateIndex;
        if (peek().type == TokType::Ident) st.indexName = ident("index name");
        expectKw("ON");
        st.table = ident("table name");
        expectSymbol("(");
        st.column = ident("column name");
        expectSymbol(")");
        return st;
      }
      fail("expected TABLE or INDEX after CREATE");
    }
    if (acceptKw("INSERT")) {
      st.kind = Statement::Kind::Insert;
      expectKw("INTO");
      st.table = ident("table name");
      expectKw("VALUES");
      do {
        expectSymbol("(");
        std::vector<AstPtr> row;
        do { row.push_back(expr()); } while (acceptSymbol(","));
        expectSymbol(")");
        st.rows.push_back(std::move(row));
      } while (acceptSymbol(","));
      return st;
    }
    if (acceptKw("ANALYZE")) {
      st.kind = Statement::Kind::Analyze;
      if (peek().type == TokType::Ident) st.table = ident("table name");
      return st;
    }
    if (acceptKw("SET")) {
      st.kind = Statement::Kind::Set;
      st.setName = ident("setting name");
      if (!acceptSymbol("=")) expectKw("TO");
      const Token& v = peek();
      if (v.type == TokType::End || (v.type == TokType::Symbol && v.text == ";")) fail("expected a value");
      st.setValue = v.type == TokType::Keyword ? lower(v.text) : v.text;
      ++pos_;
      return st;
    }
    if (acceptKw("SHOW")) {
      if (acceptKw("TABLES")) { st.kind = Statement::Kind::ShowTables; return st; }
      if (acceptKw("STATS")) {
        st.kind = Statement::Kind::ShowStats;
        st.table = ident("table name");
        return st;
      }
      fail("expected TABLES or STATS after SHOW");
    }
    fail("expected a statement");
    return st;
  }

 private:
  const Token& peek(size_t k = 0) const {
    size_t i = std::min(pos_ + k, toks_.size() - 1);
    return toks_[i];
  }
  bool peekKw(const char* kw) const { return peek().type == TokType::Keyword && peek().text == kw; }
  bool acceptKw(const char* kw) {
    if (peekKw(kw)) { ++pos_; return true; }
    return false;
  }
  void expectKw(const char* kw) {
    if (!acceptKw(kw)) fail(std::string("expected ") + kw);
  }
  void expectKwPeek(const char* kw) {
    if (!peekKw(kw)) fail(std::string("expected ") + kw);
  }
  void expectSymbol(const char* s) {
    if (!acceptSymbol(s)) fail(std::string("expected '") + s + "'");
  }
  [[noreturn]] void fail(const std::string& msg) const {
    const Token& t = peek();
    std::string near = t.type == TokType::End ? "end of input" : "'" + t.text + "'";
    throw DbError("syntax error: " + msg + " near " + near + " (offset " + std::to_string(t.pos) + ")");
  }
  std::string ident(const char* what) {
    if (peek().type != TokType::Ident) fail(std::string("expected ") + what);
    return toks_[pos_++].text;
  }

  TypeId columnType() {
    const Token& t = peek();
    if (t.type == TokType::Keyword) {
      if (t.text == "INT" || t.text == "INTEGER" || t.text == "BIGINT") { ++pos_; return TypeId::Int; }
      if (t.text == "DOUBLE" || t.text == "FLOAT" || t.text == "REAL") { ++pos_; return TypeId::Double; }
      if (t.text == "TEXT" || t.text == "STRING") { ++pos_; return TypeId::String; }
      if (t.text == "VARCHAR") {
        ++pos_;
        if (acceptSymbol("(")) {
          if (peek().type != TokType::Int) fail("expected length");
          ++pos_;
          expectSymbol(")");
        }
        return TypeId::String;
      }
    }
    fail("expected a column type (INT, DOUBLE, TEXT)");
  }

  std::shared_ptr<SelectStmt> select() {
    expectKw("SELECT");
    auto s = std::make_shared<SelectStmt>();
    if (acceptSymbol("*")) {
      s->selectStar = true;
    } else {
      do {
        SelectItem item;
        item.expr = expr();
        if (acceptKw("AS")) item.alias = ident("alias");
        else if (peek().type == TokType::Ident) item.alias = ident("alias");
        s->items.push_back(std::move(item));
      } while (acceptSymbol(","));
    }
    expectKw("FROM");
    s->from.push_back(tableRef());
    while (true) {
      if (acceptSymbol(",")) { s->from.push_back(tableRef()); continue; }
      if (acceptKw("CROSS")) {
        expectKw("JOIN");
        s->from.push_back(tableRef());
        continue;
      }
      if (peekKw("LEFT") || peekKw("RIGHT") || peekKw("FULL") || peekKw("OUTER"))
        fail("outer joins are not supported (inner joins only)");
      bool inner = acceptKw("INNER");
      if (acceptKw("JOIN")) {
        s->from.push_back(tableRef());
        expectKw("ON");
        s->joinConditions.push_back(expr());
        continue;
      }
      if (inner) fail("expected JOIN after INNER");
      break;
    }
    if (acceptKw("WHERE")) s->where = expr();
    if (acceptKw("GROUP")) {
      expectKw("BY");
      do { s->groupBy.push_back(expr()); } while (acceptSymbol(","));
    }
    if (acceptKw("ORDER")) {
      expectKw("BY");
      do {
        OrderItem o;
        o.expr = expr();
        if (acceptKw("DESC")) o.desc = true;
        else acceptKw("ASC");
        s->orderBy.push_back(std::move(o));
      } while (acceptSymbol(","));
    }
    if (acceptKw("LIMIT")) {
      if (peek().type != TokType::Int) fail("expected an integer after LIMIT");
      s->limit = toks_[pos_++].ival;
    }
    return s;
  }

  TableRef tableRef() {
    TableRef r;
    r.table = ident("table name");
    if (acceptKw("AS")) r.alias = ident("alias");
    else if (peek().type == TokType::Ident) r.alias = ident("alias");
    if (r.alias.empty()) r.alias = r.table;
    return r;
  }

  static AstPtr binary(const std::string& op, AstPtr l, AstPtr r) {
    auto e = std::make_shared<AstExpr>();
    e->kind = AstExpr::Kind::Binary;
    e->op = op;
    e->args = {std::move(l), std::move(r)};
    return e;
  }

  AstPtr expr() { return orExpr(); }

  AstPtr orExpr() {
    AstPtr l = andExpr();
    while (acceptKw("OR")) l = binary("OR", l, andExpr());
    return l;
  }
  AstPtr andExpr() {
    AstPtr l = notExpr();
    while (acceptKw("AND")) l = binary("AND", l, notExpr());
    return l;
  }
  AstPtr notExpr() {
    if (acceptKw("NOT")) {
      auto e = std::make_shared<AstExpr>();
      e->kind = AstExpr::Kind::Unary;
      e->op = "NOT";
      e->args = {notExpr()};
      return e;
    }
    return predicate();
  }
  AstPtr predicate() {
    AstPtr l = additive();
    if (peek().type == TokType::Symbol) {
      const std::string& s = peek().text;
      if (s == "=" || s == "!=" || s == "<" || s == "<=" || s == ">" || s == ">=") {
        std::string op = s;
        ++pos_;
        return binary(op, l, additive());
      }
    }
    bool negated = false;
    if (peekKw("NOT") && (peek(1).type == TokType::Keyword && (peek(1).text == "BETWEEN" || peek(1).text == "IN"))) {
      ++pos_;
      negated = true;
    }
    if (acceptKw("BETWEEN")) {
      auto e = std::make_shared<AstExpr>();
      e->kind = AstExpr::Kind::Between;
      e->negated = negated;
      AstPtr lo = additive();
      expectKw("AND");
      AstPtr hi = additive();
      e->args = {l, lo, hi};
      return e;
    }
    if (acceptKw("IN")) {
      auto e = std::make_shared<AstExpr>();
      e->kind = AstExpr::Kind::In;
      e->negated = negated;
      e->args.push_back(l);
      expectSymbol("(");
      do { e->args.push_back(additive()); } while (acceptSymbol(","));
      expectSymbol(")");
      return e;
    }
    if (negated) fail("expected BETWEEN or IN");
    return l;
  }
  AstPtr additive() {
    AstPtr l = multiplicative();
    while (true) {
      if (acceptSymbol("+")) l = binary("+", l, multiplicative());
      else if (acceptSymbol("-")) l = binary("-", l, multiplicative());
      else return l;
    }
  }
  AstPtr multiplicative() {
    AstPtr l = unary();
    while (true) {
      if (acceptSymbol("*")) l = binary("*", l, unary());
      else if (acceptSymbol("/")) l = binary("/", l, unary());
      else return l;
    }
  }
  AstPtr unary() {
    if (acceptSymbol("-")) {
      AstPtr x = unary();
      if (x->kind == AstExpr::Kind::Int) { x->ival = -x->ival; return x; }
      if (x->kind == AstExpr::Kind::Double) { x->dval = -x->dval; return x; }
      auto e = std::make_shared<AstExpr>();
      e->kind = AstExpr::Kind::Unary;
      e->op = "-";
      e->args = {x};
      return e;
    }
    return primary();
  }
  AstPtr primary() {
    const Token& t = peek();
    auto e = std::make_shared<AstExpr>();
    switch (t.type) {
      case TokType::Int: e->kind = AstExpr::Kind::Int; e->ival = t.ival; ++pos_; return e;
      case TokType::Float: e->kind = AstExpr::Kind::Double; e->dval = t.dval; ++pos_; return e;
      case TokType::String: e->kind = AstExpr::Kind::Str; e->sval = t.text; ++pos_; return e;
      case TokType::Keyword:
        if (t.text == "TRUE" || t.text == "FALSE") {
          e->kind = AstExpr::Kind::Int;
          e->ival = t.text == "TRUE";
          ++pos_;
          return e;
        }
        break;
      case TokType::Symbol:
        if (t.text == "(") {
          ++pos_;
          AstPtr inner = expr();
          expectSymbol(")");
          return inner;
        }
        break;
      case TokType::Ident: {
        std::string first = t.text;
        ++pos_;
        if (acceptSymbol("(")) {
          e->kind = AstExpr::Kind::Func;
          e->name = first;
          if (acceptSymbol("*")) e->star = true;
          else if (!(peek().type == TokType::Symbol && peek().text == ")")) {
            do { e->args.push_back(expr()); } while (acceptSymbol(","));
          }
          expectSymbol(")");
          return e;
        }
        e->kind = AstExpr::Kind::Column;
        if (acceptSymbol(".")) {
          e->table = first;
          e->name = ident("column name");
        } else {
          e->name = first;
        }
        return e;
      }
      default: break;
    }
    fail("expected an expression");
  }

  std::vector<Token> toks_;
  size_t pos_ = 0;
};

}  // namespace

std::vector<Statement> parseScript(const std::string& sql) {
  Parser p(tokenize(sql));
  std::vector<Statement> out;
  while (true) {
    while (p.acceptSymbol(";")) {}
    if (p.atEnd()) break;
    size_t start = p.position();
    Statement st = p.statement();
    size_t end = p.position();
    st.text = sql.substr(start, end - start);
    while (!st.text.empty() && std::isspace(static_cast<unsigned char>(st.text.back()))) st.text.pop_back();
    out.push_back(std::move(st));
    if (!p.acceptSymbol(";") && !p.atEnd()) {
      throw DbError("syntax error: expected ';' between statements (offset " + std::to_string(p.position()) + ")");
    }
  }
  return out;
}

Statement parseStatement(const std::string& sql) {
  std::vector<Statement> all = parseScript(sql);
  if (all.size() != 1) throw DbError("expected exactly one statement, got " + std::to_string(all.size()));
  return std::move(all[0]);
}

}  // namespace sysr
