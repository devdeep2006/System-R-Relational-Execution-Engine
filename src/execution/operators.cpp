#include "execution/operators.h"

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace sysr {

namespace {

size_t footprint(const Tuple& t) { return slotted::footprint(encodedSize(t)); }
size_t budgetBytes(size_t pages) { return pages * slotted::kUsable; }

void concat(const Tuple& l, const Tuple& r, Tuple& out) {
  out.clear();
  out.reserve(l.size() + r.size());
  out.insert(out.end(), l.begin(), l.end());
  out.insert(out.end(), r.begin(), r.end());
}

int slotOf(const Layout& layout, ColRef c) {
  for (size_t i = 0; i < layout.size(); ++i)
    if (layout[i] == c) return static_cast<int>(i);
  throw DbError("internal: join key not present in operator input");
}

void keyOf(const Tuple& t, const std::vector<int>& slots, Tuple& key) {
  key.resize(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) key[i] = t[static_cast<size_t>(slots[i])];
}

Layout fullLayout(const BoundQuery& q, int rel) {
  Layout l;
  size_t n = q.rels[static_cast<size_t>(rel)].table->schema.size();
  for (size_t c = 0; c < n; ++c) l.push_back(ColRef{rel, static_cast<int>(c)});
  return l;
}

// Projects table columns (by column index) out of a full heap row.
void projectRow(Tuple& row, const std::vector<int>& cols, Tuple& out) {
  out.resize(cols.size());
  for (size_t i = 0; i < cols.size(); ++i) out[i] = std::move(row[static_cast<size_t>(cols[i])]);
}

std::vector<int> projection(const Layout& layout, int rel, size_t from = 0) {
  std::vector<int> cols;
  for (size_t i = from; i < layout.size(); ++i)
    if (layout[i].rel == rel) cols.push_back(layout[i].col);
  return cols;
}

// Comparator for ORDER BY / merge sort keys. Plain column keys read the slot directly.
class TupleOrder {
 public:
  TupleOrder() = default;
  explicit TupleOrder(std::vector<SortKey> keys) : keys_(std::move(keys)) {
    for (const auto& k : keys_)
      slots_.push_back((k.expr->kind == ExprKind::Col || k.expr->kind == ExprKind::Slot) ? k.expr->slot : -1);
  }
  int compare(const Tuple& a, const Tuple& b) const {
    for (size_t i = 0; i < keys_.size(); ++i) {
      int c;
      if (slots_[i] >= 0) c = a[static_cast<size_t>(slots_[i])].compare(b[static_cast<size_t>(slots_[i])]);
      else c = eval(*keys_[i].expr, a).compare(eval(*keys_[i].expr, b));
      if (c != 0) return keys_[i].desc ? -c : c;
    }
    return 0;
  }
  bool less(const Tuple& a, const Tuple& b) const { return compare(a, b) < 0; }

 private:
  std::vector<SortKey> keys_;
  std::vector<int> slots_;
};

std::vector<SortKey> resolveKeys(const std::vector<SortKey>& keys, const Layout& layout) {
  std::vector<SortKey> out;
  for (const auto& k : keys) out.push_back(SortKey{resolve(k.expr, layout), k.desc});
  return out;
}

// ----------------------------------------------------------------- scans

class SeqScanOp : public Operator {
 public:
  SeqScanOp(const PhysNode* n, ExecContext& ctx) : Operator(n) {
    heap_ = ctx.query->rels[static_cast<size_t>(n->rel)].table->heap.get();
    filters_ = resolveAll(n->filters, fullLayout(*ctx.query, n->rel));
    cols_ = projection(n->layout, n->rel);
  }
  void open() override { scan_ = std::make_unique<HeapFile::Scanner>(heap_); }
  void close() override { scan_.reset(); }

 protected:
  bool nextImpl(Tuple& out) override {
    while (scan_->next(row_)) {
      if (!evalAll(filters_, row_)) continue;
      projectRow(row_, cols_, out);
      return true;
    }
    return false;
  }

 private:
  const HeapFile* heap_;
  std::vector<ExprPtr> filters_;
  std::vector<int> cols_;
  std::unique_ptr<HeapFile::Scanner> scan_;
  Tuple row_;
};

class IndexScanOp : public Operator {
 public:
  IndexScanOp(const PhysNode* n, ExecContext& ctx) : Operator(n) {
    heap_ = ctx.query->rels[static_cast<size_t>(n->rel)].table->heap.get();
    filters_ = resolveAll(n->filters, fullLayout(*ctx.query, n->rel));
    cols_ = projection(n->layout, n->rel);
  }
  void open() override { cursor_ = node_->index->tree->range(node_->lo, node_->hi); }

 protected:
  bool nextImpl(Tuple& out) override {
    int64_t key;
    RID rid;
    while (cursor_.next(key, rid)) {
      heap_->get(rid, row_);
      if (!evalAll(filters_, row_)) continue;
      projectRow(row_, cols_, out);
      return true;
    }
    return false;
  }

 private:
  const HeapFile* heap_;
  std::vector<ExprPtr> filters_;
  std::vector<int> cols_;
  BPlusTree::Cursor cursor_;
  Tuple row_;
};

// ------------------------------------------------------------- simple ops

class FilterOp : public Operator {
 public:
  FilterOp(const PhysNode* n, OpPtr child) : Operator(n), child_(std::move(child)) {
    preds_ = resolveAll(n->filters, n->kids[0]->layout);
    for (const auto& p : preds_)
      if (p->kind == ExprKind::Const && !p->value.truthy()) dead_ = true;
    children = {child_.get()};
  }
  void open() override { if (!dead_) child_->open(); }
  void close() override { if (!dead_) child_->close(); }

 protected:
  bool nextImpl(Tuple& out) override {
    if (dead_) return false;
    while (child_->next(out))
      if (evalAll(preds_, out)) return true;
    return false;
  }

 private:
  OpPtr child_;
  std::vector<ExprPtr> preds_;
  bool dead_ = false;
};

class ProjectOp : public Operator {
 public:
  ProjectOp(const PhysNode* n, OpPtr child) : Operator(n), child_(std::move(child)) {
    exprs_ = resolveAll(n->exprs, n->kids[0]->layout);
    children = {child_.get()};
  }
  void open() override { child_->open(); }
  void close() override { child_->close(); }

 protected:
  bool nextImpl(Tuple& out) override {
    if (!child_->next(in_)) return false;
    out.resize(exprs_.size());
    for (size_t i = 0; i < exprs_.size(); ++i) out[i] = eval(*exprs_[i], in_);
    return true;
  }

 private:
  OpPtr child_;
  std::vector<ExprPtr> exprs_;
  Tuple in_;
};

class LimitOp : public Operator {
 public:
  LimitOp(const PhysNode* n, OpPtr child) : Operator(n), child_(std::move(child)) { children = {child_.get()}; }
  void open() override { child_->open(); count_ = 0; }
  void close() override { child_->close(); }

 protected:
  bool nextImpl(Tuple& out) override {
    if (count_ >= node_->limit) return false;
    if (!child_->next(out)) return false;
    ++count_;
    return true;
  }

 private:
  OpPtr child_;
  int64_t count_ = 0;
};

// ------------------------------------------------------------------ joins

// Hash join. Builds an in-memory hash table when the build input fits in M-2 pages and
// streams the probe input (pipelined, preserving probe order). Otherwise falls back to a
// Grace hash join: both inputs are hash-partitioned into spill files, and each partition
// pair is joined in memory, repartitioning recursively (new hash seed) when a build
// partition still does not fit.
class HashJoinOp : public Operator {
 public:
  HashJoinOp(const PhysNode* n, OpPtr left, OpPtr right, ExecContext& ctx)
      : Operator(n), left_(std::move(left)), right_(std::move(right)), ctx_(ctx) {
    for (const JoinKey& k : n->keys) {
      leftKeys_.push_back(slotOf(n->kids[0]->layout, k.left));
      rightKeys_.push_back(slotOf(n->kids[1]->layout, k.right));
    }
    residual_ = resolveAll(n->residual, n->layout);
    children = {left_.get(), right_.get()};
  }

  void open() override {
    table_.clear();
    work_.clear();
    matches_ = nullptr;
    const size_t M = ctx_.params.memPages;
    Operator* b = build();
    b->open();
    std::vector<Tuple> held;
    size_t bytes = 0;
    bool overflow = false;
    Tuple t;
    while (b->next(t)) {
      bytes += footprint(t);
      held.push_back(std::move(t));
      if (bytes > budgetBytes(M - 2)) { overflow = true; break; }
    }
    if (!overflow) {
      b->close();
      for (auto& r : held) insertBuild(std::move(r));
      pipelined_ = true;
      probe()->open();
      note_ = "in-memory hash table";
      return;
    }

    pipelined_ = false;
    double estPages = std::max(node_->kids[node_->buildRight ? 1 : 0]->pages, static_cast<double>(bytes) / slotted::kUsable + 1);
    std::vector<std::unique_ptr<SpillFile>> bparts, pparts;
    size_t parts = makePartitions(estPages, bparts, pparts);
    for (auto& r : held) bparts[partition(r, buildKeys(), 1, parts)]->append(r);
    held.clear();
    held.shrink_to_fit();
    while (b->next(t)) bparts[partition(t, buildKeys(), 1, parts)]->append(t);
    b->close();
    Operator* p = probe();
    p->open();
    while (p->next(t)) pparts[partition(t, probeKeys(), 1, parts)]->append(t);
    p->close();
    for (size_t i = 0; i < parts; ++i) {
      bparts[i]->finish();
      pparts[i]->finish();
      work_.push_back(Work{std::move(bparts[i]), std::move(pparts[i]), 1});
    }
    partitions_ = parts;
    maxLevel_ = 1;
  }

  void close() override {
    if (pipelined_) {
      probe()->close();
    } else {
      note_ = "grace: " + std::to_string(partitions_) + " partitions, " + std::to_string(maxLevel_) +
              (maxLevel_ > 1 ? " levels" : " level");
    }
    table_.clear();
    work_.clear();
    probeReader_.reset();
    curProbe_.reset();
  }

 protected:
  bool nextImpl(Tuple& out) override {
    while (true) {
      if (matches_ && mi_ < matches_->size()) {
        const Tuple& b = (*matches_)[mi_++];
        if (node_->buildRight) concat(probeRow_, b, out);
        else concat(b, probeRow_, out);
        if (residual_.empty() || evalAll(residual_, out)) return true;
        continue;
      }
      matches_ = nullptr;
      if (pipelined_) {
        if (!probe()->next(probeRow_)) return false;
      } else if (!probeReader_ || !probeReader_->next(probeRow_)) {
        if (!advancePartition()) return false;
        continue;
      }
      keyOf(probeRow_, probeKeys(), key_);
      auto it = table_.find(key_);
      if (it != table_.end()) {
        matches_ = &it->second;
        mi_ = 0;
      }
    }
  }

 private:
  struct Work {
    std::unique_ptr<SpillFile> build, probe;
    int level;
  };
  static constexpr int kMaxLevels = 3;

  Operator* build() const { return node_->buildRight ? right_.get() : left_.get(); }
  Operator* probe() const { return node_->buildRight ? left_.get() : right_.get(); }
  const std::vector<int>& buildKeys() const { return node_->buildRight ? rightKeys_ : leftKeys_; }
  const std::vector<int>& probeKeys() const { return node_->buildRight ? leftKeys_ : rightKeys_; }

  size_t makePartitions(double buildPages, std::vector<std::unique_ptr<SpillFile>>& b,
                        std::vector<std::unique_ptr<SpillFile>>& p) const {
    CostParams cp = ctx_.params;
    size_t parts = static_cast<size_t>(costmodel::gracePartitions(buildPages, cp));
    size_t chunk = std::max<size_t>(1, (cp.memPages - 1) / parts);
    for (size_t i = 0; i < parts; ++i) {
      b.push_back(std::make_unique<SpillFile>(ctx_.disk, chunk));
      p.push_back(std::make_unique<SpillFile>(ctx_.disk, chunk));
    }
    return parts;
  }

  size_t partition(const Tuple& t, const std::vector<int>& keys, int level, size_t parts) {
    keyOf(t, keys, scratch_);
    uint64_t h = TupleHash()(scratch_);
    return static_cast<size_t>(mix64(h ^ (0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(level))) % parts);
  }

  void insertBuild(Tuple t) {
    Tuple key;
    keyOf(t, buildKeys(), key);
    table_[std::move(key)].push_back(std::move(t));
  }

  bool advancePartition() {
    const size_t M = ctx_.params.memPages;
    while (!work_.empty()) {
      Work w = std::move(work_.back());
      work_.pop_back();
      if (w.build->tuples() == 0 || w.probe->tuples() == 0) continue;
      if (w.build->pages() > M - 2 && w.level < kMaxLevels) {
        std::vector<std::unique_ptr<SpillFile>> bparts, pparts;
        size_t parts = makePartitions(static_cast<double>(w.build->pages()), bparts, pparts);
        Tuple t;
        auto br = w.build->reader(ctx_.params.ioChunk);
        while (br.next(t)) bparts[partition(t, buildKeys(), w.level + 1, parts)]->append(t);
        auto pr = w.probe->reader(ctx_.params.ioChunk);
        while (pr.next(t)) pparts[partition(t, probeKeys(), w.level + 1, parts)]->append(t);
        for (size_t i = 0; i < parts; ++i) {
          bparts[i]->finish();
          pparts[i]->finish();
          work_.push_back(Work{std::move(bparts[i]), std::move(pparts[i]), w.level + 1});
        }
        maxLevel_ = std::max(maxLevel_, w.level + 1);
        continue;
      }
      table_.clear();
      Tuple t;
      auto br = w.build->reader(ctx_.params.ioChunk);
      while (br.next(t)) insertBuild(std::move(t));
      w.build.reset();
      curProbe_ = std::move(w.probe);
      probeReader_ = std::make_unique<SpillFile::Reader>(curProbe_->reader(ctx_.params.ioChunk));
      return true;
    }
    return false;
  }

  OpPtr left_, right_;
  ExecContext& ctx_;
  std::vector<int> leftKeys_, rightKeys_;
  std::vector<ExprPtr> residual_;
  std::unordered_map<Tuple, std::vector<Tuple>, TupleHash, TupleEq> table_;
  const std::vector<Tuple>* matches_ = nullptr;
  size_t mi_ = 0;
  Tuple probeRow_, key_, scratch_;
  bool pipelined_ = true;
  std::vector<Work> work_;
  std::unique_ptr<SpillFile> curProbe_;
  std::unique_ptr<SpillFile::Reader> probeReader_;
  size_t partitions_ = 0;
  int maxLevel_ = 0;
};

// Sort-merge join over inputs sorted on the first key. Runs of equal keys on the right are
// buffered so many-to-many matches produce the full cross product of each key group.
class MergeJoinOp : public Operator {
 public:
  MergeJoinOp(const PhysNode* n, OpPtr left, OpPtr right) : Operator(n), left_(std::move(left)), right_(std::move(right)) {
    lk_ = static_cast<size_t>(slotOf(n->kids[0]->layout, n->keys[0].left));
    rk_ = static_cast<size_t>(slotOf(n->kids[1]->layout, n->keys[0].right));
    residual_ = resolveAll(n->residual, n->layout);
    children = {left_.get(), right_.get()};
  }
  void open() override {
    left_->open();
    right_->open();
    haveL_ = left_->next(l_);
    haveR_ = right_->next(r_);
    group_.clear();
    inGroup_ = false;
  }
  void close() override {
    left_->close();
    right_->close();
    group_.clear();
  }

 protected:
  bool nextImpl(Tuple& out) override {
    while (true) {
      if (inGroup_) {
        if (gi_ < group_.size()) {
          concat(l_, group_[gi_++], out);
          if (residual_.empty() || evalAll(residual_, out)) return true;
          continue;
        }
        haveL_ = left_->next(l_);
        gi_ = 0;
        if (haveL_ && l_[lk_].compare(groupKey_) == 0) continue;
        inGroup_ = false;
        group_.clear();
        continue;
      }
      if (!haveL_ || !haveR_) return false;
      int c = l_[lk_].compare(r_[rk_]);
      if (c < 0) { haveL_ = left_->next(l_); continue; }
      if (c > 0) { haveR_ = right_->next(r_); continue; }
      groupKey_ = r_[rk_];
      group_.clear();
      while (haveR_ && r_[rk_].compare(groupKey_) == 0) {
        group_.push_back(r_);
        haveR_ = right_->next(r_);
      }
      inGroup_ = true;
      gi_ = 0;
    }
  }

 private:
  OpPtr left_, right_;
  size_t lk_, rk_;
  std::vector<ExprPtr> residual_;
  Tuple l_, r_;
  bool haveL_ = false, haveR_ = false, inGroup_ = false;
  Value groupKey_;
  std::vector<Tuple> group_;
  size_t gi_ = 0;
};

// Block nested loop join. If the inner input fits in M-2 pages it is held in memory and the
// outer is streamed once. Otherwise the inner is spilled once and rescanned for every
// (M-2)-page block of the outer. With equi-join keys the in-memory side is hashed so each
// probe touches only matching tuples; without keys every pair is tested (theta / cross).
class NestedLoopJoinOp : public Operator {
 public:
  NestedLoopJoinOp(const PhysNode* n, OpPtr outer, OpPtr inner, ExecContext& ctx)
      : Operator(n), outer_(std::move(outer)), inner_(std::move(inner)), ctx_(ctx) {
    for (const JoinKey& k : n->keys) {
      outerKeys_.push_back(slotOf(n->kids[0]->layout, k.left));
      innerKeys_.push_back(slotOf(n->kids[1]->layout, k.right));
    }
    hashed_ = !n->keys.empty();
    residual_ = resolveAll(n->residual, n->layout);
    children = {outer_.get(), inner_.get()};
  }

  void open() override {
    const size_t budget = budgetBytes(ctx_.params.memPages - 2);
    block_.clear();
    index_.clear();
    spill_.reset();
    reader_.reset();
    hasCur_ = false;
    blocks_ = 0;
    inner_->open();
    size_t bytes = 0;
    bool overflow = false;
    Tuple t;
    while (inner_->next(t)) {
      bytes += footprint(t);
      block_.push_back(std::move(t));
      if (bytes > budget) { overflow = true; break; }
    }
    if (!overflow) {
      inner_->close();
      innerInMemory_ = true;
      indexBlock(innerKeys_);
      outer_->open();
      note_ = "inner held in memory";
      return;
    }
    innerInMemory_ = false;
    spill_ = std::make_unique<SpillFile>(ctx_.disk, ctx_.params.ioChunk);
    for (const auto& r : block_) spill_->append(r);
    block_.clear();
    while (inner_->next(t)) spill_->append(t);
    inner_->close();
    spill_->finish();
    outer_->open();
  }

  void close() override {
    outer_->close();
    if (!innerInMemory_)
      note_ = "inner spilled (" + std::to_string(spill_ ? spill_->pages() : 0) + " pages), " + std::to_string(blocks_) +
              " outer blocks";
    block_.clear();
    index_.clear();
    reader_.reset();
    spill_.reset();
  }

 protected:
  bool nextImpl(Tuple& out) override {
    while (true) {
      while (hasCur_ && ci_ < candidateCount()) {
        size_t j = all_ ? ci_ : (*cand_)[ci_];
        ++ci_;
        const Tuple& held = block_[j];
        if (innerInMemory_) concat(cur_, held, out);
        else concat(held, cur_, out);
        if (residual_.empty() || evalAll(residual_, out)) return true;
      }
      hasCur_ = false;
      if (innerInMemory_) {
        if (!outer_->next(cur_)) return false;
      } else if (!reader_ || !reader_->next(cur_)) {
        if (!loadOuterBlock()) return false;
        continue;
      }
      hasCur_ = true;
      ci_ = 0;
      setCandidates(cur_, innerInMemory_ ? outerKeys_ : innerKeys_);
    }
  }

 private:
  size_t candidateCount() const { return all_ ? block_.size() : (cand_ ? cand_->size() : 0); }

  void indexBlock(const std::vector<int>& keys) {
    index_.clear();
    if (!hashed_) return;
    Tuple key;
    for (size_t i = 0; i < block_.size(); ++i) {
      keyOf(block_[i], keys, key);
      index_[key].push_back(i);
    }
  }

  void setCandidates(const Tuple& t, const std::vector<int>& keys) {
    if (!hashed_) { all_ = true; return; }
    all_ = false;
    keyOf(t, keys, key_);
    auto it = index_.find(key_);
    cand_ = it == index_.end() ? nullptr : &it->second;
  }

  bool loadOuterBlock() {
    const size_t budget = budgetBytes(ctx_.params.memPages - 2);
    block_.clear();
    size_t bytes = 0;
    Tuple t;
    while (bytes <= budget && outer_->next(t)) {
      bytes += footprint(t);
      block_.push_back(std::move(t));
    }
    if (block_.empty()) return false;
    ++blocks_;
    indexBlock(outerKeys_);
    reader_ = std::make_unique<SpillFile::Reader>(spill_->reader(ctx_.params.ioChunk));
    return true;
  }

  OpPtr outer_, inner_;
  ExecContext& ctx_;
  std::vector<int> outerKeys_, innerKeys_;
  bool hashed_ = false;
  std::vector<ExprPtr> residual_;
  bool innerInMemory_ = true;
  std::vector<Tuple> block_;
  std::unordered_map<Tuple, std::vector<size_t>, TupleHash, TupleEq> index_;
  std::unique_ptr<SpillFile> spill_;
  std::unique_ptr<SpillFile::Reader> reader_;
  Tuple cur_, key_;
  bool hasCur_ = false, all_ = false;
  const std::vector<size_t>* cand_ = nullptr;
  size_t ci_ = 0;
  size_t blocks_ = 0;
};

// Index nested loop join: for each outer row, probe the inner relation's B+ tree with the
// join key and fetch matching heap rows.
class IndexNLJOp : public Operator {
 public:
  IndexNLJOp(const PhysNode* n, OpPtr outer, ExecContext& ctx) : Operator(n), outer_(std::move(outer)) {
    heap_ = ctx.query->rels[static_cast<size_t>(n->rel)].table->heap.get();
    tree_ = n->index->tree.get();
    outerKey_ = static_cast<size_t>(slotOf(n->kids[0]->layout, n->keys[0].left));
    innerFilters_ = resolveAll(n->filters, fullLayout(*ctx.query, n->rel));
    innerCols_ = projection(n->layout, n->rel, n->kids[0]->layout.size());
    residual_ = resolveAll(n->residual, n->layout);
    children = {outer_.get()};
  }
  void open() override {
    outer_->open();
    active_ = false;
    probes_ = 0;
  }
  void close() override {
    outer_->close();
    note_ = std::to_string(probes_) + " index probes";
  }

 protected:
  bool nextImpl(Tuple& out) override {
    while (true) {
      if (active_) {
        int64_t k;
        RID rid;
        while (cursor_.next(k, rid)) {
          heap_->get(rid, row_);
          if (!evalAll(innerFilters_, row_)) continue;
          projectRow(row_, innerCols_, inner_);
          concat(outerRow_, inner_, out);
          if (residual_.empty() || evalAll(residual_, out)) return true;
        }
        active_ = false;
      }
      if (!outer_->next(outerRow_)) return false;
      const Value& v = outerRow_[outerKey_];
      if (!v.isNumeric()) continue;
      if (v.type() == TypeId::Double && v.asDouble() != static_cast<double>(v.asInt())) continue;
      cursor_ = tree_->range(v.asInt(), v.asInt());
      active_ = true;
      ++probes_;
    }
  }

 private:
  OpPtr outer_;
  const HeapFile* heap_;
  const BPlusTree* tree_;
  size_t outerKey_;
  std::vector<ExprPtr> innerFilters_, residual_;
  std::vector<int> innerCols_;
  BPlusTree::Cursor cursor_;
  bool active_ = false;
  Tuple outerRow_, row_, inner_;
  uint64_t probes_ = 0;
};

// ---------------------------------------------------------- sort / top-n

// External merge sort. Pass 0 fills M pages, sorts them and writes a run; merge passes
// combine up to floor(M / b_b) - 1 runs at a time (b_b pages of input buffer per run) until
// one pass can stream the final merge to the parent. Stable: ties keep input order.
class SortOp : public Operator {
 public:
  SortOp(const PhysNode* n, OpPtr child, ExecContext& ctx) : Operator(n), child_(std::move(child)), ctx_(ctx) {
    order_ = TupleOrder(resolveKeys(n->sortKeys, n->kids[0]->layout));
    children = {child_.get()};
  }

  void open() override {
    const size_t M = ctx_.params.memPages;
    runs_.clear();
    mem_.clear();
    heap_.clear();
    readers_.clear();
    std::vector<Tuple> buf;
    size_t bytes = 0;
    Tuple t;
    child_->open();
    while (child_->next(t)) {
      bytes += footprint(t);
      buf.push_back(std::move(t));
      if (bytes > budgetBytes(M)) {
        writeRun(buf);
        bytes = 0;
      }
    }
    child_->close();
    if (runs_.empty()) {
      std::stable_sort(buf.begin(), buf.end(), [&](const Tuple& a, const Tuple& b) { return order_.less(a, b); });
      mem_ = std::move(buf);
      memPos_ = 0;
      inMemory_ = true;
      note_ = "in-memory";
      return;
    }
    if (!buf.empty()) writeRun(buf);
    inMemory_ = false;
    const size_t initialRuns = runs_.size();
    const size_t fanIn = std::max<size_t>(2, M / ctx_.params.ioChunk - 1);
    size_t passes = 1;
    while (runs_.size() > fanIn) {
      ++passes;
      std::vector<std::unique_ptr<SpillFile>> merged;
      for (size_t i = 0; i < runs_.size(); i += fanIn) {
        auto out = std::make_unique<SpillFile>(ctx_.disk, ctx_.params.ioChunk);
        startMerge(i, std::min(runs_.size(), i + fanIn));
        Tuple x;
        while (popMerged(x)) out->append(x);
        out->finish();
        merged.push_back(std::move(out));
      }
      readers_.clear();
      runs_ = std::move(merged);
    }
    startMerge(0, runs_.size());
    note_ = "external: " + std::to_string(initialRuns) + " runs, " + std::to_string(passes) + " merge pass" +
            (passes > 1 ? "es" : "");
  }

  void close() override {
    runs_.clear();
    readers_.clear();
    heap_.clear();
    mem_.clear();
  }

 protected:
  bool nextImpl(Tuple& out) override {
    if (inMemory_) {
      if (memPos_ >= mem_.size()) return false;
      out = std::move(mem_[memPos_++]);
      return true;
    }
    return popMerged(out);
  }

 private:
  struct Item {
    Tuple t;
    size_t src;
  };

  void writeRun(std::vector<Tuple>& buf) {
    std::stable_sort(buf.begin(), buf.end(), [&](const Tuple& a, const Tuple& b) { return order_.less(a, b); });
    // A run is written as one request: its whole buffer is one extent.
    auto run = std::make_unique<SpillFile>(ctx_.disk, ctx_.params.memPages + 2);
    for (const auto& r : buf) run->append(r);
    run->finish();
    runs_.push_back(std::move(run));
    buf.clear();
  }

  // Min-heap order: smaller tuple first, earlier run first on ties (stability).
  bool after(const Item& a, const Item& b) const {
    int c = order_.compare(a.t, b.t);
    return c != 0 ? c > 0 : a.src > b.src;
  }

  void startMerge(size_t from, size_t to) {
    readers_.clear();
    heap_.clear();
    for (size_t i = from; i < to; ++i) {
      readers_.push_back(std::make_unique<SpillFile::Reader>(runs_[i]->reader(ctx_.params.ioChunk)));
      Item it{{}, readers_.size() - 1};
      if (readers_.back()->next(it.t)) pushItem(std::move(it));
    }
  }
  void pushItem(Item it) {
    heap_.push_back(std::move(it));
    std::push_heap(heap_.begin(), heap_.end(), [&](const Item& a, const Item& b) { return after(a, b); });
  }
  bool popMerged(Tuple& out) {
    if (heap_.empty()) return false;
    std::pop_heap(heap_.begin(), heap_.end(), [&](const Item& a, const Item& b) { return after(a, b); });
    Item it = std::move(heap_.back());
    heap_.pop_back();
    out = std::move(it.t);
    Item nextItem{{}, it.src};
    if (readers_[it.src]->next(nextItem.t)) pushItem(std::move(nextItem));
    return true;
  }

  OpPtr child_;
  ExecContext& ctx_;
  TupleOrder order_;
  bool inMemory_ = true;
  std::vector<Tuple> mem_;
  size_t memPos_ = 0;
  std::vector<std::unique_ptr<SpillFile>> runs_;
  std::vector<std::unique_ptr<SpillFile::Reader>> readers_;
  std::vector<Item> heap_;
};

// ORDER BY ... LIMIT k with a bounded max-heap: O(n log k) time, O(k) memory, no spilling.
class TopNOp : public Operator {
 public:
  TopNOp(const PhysNode* n, OpPtr child) : Operator(n), child_(std::move(child)) {
    order_ = TupleOrder(resolveKeys(n->sortKeys, n->kids[0]->layout));
    children = {child_.get()};
  }
  void open() override {
    const size_t k = static_cast<size_t>(node_->limit);
    std::vector<Item> heap;
    // Total order (sort keys, then arrival) so ties are stable. As a heap comparator this
    // keeps the worst retained row on top, ready to be evicted.
    auto before = [&](const Item& a, const Item& b) {
      int c = order_.compare(a.t, b.t);
      return c != 0 ? c < 0 : a.seq < b.seq;
    };
    child_->open();
    Tuple t;
    uint64_t seq = 0;
    while (child_->next(t)) {
      Item it{std::move(t), seq++};
      if (heap.size() < k) {
        heap.push_back(std::move(it));
        std::push_heap(heap.begin(), heap.end(), before);
      } else if (k > 0 && before(it, heap.front())) {
        std::pop_heap(heap.begin(), heap.end(), before);
        heap.back() = std::move(it);
        std::push_heap(heap.begin(), heap.end(), before);
      }
    }
    child_->close();
    std::sort(heap.begin(), heap.end(), before);
    out_.clear();
    for (auto& it : heap) out_.push_back(std::move(it.t));
    pos_ = 0;
  }

 protected:
  bool nextImpl(Tuple& out) override {
    if (pos_ >= out_.size()) return false;
    out = std::move(out_[pos_++]);
    return true;
  }

 private:
  struct Item {
    Tuple t;
    uint64_t seq;
  };
  OpPtr child_;
  TupleOrder order_;
  std::vector<Tuple> out_;
  size_t pos_ = 0;
};

// ------------------------------------------------------------ aggregation

class HashAggOp : public Operator {
 public:
  HashAggOp(const PhysNode* n, OpPtr child, ExecContext& ctx) : Operator(n), child_(std::move(child)) {
    const Layout& in = n->kids[0]->layout;
    groupKeys_ = resolveAll(ctx.query->groupKeys, in);
    for (const AggSpec& a : ctx.query->aggs) {
      AggSpec r = a;
      if (r.arg) r.arg = resolve(r.arg, in);
      aggs_.push_back(r);
    }
    children = {child_.get()};
  }

  void open() override {
    keys_.clear();
    states_.clear();
    std::unordered_map<Tuple, size_t, TupleHash, TupleEq> index;
    child_->open();
    Tuple t, key;
    while (child_->next(t)) {
      key.resize(groupKeys_.size());
      for (size_t i = 0; i < groupKeys_.size(); ++i) key[i] = eval(*groupKeys_[i], t);
      auto it = index.find(key);
      size_t g;
      if (it == index.end()) {
        g = keys_.size();
        index.emplace(key, g);
        keys_.push_back(key);
        states_.emplace_back(aggs_.size());
      } else {
        g = it->second;
      }
      for (size_t a = 0; a < aggs_.size(); ++a) update(aggs_[a], states_[g][a], t);
    }
    child_->close();
    if (groupKeys_.empty() && keys_.empty()) {
      keys_.emplace_back();
      states_.emplace_back(aggs_.size());
    }
    pos_ = 0;
    note_ = std::to_string(keys_.size()) + " groups";
  }

 protected:
  bool nextImpl(Tuple& out) override {
    if (pos_ >= keys_.size()) return false;
    out = keys_[pos_];
    for (size_t a = 0; a < aggs_.size(); ++a) out.push_back(finalize(aggs_[a], states_[pos_][a]));
    ++pos_;
    return true;
  }

 private:
  struct State {
    int64_t count = 0;
    int64_t isum = 0;
    double dsum = 0;
    Value min, max;
  };

  static void update(const AggSpec& a, State& s, const Tuple& t) {
    if (a.fn == AggFn::CountStar) { ++s.count; return; }
    Value v = eval(*a.arg, t);
    if (v.isNull()) return;
    ++s.count;
    switch (a.fn) {
      case AggFn::Sum:
      case AggFn::Avg:
        if (v.type() == TypeId::Int) s.isum += v.asInt();
        s.dsum += v.asDouble();
        break;
      case AggFn::Min:
        if (s.min.isNull() || v.compare(s.min) < 0) s.min = v;
        break;
      case AggFn::Max:
        if (s.max.isNull() || v.compare(s.max) > 0) s.max = v;
        break;
      default:
        break;
    }
  }

  static Value finalize(const AggSpec& a, const State& s) {
    switch (a.fn) {
      case AggFn::CountStar:
      case AggFn::Count: return Value::Int(s.count);
      case AggFn::Sum:
        if (s.count == 0) return Value();
        return a.type == TypeId::Int ? Value::Int(s.isum) : Value::Double(s.dsum);
      case AggFn::Avg: return s.count == 0 ? Value() : Value::Double(s.dsum / static_cast<double>(s.count));
      case AggFn::Min: return s.min;
      case AggFn::Max: return s.max;
    }
    return Value();
  }

  OpPtr child_;
  std::vector<ExprPtr> groupKeys_;
  std::vector<AggSpec> aggs_;
  std::vector<Tuple> keys_;
  std::vector<std::vector<State>> states_;
  size_t pos_ = 0;
};

}  // namespace

OpPtr buildOperator(const PhysNode& n, ExecContext& ctx) {
  switch (n.kind) {
    case PhysKind::SeqScan: return std::make_unique<SeqScanOp>(&n, ctx);
    case PhysKind::IndexScan: return std::make_unique<IndexScanOp>(&n, ctx);
    case PhysKind::Filter: return std::make_unique<FilterOp>(&n, buildOperator(*n.kids[0], ctx));
    case PhysKind::Project: return std::make_unique<ProjectOp>(&n, buildOperator(*n.kids[0], ctx));
    case PhysKind::Limit: return std::make_unique<LimitOp>(&n, buildOperator(*n.kids[0], ctx));
    case PhysKind::HashJoin:
      return std::make_unique<HashJoinOp>(&n, buildOperator(*n.kids[0], ctx), buildOperator(*n.kids[1], ctx), ctx);
    case PhysKind::MergeJoin:
      return std::make_unique<MergeJoinOp>(&n, buildOperator(*n.kids[0], ctx), buildOperator(*n.kids[1], ctx));
    case PhysKind::NestedLoop:
      return std::make_unique<NestedLoopJoinOp>(&n, buildOperator(*n.kids[0], ctx), buildOperator(*n.kids[1], ctx), ctx);
    case PhysKind::IndexNLJ: return std::make_unique<IndexNLJOp>(&n, buildOperator(*n.kids[0], ctx), ctx);
    case PhysKind::Sort: return std::make_unique<SortOp>(&n, buildOperator(*n.kids[0], ctx), ctx);
    case PhysKind::TopN: return std::make_unique<TopNOp>(&n, buildOperator(*n.kids[0], ctx));
    case PhysKind::HashAgg: return std::make_unique<HashAggOp>(&n, buildOperator(*n.kids[0], ctx), ctx);
  }
  throw DbError("internal: unknown physical operator");
}

}  // namespace sysr
