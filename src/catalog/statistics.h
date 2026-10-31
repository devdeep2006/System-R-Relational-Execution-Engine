#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "common/rng.h"
#include "common/value.h"
#include "storage/heap_file.h"

namespace sysr {

// HyperLogLog distinct-count sketch (2^p registers, ~1.04/sqrt(2^p) standard error),
// with linear counting for small cardinalities.
class HyperLogLog {
 public:
  explicit HyperLogLog(int p = 12) : p_(p), reg_(size_t{1} << p, 0) {}

  void add(uint64_t hash) {
    size_t idx = static_cast<size_t>(hash >> (64 - p_));
    uint64_t rest = (hash << p_) | (uint64_t{1} << (p_ - 1));  // sentinel bounds the rank
    uint8_t rank = static_cast<uint8_t>(leadingZeros(rest) + 1);
    if (rank > reg_[idx]) reg_[idx] = rank;
  }

  double estimate() const {
    const double m = static_cast<double>(reg_.size());
    double sum = 0;
    size_t zeros = 0;
    for (uint8_t r : reg_) {
      sum += std::ldexp(1.0, -static_cast<int>(r));
      zeros += (r == 0);
    }
    double e = (0.7213 / (1.0 + 1.079 / m)) * m * m / sum;
    if (e <= 2.5 * m && zeros > 0) e = m * std::log(m / static_cast<double>(zeros));
    return e;
  }

 private:
  static int leadingZeros(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return x ? __builtin_clzll(x) : 64;
#else
    int n = 0;
    for (uint64_t bit = uint64_t{1} << 63; bit && !(x & bit); bit >>= 1) ++n;
    return n;
#endif
  }
  int p_;
  std::vector<uint8_t> reg_;
};

// Space-Saving heavy-hitter sketch (Metwally et al.): k counters, each count overestimates
// the true frequency by at most its recorded error.
class SpaceSaving {
 public:
  struct Counter {
    Value value;
    uint64_t count;
    uint64_t error;
  };
  explicit SpaceSaving(size_t k) : k_(k) {}

  void add(const Value& v) {
    auto it = index_.find(v);
    if (it != index_.end()) { ++counters_[it->second].count; return; }
    if (counters_.size() < k_) {
      index_.emplace(v, counters_.size());
      counters_.push_back({v, 1, 0});
      return;
    }
    size_t victim = 0;
    for (size_t i = 1; i < counters_.size(); ++i)
      if (counters_[i].count < counters_[victim].count) victim = i;
    Counter& c = counters_[victim];
    evicted_ = true;
    index_.erase(c.value);
    c.error = c.count;
    c.count += 1;
    c.value = v;
    index_.emplace(v, victim);
  }

  std::vector<Counter> top() const {
    std::vector<Counter> out = counters_;
    std::sort(out.begin(), out.end(), [](const Counter& a, const Counter& b) { return a.count > b.count; });
    return out;
  }
  // True while no counter was ever evicted: every count is then exact.
  bool exact() const { return !evicted_; }

