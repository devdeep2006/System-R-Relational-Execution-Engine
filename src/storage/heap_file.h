#pragma once

#include <vector>

#include "storage/buffer_pool.h"
#include "storage/page.h"

namespace sysr {

struct RID {
  page_id_t page = kInvalidPage;
  uint16_t slot = 0;
  uint64_t pack() const { return (static_cast<uint64_t>(page) << 16) | slot; }
  static RID unpack(uint64_t v) { return RID{static_cast<page_id_t>(v >> 16), static_cast<uint16_t>(v & 0xFFFF)}; }
};

// Unordered heap of slotted pages, accessed only through the buffer pool.
class HeapFile {
 public:
  explicit HeapFile(BufferPool* bp) : bp_(bp) {}

  RID insert(const Tuple& t) {
    size_t len = encodedSize(t);
    if (len > kMaxTupleBytes) throw DbError("tuple too large for a page");
    buf_.resize(len);
    encodeTuple(t, buf_.data());
    if (!pages_.empty()) {
      PageGuard g(bp_, pages_.back());
      int slot = slotted::insert(g.data(), buf_.data(), len);
      if (slot >= 0) {
        g.markDirty();
        ++count_;
        return RID{pages_.back(), static_cast<uint16_t>(slot)};
      }
    }
    page_id_t pid;
    char* data = bp_->create(&pid);
    slotted::init(data);
    int slot = slotted::insert(data, buf_.data(), len);
    bp_->unpin(pid, true);
    pages_.push_back(pid);
    ++count_;
    return RID{pid, static_cast<uint16_t>(slot)};
  }

  void get(RID rid, Tuple& out) const {
    PageGuard g(bp_, rid.page);
    decodeTuple(slotted::record(g.data(), rid.slot), out);
  }

  size_t numPages() const { return pages_.size(); }
  uint64_t numTuples() const { return count_; }
  const std::vector<page_id_t>& pages() const { return pages_; }

  // Sequential scan. Each page is pinned once, decoded, and unpinned immediately,
  // so a scan holds at most one frame.
  class Scanner {
   public:
    explicit Scanner(const HeapFile* f) : f_(f) {}
    bool next(Tuple& out, RID* rid = nullptr) {
      while (pos_ >= batch_.size()) {
        if (page_ >= f_->pages_.size()) return false;
        load(page_++);
      }
      if (rid) *rid = RID{f_->pages_[page_ - 1], static_cast<uint16_t>(pos_)};
      out = std::move(batch_[pos_++]);
      return true;
    }

   private:
    void load(size_t i) {
      PageGuard g(f_->bp_, f_->pages_[i]);
      uint16_t n = slotted::count(g.data());
      batch_.resize(n);
      for (uint16_t s = 0; s < n; ++s) decodeTuple(slotted::record(g.data(), s), batch_[s]);
      pos_ = 0;
    }
    const HeapFile* f_;
    size_t page_ = 0;
    std::vector<Tuple> batch_;
    size_t pos_ = 0;
  };

 private:
  BufferPool* bp_;
  std::vector<page_id_t> pages_;
  uint64_t count_ = 0;
  std::vector<char> buf_;
};

}  // namespace sysr
