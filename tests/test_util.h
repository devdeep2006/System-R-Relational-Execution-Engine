#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "engine/database.h"
#include "test_framework.h"

namespace sysrtest {

inline std::string rowKey(const sysr::Tuple& t) {
  std::string s;
  for (size_t i = 0; i < t.size(); ++i) {
    if (i) s += '|';
    s += t[i].isNull() ? "NULL" : (t[i].type() == sysr::TypeId::Double ? sysr::Value::Double(t[i].asDouble()).toString()
                                                                       : t[i].toString());
  }
  return s;
}

// Order-insensitive canonical form of a result set.
inline std::vector<std::string> canon(const std::vector<sysr::Tuple>& rows) {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const auto& r : rows) out.push_back(rowKey(r));
  std::sort(out.begin(), out.end());
  return out;
}

inline int64_t scalar(sysr::Database& db, const std::string& sql) {
  sysr::QueryResult r = db.execute(sql);
  CHECK_EQ(r.rows.size(), size_t{1});
  return r.rows[0][0].asInt();
}

// Allows only the named join algorithm (nested loop remains the fallback for cross products).
inline void onlyJoin(sysr::OptimizerConfig& c, const std::string& which) {
  c.enableHashJoin = which == "hash";
  c.enableMergeJoin = which == "merge";
  c.enableNestedLoop = which == "nestloop";
  c.enableIndexNLJ = which == "indexnlj";
}

}  // namespace sysrtest