 private:
  bool evicted_ = false;
  size_t k_;
  std::vector<Counter> counters_;
  std::unordered_map<Value, size_t, ValueHash, ValueEq> index_;
};

struct Bucket {
  double lo, hi;  // inclusive value bounds
  double rows;    // estimated rows in the bucket
};

// Closed/open interval over a numeric column, used for range selectivity.
struct Interval {
  bool hasLo = false, hasHi = false;
  double lo = 0, hi = 0;
  bool loInc = true, hiInc = true;
};

struct ColumnStats {
  TypeId type = TypeId::Null;
  double ndv = 1;
  Value min, max;
  double avgWidth = 9;      // average encoded bytes of one value
  double correlation = 0;   // fraction of adjacent rows in non-decreasing order (1.0 = physically sorted)
  std::vector<std::pair<Value, double>> mcv;  // most common values with their row fractions
  double mcvFraction = 0;
  std::vector<Bucket> hist;  // equi-depth histogram over the non-MCV values (numeric columns only)
};

struct TableStats {
  bool valid = false;
  double rows = 0;
  double pages = 0;
  double avgTupleBytes = 0;
  std::vector<ColumnStats> cols;
};

struct AnalyzeOptions {
  size_t buckets = 64;
  size_t mcvTarget = 32;
  size_t sketchCounters = 128;
  size_t sampleSize = 30000;
  uint64_t seed = 42;
};

// Single pass over the heap with bounded memory per column: HyperLogLog for NDV,
// Space-Saving for most-common values, a reservoir sample (Vitter's Algorithm R) for the
// equi-depth histogram, plus exact min/max, width and physical-order correlation.
inline TableStats analyzeTable(const HeapFile& heap, const Schema& schema, const AnalyzeOptions& opt = {}) {
  const size_t ncols = schema.size();
  std::vector<HyperLogLog> hll(ncols);
  std::vector<SpaceSaving> heavy(ncols, SpaceSaving(opt.sketchCounters));
  std::vector<std::vector<Value>> sample(ncols);
  std::vector<double> widthSum(ncols, 0), ordered(ncols, 0);
  std::vector<Value> prev(ncols);
  TableStats ts;
  ts.cols.resize(ncols);
  Rng rng(opt.seed);

  uint64_t rows = 0;
  double tupleBytes = 0;
  HeapFile::Scanner scan(&heap);
  Tuple t;
  while (scan.next(t)) {
    ++rows;
    tupleBytes += static_cast<double>(encodedSize(t));
    // One reservoir slot decision per row keeps the per-column samples aligned.
    uint64_t slot = rows <= opt.sampleSize ? rows - 1 : rng.uniform(rows);
    for (size_t c = 0; c < ncols; ++c) {
      const Value& v = t[c];
      ColumnStats& cs = ts.cols[c];
      hll[c].add(v.hash());
      heavy[c].add(v);
      widthSum[c] += v.type() == TypeId::String ? 3.0 + static_cast<double>(v.asString().size()) : 9.0;
      if (rows == 1) { cs.min = v; cs.max = v; }
      else {
        if (v.compare(cs.min) < 0) cs.min = v;
        if (v.compare(cs.max) > 0) cs.max = v;
        if (prev[c].compare(v) <= 0) ordered[c] += 1;
      }
      prev[c] = v;
      if (slot < opt.sampleSize) {
        if (sample[c].size() < opt.sampleSize) sample[c].push_back(v);
        else sample[c][slot] = v;
      }
    }
  }

  ts.valid = true;
  ts.rows = static_cast<double>(rows);
  ts.pages = static_cast<double>(heap.numPages());
  ts.avgTupleBytes = rows ? tupleBytes / static_cast<double>(rows) : 0;
  if (rows == 0) {
    for (size_t c = 0; c < ncols; ++c) ts.cols[c].type = schema.cols[c].type;
    return ts;
  }

  for (size_t c = 0; c < ncols; ++c) {
    ColumnStats& cs = ts.cols[c];
    cs.type = schema.cols[c].type;
    cs.ndv = std::min(std::max(1.0, std::round(hll[c].estimate())), ts.rows);
    cs.avgWidth = widthSum[c] / ts.rows;
    cs.correlation = rows > 1 ? ordered[c] / static_cast<double>(rows - 1) : 1.0;

    // Keep values that are provably more common than an average value. If the sketch never
    // evicted anything, its counts are exact and a low-cardinality column keeps every value.
    const double avgFreq = ts.rows / cs.ndv;
    const auto top = heavy[c].top();
    const bool keepAll = heavy[c].exact() && top.size() <= opt.mcvTarget;
    for (const auto& ctr : top) {
      if (cs.mcv.size() >= opt.mcvTarget) break;
      double guaranteed = static_cast<double>(ctr.count - ctr.error);
      if (keepAll || (guaranteed >= 2 && guaranteed > 1.25 * avgFreq)) {
        double frac = static_cast<double>(ctr.count) / ts.rows;
        cs.mcv.emplace_back(ctr.value, frac);
        cs.mcvFraction += frac;
      }
    }
    cs.mcvFraction = std::min(cs.mcvFraction, 1.0);

    if (cs.type != TypeId::Int && cs.type != TypeId::Double) continue;
    std::vector<double> vals;
    vals.reserve(sample[c].size());
    for (const Value& v : sample[c]) {
      bool isMcv = false;
      for (const auto& m : cs.mcv)
        if (m.first.compare(v) == 0) { isMcv = true; break; }
      if (!isMcv) vals.push_back(v.asDouble());
    }
    if (vals.empty()) continue;
    std::sort(vals.begin(), vals.end());
    const size_t nb = std::min(opt.buckets, vals.size());
    const double scale = ts.rows * (1.0 - cs.mcvFraction) / static_cast<double>(vals.size());
    for (size_t b = 0; b < nb; ++b) {
      size_t from = b * vals.size() / nb;
      size_t to = (b + 1) * vals.size() / nb;
      if (to <= from) continue;
      cs.hist.push_back(Bucket{vals[from], vals[to - 1], static_cast<double>(to - from) * scale});
    }
  }
  return ts;
}

}  // namespace sysr
