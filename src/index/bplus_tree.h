#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "storage/buffer_pool.h"
#include "storage/heap_file.h"

namespace sysr {

// Disk-resident B+ tree over INT keys mapping key -> RID, stored in buffer-pool pages.
// Duplicates are supported by ordering entries on the composite (key, rid), which keeps
// every entry unique and makes splits and range scans straightforward. Leaves are chained
// left-to-right for ordered range scans.
class BPlusTree {
 public:
  struct Entry {
    int64_t key;
    uint64_t rid;
    bool operator<(const Entry& o) const { return key != o.key ? key < o.key : rid < o.rid; }
  };
  static constexpr size_t kHeaderBytes = 8;
  static constexpr size_t kLeafCap = (kPageSize - kHeaderBytes) / sizeof(Entry);  // 255
  static constexpr size_t kInnerCap = 204;  // 204 separators + 205 child ids fit in a page
  static_assert(kHeaderBytes + kInnerCap * sizeof(Entry) + (kInnerCap + 1) * 4 <= kPageSize, "inner node too big");

  explicit BPlusTree(BufferPool* bp) : bp_(bp) {
    char* d;
    root_ = newNode(true, &d);
    bp_->unpin(root_, true);
  }

  void insert(int64_t key, RID rid) {
    Entry e{key, rid.pack()};
    Split s = insertRec(root_, e);
    if (s.split) {
      char* d;
      page_id_t newRoot = newNode(false, &d);
      hdr(d)->count = 1;
      innerKeys(d)[0] = s.sep;
      innerKids(d)[0] = root_;
      innerKids(d)[1] = s.right;
      bp_->unpin(newRoot, true);
      root_ = newRoot;
      ++height_;
    }
    ++size_;
  }

  int height() const { return height_; }
  uint64_t size() const { return size_; }
  size_t numPages() const { return pages_; }
  size_t leafPages() const { return leaves_; }

  // Iterates entries with lo <= key <= hi in (key, rid) order.
  class Cursor {
   public:
    bool next(int64_t& key, RID& rid) {
      while (true) {
        if (pos_ < batch_.size()) {
          const Entry& e = batch_[pos_++];
          if (e.key > hi_) { leaf_ = kInvalidPage; batch_.clear(); pos_ = 0; return false; }
          key = e.key;
          rid = RID::unpack(e.rid);
          return true;
        }
        if (leaf_ == kInvalidPage) return false;
        PageGuard g(bp_, leaf_);
        Header* h = hdr(g.data());
        const Entry* es = leafEntries(g.data());
        size_t start = first_ ? static_cast<size_t>(std::lower_bound(es, es + h->count, Entry{lo_, 0}) - es) : 0;
        first_ = false;
        batch_.assign(es + start, es + h->count);
        pos_ = 0;
        leaf_ = h->next;
      }
    }

   private:
    friend class BPlusTree;
    BufferPool* bp_ = nullptr;
    page_id_t leaf_ = kInvalidPage;
    int64_t lo_ = 0, hi_ = 0;
    bool first_ = true;
    std::vector<Entry> batch_;
    size_t pos_ = 0;
  };

  Cursor range(int64_t lo, int64_t hi) const {
    Cursor c;
    c.bp_ = bp_;
    c.lo_ = lo;
    c.hi_ = hi;
    if (lo > hi) return c;
    Entry target{lo, 0};
    page_id_t pid = root_;
    while (true) {
      PageGuard g(bp_, pid);
      Header* h = hdr(g.data());
      if (h->leaf) break;
      const Entry* ks = innerKeys(g.data());
      size_t idx = static_cast<size_t>(std::upper_bound(ks, ks + h->count, target) - ks);
      pid = innerKids(g.data())[idx];
    }
    c.leaf_ = pid;
    return c;
  }

 private:
  struct Header {
    uint8_t leaf;
    uint8_t pad;
    uint16_t count;
    uint32_t next;
  };
  static_assert(sizeof(Header) == kHeaderBytes, "header layout");

