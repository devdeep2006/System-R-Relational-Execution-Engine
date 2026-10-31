#pragma once

#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "execution/operators.h"
#include "optimizer/optimizer.h"
#include "sql/parser.h"

namespace sysr {

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<Tuple> rows;
  bool hasRows = false;  // a SELECT result (as opposed to a status or EXPLAIN text)
  std::string message;   // status line or EXPLAIN output
  double planningMs = 0, executionMs = 0;
  IOStats io;            // measured block I/O during execution
  BufferStats buffer;
  OptimizerStats optimizer;
  Cost estimated;        // optimizer's estimate for the chosen plan
  double estimatedMs = 0;
  std::string plan;      // EXPLAIN text of the chosen plan
};

class Database {
 public:
  explicit Database(size_t bufferFrames = 256);

  QueryResult execute(const std::string& sql);  // exactly one statement
  std::vector<QueryResult> executeScript(const std::string& sql);
  QueryResult run(const Statement& st);

  // Programmatic API used by the data generator, tests and benchmarks.
  TableInfo* createTable(const std::string& name, const std::vector<Column>& cols);
  void insert(const std::string& table, Tuple row);
  void analyzeAll();
  void createIndex(const std::string& table, const std::string& column);

  OptimizerConfig& config() { return cfg_; }
  Catalog& catalog() { return catalog_; }
  BufferPool& pool() { return pool_; }
  DiskManager& disk() { return disk_; }

  // Evict the buffer pool before every query so measured I/O starts from a cold cache.
  bool coldCache = false;

 private:
  QueryResult runSelect(const SelectStmt& s, bool explain, bool analyze);
  QueryResult runSet(const Statement& st);
  QueryResult showTables() const;
  QueryResult showStats(const std::string& table) const;

  DiskManager disk_;
  BufferPool pool_;
  Catalog catalog_;
  OptimizerConfig cfg_;
};

}  // namespace sysr
