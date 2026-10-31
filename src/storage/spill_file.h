#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/page.h"

namespace sysr {

// Temporary run/partition file used by external sort, Grace hash join and block nested loop.
// It bypasses the buffer pool (direct I/O): tuples are packed into slotted pages in a private
// output buffer of `chunkPages` pages, and each full buffer is written as ONE request to a
// freshly allocated contiguous extent. Readers fetch up to `chunk` pages per request.
// This is exactly the b_b ("buffer blocks per I/O") parameter of the textbook cost formulas.
class SpillFile {
 public:
  SpillFile(DiskManager* disk, size_t chunkPages)
      : disk_(disk), chunk_(std::max<size_t>(1, chunkPages)), buf_(chunk_ * kPageSize) {
    slotted::init(cur());
  }
  ~SpillFile() {
    for (auto& e : extents_)
      for (size_t i = 0; i < e.second; ++i) disk_->free(e.first + static_cast<page_id_t>(i));
  }
  SpillFile(const SpillFile&) = delete;
  SpillFile& operator=(const SpillFile&) = delete;

  void append(const Tuple& t) {
    size_t len = encodedSize(t);
    if (len > kMaxTupleBytes) throw DbError("tuple too large to spill");
    tmp_.resize(len);
    encodeTuple(t, tmp_.data());
    if (slotted::insert(cur(), tmp_.data(), len) < 0) {
      sealPage();
      slotted::insert(cur(), tmp_.data(), len);
    }
    ++tuples_;
  }

  // Writes out whatever is still buffered. Must be called before reading.
  void finish() {
    if (slotted::count(cur()) > 0) sealPage();
    flush();
  }

  uint64_t tuples() const { return tuples_; }
  size_t pages() const { return pages_; }

  class Reader {
   public:
    Reader(const SpillFile* f, size_t chunkPages) : f_(f), chunk_(std::max<size_t>(1, chunkPages)) {}
    bool next(Tuple& out) {
      while (pos_ >= batch_.size()) {
        if (!load()) return false;
      }
      out = std::move(batch_[pos_++]);
      return true;
    }

   private:
    bool load() {
      while (ext_ < f_->extents_.size() && off_ >= f_->extents_[ext_].second) { ++ext_; off_ = 0; }
      if (ext_ >= f_->extents_.size()) return false;
      const auto& e = f_->extents_[ext_];
      size_t n = std::min(chunk_, e.second - off_);
      buf_.resize(n * kPageSize);
      f_->disk_->read(e.first + static_cast<page_id_t>(off_), n, buf_.data());
      off_ += n;
      batch_.clear();
      pos_ = 0;
      for (size_t p = 0; p < n; ++p) {
        const char* page = buf_.data() + p * kPageSize;
        uint16_t count = slotted::count(page);
        for (uint16_t s = 0; s < count; ++s) {
          batch_.emplace_back();
          decodeTuple(slotted::record(page, s), batch_.back());
        }
      }
      return true;
    }
    const SpillFile* f_;
    size_t chunk_;
    size_t ext_ = 0, off_ = 0;
    std::vector<char> buf_;
    std::vector<Tuple> batch_;
    size_t pos_ = 0;
  };

  Reader reader(size_t chunkPages) const { return Reader(this, chunkPages); }

 private:
  char* cur() { return buf_.data() + filled_ * kPageSize; }
  void sealPage() {
    ++filled_;
    if (filled_ == chunk_) flush();
    slotted::init(cur());
  }
  void flush() {
    if (filled_ == 0) return;
    page_id_t first = disk_->allocate(filled_);
    disk_->write(first, filled_, buf_.data());
    extents_.emplace_back(first, filled_);
    pages_ += filled_;
    filled_ = 0;
    slotted::init(cur());
  }

  DiskManager* disk_;
  size_t chunk_;
  std::vector<char> buf_;
  size_t filled_ = 0;  // sealed pages currently buffered
  std::vector<std::pair<page_id_t, size_t>> extents_;
  size_t pages_ = 0;
  uint64_t tuples_ = 0;
  std::vector<char> tmp_;
};

}  // namespace sysr
