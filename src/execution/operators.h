#pragma once

#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "optimizer/plan.h"
#include "storage/spill_file.h"

namespace sysr {

struct ExecContext {
  Catalog* catalog = nullptr;
  BufferPool* pool = nullptr;
  DiskManager* disk = nullptr;
  const BoundQuery* query = nullptr;
  CostParams params;
};

// Volcano-style iterator: open() / next() / close(). Every operator counts the rows it
// produces and records what it did at run time (in-memory vs spilled, partitions, passes)
// for EXPLAIN ANALYZE.
class Operator {
 public:
  explicit Operator(const PhysNode* node) : node_(node) {}
  virtual ~Operator() = default;
  virtual void open() = 0;
  bool next(Tuple& out) {
    if (!nextImpl(out)) return false;
    ++rows_;
    return true;
  }
  virtual void close() {}

  const PhysNode* node() const { return node_; }
  uint64_t rowsProduced() const { return rows_; }
  const std::string& runtimeNote() const { return note_; }
  std::vector<Operator*> children;  // non-owning, for plan annotation

 protected:
  virtual bool nextImpl(Tuple& out) = 0;
  const PhysNode* node_;
  uint64_t rows_ = 0;
  std::string note_;
};

using OpPtr = std::unique_ptr<Operator>;

// Builds the operator tree for a physical plan, resolving expressions to tuple positions.
OpPtr buildOperator(const PhysNode& node, ExecContext& ctx);

}  // namespace sysr
