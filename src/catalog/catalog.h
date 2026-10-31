#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catalog/statistics.h"
#include "index/bplus_tree.h"
#include "storage/heap_file.h"

namespace sysr {

struct IndexInfo {
  std::string name;
  int column = -1;
  std::unique_ptr<BPlusTree> tree;
  // True when the heap is physically ordered on the key (correlation ~ 1), so a range scan
  // touches heap pages sequentially instead of once per matching row.
  bool clustered = false;
};

struct TableInfo {
  std::string name;
  Schema schema;
  std::unique_ptr<HeapFile> heap;
  TableStats stats;
  bool statsStale = true;
  std::vector<std::unique_ptr<IndexInfo>> indexes;

  const IndexInfo* indexOn(int column) const {
    for (const auto& ix : indexes)
      if (ix->column == column) return ix.get();
    return nullptr;
  }
};

class Catalog {
 public:
  explicit Catalog(BufferPool* bp) : bp_(bp) {}

  TableInfo* createTable(const std::string& name, Schema schema) {
    if (tables_.count(name)) throw DbError("table '" + name + "' already exists");
    if (schema.cols.empty()) throw DbError("table must have at least one column");
    for (size_t i = 0; i < schema.cols.size(); ++i)
      for (size_t j = i + 1; j < schema.cols.size(); ++j)
        if (schema.cols[i].name == schema.cols[j].name) throw DbError("duplicate column '" + schema.cols[i].name + "'");
    auto t = std::make_unique<TableInfo>();
    t->name = name;
    t->schema = std::move(schema);
    t->heap = std::make_unique<HeapFile>(bp_);
    TableInfo* raw = t.get();
    tables_[name] = std::move(t);
    return raw;
  }

  TableInfo* find(const std::string& name) const {
    auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : it->second.get();
  }

  TableInfo& require(const std::string& name) const {
    TableInfo* t = find(name);
    if (!t) throw DbError("table '" + name + "' does not exist");
    return *t;
  }

  // Type-checks (coercing INT -> DOUBLE), appends to the heap and maintains every index.
  void insert(TableInfo& t, Tuple row) {
    if (row.size() != t.schema.size())
      throw DbError("INSERT into " + t.name + ": expected " + std::to_string(t.schema.size()) + " values, got " +
                    std::to_string(row.size()));
    for (size_t i = 0; i < row.size(); ++i) {
      TypeId want = t.schema.cols[i].type;
      if (row[i].type() == want) continue;
      if (want == TypeId::Double && row[i].type() == TypeId::Int) { row[i] = Value::Double(row[i].asDouble()); continue; }
      throw DbError("INSERT into " + t.name + ": column '" + t.schema.cols[i].name + "' expects " + typeName(want) +
                    ", got " + typeName(row[i].type()));
    }
    RID rid = t.heap->insert(row);
    for (auto& ix : t.indexes) ix->tree->insert(row[static_cast<size_t>(ix->column)].asInt(), rid);
    t.statsStale = true;
  }

  void analyze(TableInfo& t, const AnalyzeOptions& opt = {}) {
    t.stats = analyzeTable(*t.heap, t.schema, opt);
    t.statsStale = false;
    for (auto& ix : t.indexes) ix->clustered = t.stats.cols[static_cast<size_t>(ix->column)].correlation > 0.95;
  }

  IndexInfo* createIndex(TableInfo& t, const std::string& column, std::string name = "") {
    int col = t.schema.indexOf(column);
    if (col < 0) throw DbError("column '" + column + "' does not exist in " + t.name);
    if (t.schema.cols[static_cast<size_t>(col)].type != TypeId::Int) throw DbError("indexes are supported on INT columns only");
    if (t.indexOn(col)) throw DbError("index on " + t.name + "." + column + " already exists");
    auto ix = std::make_unique<IndexInfo>();
    ix->name = name.empty() ? t.name + "_" + column + "_idx" : std::move(name);
    ix->column = col;
    ix->tree = std::make_unique<BPlusTree>(bp_);
    HeapFile::Scanner scan(t.heap.get());
    Tuple row;
    RID rid;
    while (scan.next(row, &rid)) ix->tree->insert(row[static_cast<size_t>(col)].asInt(), rid);
    if (t.stats.valid) ix->clustered = t.stats.cols[static_cast<size_t>(col)].correlation > 0.95;
    t.indexes.push_back(std::move(ix));
    return t.indexes.back().get();
  }

  std::vector<TableInfo*> tables() const {
    std::vector<TableInfo*> out;
    for (const auto& kv : tables_) out.push_back(kv.second.get());
    return out;
  }

 private:
  BufferPool* bp_;
  std::map<std::string, std::unique_ptr<TableInfo>> tables_;
};

}  // namespace sysr