  static Header* hdr(char* p) { return reinterpret_cast<Header*>(p); }
  static Entry* leafEntries(char* p) { return reinterpret_cast<Entry*>(p + kHeaderBytes); }
  static Entry* innerKeys(char* p) { return reinterpret_cast<Entry*>(p + kHeaderBytes); }
  static uint32_t* innerKids(char* p) {
    return reinterpret_cast<uint32_t*>(p + kHeaderBytes + kInnerCap * sizeof(Entry));
  }

  struct Split {
    bool split = false;
    Entry sep{0, 0};
    page_id_t right = kInvalidPage;
  };

  page_id_t newNode(bool leaf, char** data) {
    page_id_t pid;
    char* d = bp_->create(&pid);
    Header* h = hdr(d);
    h->leaf = leaf ? 1 : 0;
    h->count = 0;
    h->next = kInvalidPage;
    ++pages_;
    if (leaf) ++leaves_;
    *data = d;
    return pid;
  }

  // Inserts into the subtree rooted at pid. Inner-node invariant: child[i] holds entries
  // < key[i] and child[i+1] holds entries >= key[i].
  Split insertRec(page_id_t pid, const Entry& e) {
    PageGuard g(bp_, pid);
    char* p = g.data();
    Header* h = hdr(p);
    size_t n = h->count;

    if (h->leaf) {
      Entry* es = leafEntries(p);
      size_t pos = static_cast<size_t>(std::upper_bound(es, es + n, e) - es);
      g.markDirty();
      if (n < kLeafCap) {
        std::memmove(es + pos + 1, es + pos, (n - pos) * sizeof(Entry));
        es[pos] = e;
        h->count = static_cast<uint16_t>(n + 1);
        return {};
      }
      std::vector<Entry> all(es, es + n);
      all.insert(all.begin() + static_cast<std::ptrdiff_t>(pos), e);
      size_t half = all.size() / 2;
      char* rp;
      page_id_t rpid = newNode(true, &rp);
      std::copy(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(half), es);
      h->count = static_cast<uint16_t>(half);
      std::copy(all.begin() + static_cast<std::ptrdiff_t>(half), all.end(), leafEntries(rp));
      hdr(rp)->count = static_cast<uint16_t>(all.size() - half);
      hdr(rp)->next = h->next;
      h->next = rpid;
      bp_->unpin(rpid, true);
      return {true, all[half], rpid};
    }

    Entry* ks = innerKeys(p);
    uint32_t* kids = innerKids(p);
    size_t idx = static_cast<size_t>(std::upper_bound(ks, ks + n, e) - ks);
    Split s = insertRec(kids[idx], e);
    if (!s.split) return {};
    g.markDirty();
    if (n < kInnerCap) {
      std::memmove(ks + idx + 1, ks + idx, (n - idx) * sizeof(Entry));
      ks[idx] = s.sep;
      std::memmove(kids + idx + 2, kids + idx + 1, (n - idx) * sizeof(uint32_t));
      kids[idx + 1] = s.right;
      h->count = static_cast<uint16_t>(n + 1);
      return {};
    }
    std::vector<Entry> keys(ks, ks + n);
    std::vector<uint32_t> ch(kids, kids + n + 1);
    keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(idx), s.sep);
    ch.insert(ch.begin() + static_cast<std::ptrdiff_t>(idx) + 1, s.right);
    size_t mid = keys.size() / 2;  // keys[mid] moves up
    char* rp;
    page_id_t rpid = newNode(false, &rp);
    std::copy(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(mid), ks);
    std::copy(ch.begin(), ch.begin() + static_cast<std::ptrdiff_t>(mid) + 1, kids);
    h->count = static_cast<uint16_t>(mid);
    std::copy(keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1, keys.end(), innerKeys(rp));
    std::copy(ch.begin() + static_cast<std::ptrdiff_t>(mid) + 1, ch.end(), innerKids(rp));
    hdr(rp)->count = static_cast<uint16_t>(keys.size() - mid - 1);
    bp_->unpin(rpid, true);
    return {true, keys[mid], rpid};
  }

  BufferPool* bp_;
  page_id_t root_ = kInvalidPage;
  int height_ = 1;
  uint64_t size_ = 0;
  size_t pages_ = 0, leaves_ = 0;
};

}  // namespace sysr
