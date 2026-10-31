#pragma once

#include <algorithm>
#include <cmath>

namespace sysr {

struct CostParams {
  double seekMs = 10.0;        // t_S: average seek + rotational latency
  double transferMs = 0.1;     // t_T: transfer of one 4 KB block
  double cpuTupleMs = 0.0002;  // per-tuple CPU work (hashing, comparing, copying)
  size_t memPages = 64;        // M: work memory per blocking operator, in pages
  size_t ioChunk = 4;          // b_b: pages per spill read/write request
  size_t bufferFrames = 256;   // buffer pool size (for cache-aware index costing)
};

struct Cost {
  double transfers = 0, seeks = 0, cpu = 0;
  double ms(const CostParams& p) const { return seeks * p.seekMs + transfers * p.transferMs + cpu * p.cpuTupleMs; }
  Cost& operator+=(const Cost& o) { transfers += o.transfers; seeks += o.seeks; cpu += o.cpu; return *this; }
  friend Cost operator+(Cost a, const Cost& b) { return a += b; }
};

// Each function returns the work an operator adds on top of its inputs, mirroring the
// physical implementation (see execution/operators.cpp) so estimates can be checked against
// the disk's measured transfers and seeks.
namespace costmodel {

inline double log2c(double x) { return std::log2(std::max(2.0, x)); }

// Expected distinct pages touched by k random fetches over n pages (Cardenas' formula).
inline double cardenas(double n, double k) {
  if (n <= 1) return std::min(n, k);
  return n * (1.0 - std::pow(1.0 - 1.0 / n, k));
}

inline Cost seqScan(double pages, double rows) { return Cost{pages, pages > 0 ? 1.0 : 0.0, rows}; }

inline int sortPasses(double pages, const CostParams& p) {
  double M = static_cast<double>(p.memPages);
  if (pages <= M) return 0;
  double runs = std::ceil(pages / M);
  double fanIn = std::max(2.0, std::floor(M / static_cast<double>(p.ioChunk)) - 1.0);
  return static_cast<int>(std::ceil(std::log(runs) / std::log(fanIn) - 1e-9));
}

// External merge sort: pass 0 writes ceil(b/M) sorted runs (one request each); every merge
// pass reads all runs b_b pages at a time; all but the last pass also write their output.
inline Cost externalSort(double pages, double rows, const CostParams& p) {
  Cost c;
  c.cpu = rows * log2c(rows);
  int passes = sortPasses(pages, p);
  if (passes == 0) return c;
  double runs = std::ceil(pages / static_cast<double>(p.memPages));
  double chunks = std::ceil(pages / static_cast<double>(p.ioChunk));
  c.transfers = 2.0 * pages * passes;
  c.seeks = runs + chunks * (2.0 * passes - 1.0);
  c.cpu += rows * passes;
  return c;
}

inline bool hashFits(double buildPages, const CostParams& p) {
  return buildPages <= static_cast<double>(p.memPages) - 2.0;
}

inline double gracePartitions(double buildPages, const CostParams& p) {
  double M = static_cast<double>(p.memPages);
  return std::min(M - 1.0, std::max(2.0, std::ceil(1.2 * buildPages / (M - 2.0))));
}

// Hash join: in-memory when the build side fits in M-2 pages; otherwise Grace hash join that
// partitions both inputs to disk (recursively when a partition still does not fit).
inline Cost hashJoin(double buildPages, double buildRows, double probePages, double probeRows, const CostParams& p) {
  Cost c;
  c.cpu = 2.0 * buildRows + probeRows;
  if (hashFits(buildPages, p)) return c;
  double M = static_cast<double>(p.memPages);
  double parts = gracePartitions(buildPages, p);
  double bb = std::max(1.0, std::floor((M - 1.0) / parts));
  double levels = std::max(1.0, std::ceil(std::log(buildPages / (M - 2.0)) / std::log(M - 1.0) - 1e-9));
  c.transfers = levels * (2.0 * (buildPages + probePages) + 2.0 * parts);
  c.seeks = levels * (2.0 * (std::ceil(buildPages / bb) + std::ceil(probePages / bb)) + 2.0 * parts);
  c.cpu += levels * (buildRows + probeRows);
  return c;
}

// Block nested loop: if the inner fits in M-2 pages it is held in memory and the outer is
// streamed once; otherwise the inner is spilled once and rescanned for every (M-2)-page
// block of the outer. Equi-join keys are hashed within the in-memory block.
inline Cost nestedLoop(double outerPages, double outerRows, double innerPages, double innerRows, bool hashed,
                       const CostParams& p) {
  Cost c;
  double M = static_cast<double>(p.memPages);
  if (innerPages <= M - 2.0) {
    c.cpu = hashed ? innerRows + outerRows : outerRows * innerRows;
    return c;
  }
  double blocks = std::max(1.0, std::ceil(outerPages / (M - 2.0)));
  double chunks = std::ceil(innerPages / static_cast<double>(p.ioChunk));
  c.transfers = innerPages * (1.0 + blocks);
  // Spilling the inner interleaves its writes with reads of the inner input (2 seeks per
  // chunk); afterwards each outer block costs one seek to resume the outer and one to
  // restart the sequential inner scan.
  c.seeks = 2.0 * chunks + 2.0 * blocks;
  c.cpu = hashed ? outerRows + blocks * innerRows : outerRows * innerRows;
  return c;
}

// B+ tree range scan: descend the inner levels, walk the matching leaves, fetch heap rows.
// A clustered index reads the heap sequentially; an unclustered one fetches a page per row
// unless the touched pages fit in the buffer pool.
inline Cost indexScan(double selectivity, double matchedRows, int height, double leafPages, double heapPages,
                      bool clustered, const CostParams& p) {
  Cost c;
  double leaves = std::max(1.0, std::ceil(selectivity * leafPages));
  c.transfers = (height - 1) + leaves;
  c.seeks = (height - 1) + leaves;
  if (clustered) {
    double hp = std::max(1.0, std::ceil(selectivity * heapPages));
    c.transfers += hp;
    c.seeks += leaves;  // leaf and heap reads interleave
  } else {
    double distinct = costmodel::cardenas(heapPages, matchedRows);
    double fetches = heapPages <= static_cast<double>(p.bufferFrames) ? distinct : matchedRows;
    c.transfers += fetches;
    c.seeks += fetches;
  }
  c.cpu = 2.0 * matchedRows;
  return c;
}

// Index nested loop: one B+ tree probe per outer row plus heap fetches for matches.
// Random probes are costed with a steady-state LRU approximation over the index + heap
// pages. When the outer arrives (nearly) sorted on the join key, the probes sweep the index
// in key order: `probeRuns` ascending runs each touch every needed page about once.
inline Cost indexNestedLoop(double outerRows, double outerPages, double fetchedRows, double probeRuns, int height,
                            double leafPages, double heapPages, const CostParams& p) {
  Cost c;
  double frames = static_cast<double>(p.bufferFrames);
  double total = leafPages + heapPages + std::max(0, height - 1);
  double accesses = outerRows + fetchedRows;
  double randomIo;
  if (total <= frames) randomIo = costmodel::cardenas(total, accesses);
  else randomIo = std::min(accesses, frames + accesses * (1.0 - frames / total));
  double runs = std::max(1.0, std::min(probeRuns, outerRows));
  double sweepIo = runs * costmodel::cardenas(total, accesses / runs);
  double io = std::min(randomIo, sweepIo);
  c.transfers = io;
  c.seeks = io + std::min(io, outerPages);  // inner reads also interrupt the outer's sequential scan
  c.cpu = outerRows * (height + 1) + fetchedRows;
  return c;
}

inline Cost hashAggregate(double rows) { return Cost{0, 0, 2.0 * rows}; }
inline Cost topN(double rows, double k) { return Cost{0, 0, rows * log2c(k + 1)}; }

}  // namespace costmodel
}  // namespace sysr
