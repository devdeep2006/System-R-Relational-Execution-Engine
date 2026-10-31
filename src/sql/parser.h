#pragma once

#include <string>
#include <vector>

#include "sql/ast.h"

namespace sysr {

enum class TokType { Ident, Keyword, Int, Float, String, Symbol, End };

struct Token {
  TokType type = TokType::End;
  std::string text;  // keywords upper-case, identifiers lower-case, symbols verbatim
  int64_t ival = 0;
  double dval = 0;
  size_t pos = 0;    // byte offset in the source
};

std::vector<Token> tokenize(const std::string& sql);

// Parses a ';'-separated script. Each statement keeps its source text.
std::vector<Statement> parseScript(const std::string& sql);

// Parses exactly one statement (a trailing ';' is optional).
Statement parseStatement(const std::string& sql);

}  // namespace sysr
