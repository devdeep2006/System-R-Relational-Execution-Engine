#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "common/value.h"

namespace sysr {

constexpr size_t kPageSize = 4096;
using page_id_t = uint32_t;
constexpr page_id_t kInvalidPage = 0xFFFFFFFFu;

// Block-level I/O counters. A request that does not continue exactly where the previous
// request ended is charged one seek; every page moved is one transfer. These are the
// quantities the cost model predicts (t_S per seek, t_T per transfer).
struct IOStats {
  uint64_t reads = 0, writes = 0, seeks = 0;
  uint64_t transfers() const { return reads + writes; }
  IOStats operator-(const IOStats& o) const { return {reads - o.reads, writes - o.writes, seeks - o.seeks}; }
};

// Simulated disk: an array of 4 KB pages held in memory. Pages are allocated as contiguous
// extents so that sequential access patterns cost one seek per request run, as on a real disk.
class DiskManager {
 public:
  page_id_t allocate(size_t n = 1) {
    page_id_t first = static_cast<page_id_t>(pages_.size());
    for (size_t i = 0; i < n; ++i) pages_.emplace_back(new char[kPageSize]());
    live_ += n;
    return first;
  }

  void free(page_id_t id) {
    if (id < pages_.size() && pages_[id]) {
      pages_[id].reset();
      --live_;
    }
  }

  void read(page_id_t first, size_t n, char* dst) {
    account(first, n);
    stats_.reads += n;
    for (size_t i = 0; i < n; ++i) std::memcpy(dst + i * kPageSize, page(first + i), kPageSize);
  }

  void write(page_id_t first, size_t n, const char* src) {
    account(first, n);
    stats_.writes += n;
    for (size_t i = 0; i < n; ++i) std::memcpy(page(first + i), src + i * kPageSize, kPageSize);
  }

  const IOStats& stats() const { return stats_; }
  void resetStats() { stats_ = IOStats{}; head_ = kInvalidPage; }
  size_t livePages() const { return live_; }

 private:
  char* page(page_id_t id) {
    if (id >= pages_.size() || !pages_[id]) throw DbError("disk: access to unallocated page " + std::to_string(id));
    return pages_[id].get();
  }
  void account(page_id_t first, size_t n) {
    if (first != head_) ++stats_.seeks;
    head_ = first + static_cast<page_id_t>(n);
  }

  std::vector<std::unique_ptr<char[]>> pages_;
  size_t live_ = 0;
  page_id_t head_ = kInvalidPage;
  IOStats stats_;
};

}  // namespace sysr
