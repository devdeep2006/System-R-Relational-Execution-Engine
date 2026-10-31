#pragma once

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "storage/disk_manager.h"

namespace sysr {

struct BufferStats {
  uint64_t hits = 0, misses = 0, evictions = 0, writebacks = 0;
};

// Fixed-size buffer pool with pin counts, dirty tracking and CLOCK (second-chance) replacement.
class BufferPool {
 public:
  BufferPool(DiskManager* disk, size_t frames) : disk_(disk), frames_(frames) {
    if (frames < 4) throw DbError("buffer pool needs at least 4 frames");
    for (size_t i = 0; i < frames; ++i) {
      frames_[i].data.reset(new char[kPageSize]());
      free_.push_back(frames - 1 - i);
    }
  }

  // Pins the page and returns its frame. The caller must unpin it.
  char* fetch(page_id_t pid) {
    auto it = table_.find(pid);
    if (it != table_.end()) {
      Frame& f = frames_[it->second];
      ++stats_.hits;
      ++f.pins;
      f.ref = true;
      return f.data.get();
    }
    ++stats_.misses;
    size_t idx = acquireFrame();
    Frame& f = frames_[idx];
    disk_->read(pid, 1, f.data.get());
    f.pid = pid;
    f.pins = 1;
    f.ref = true;
    f.dirty = false;
    table_[pid] = idx;
    return f.data.get();
  }

  // Allocates a fresh zeroed page on disk and pins it in the pool (dirty).
  char* create(page_id_t* out) {
    page_id_t pid = disk_->allocate(1);
    size_t idx = acquireFrame();
    Frame& f = frames_[idx];
    std::memset(f.data.get(), 0, kPageSize);
    f.pid = pid;
    f.pins = 1;
    f.ref = true;
    f.dirty = true;
    table_[pid] = idx;
    *out = pid;
    return f.data.get();
  }

  void unpin(page_id_t pid, bool dirty) {
    auto it = table_.find(pid);
    if (it == table_.end()) throw DbError("buffer pool: unpin of non-resident page");
    Frame& f = frames_[it->second];
    if (f.pins <= 0) throw DbError("buffer pool: unpin of unpinned page");
    --f.pins;
    f.dirty = f.dirty || dirty;
  }

  void flushAll() {
    for (Frame& f : frames_) {
      if (f.pid != kInvalidPage && f.dirty) {
        disk_->write(f.pid, 1, f.data.get());
        ++stats_.writebacks;
        f.dirty = false;
      }
    }
  }

  // Flushes and drops every unpinned frame, giving the next query a cold cache.
  void evictAll() {
    flushAll();
    for (size_t i = 0; i < frames_.size(); ++i) {
      Frame& f = frames_[i];
      if (f.pid != kInvalidPage && f.pins == 0) {
        table_.erase(f.pid);
        f.pid = kInvalidPage;
        f.ref = false;
        free_.push_back(i);
      }
    }
  }

  size_t frames() const { return frames_.size(); }
  size_t pinnedFrames() const {
    size_t n = 0;
    for (const Frame& f : frames_) n += f.pins > 0;
    return n;
  }
  const BufferStats& stats() const { return stats_; }
  void resetStats() { stats_ = BufferStats{}; }

 private:
  struct Frame {
    page_id_t pid = kInvalidPage;
    int pins = 0;
    bool dirty = false;
    bool ref = false;
    std::unique_ptr<char[]> data;
  };

  size_t acquireFrame() {
    if (!free_.empty()) {
      size_t idx = free_.back();
      free_.pop_back();
      return idx;
    }
    // Two full sweeps clear every reference bit, so a third finds a victim unless all are pinned.
    for (size_t step = 0; step < 3 * frames_.size(); ++step) {
      size_t idx = hand_;
      hand_ = (hand_ + 1) % frames_.size();
      Frame& f = frames_[idx];
      if (f.pins > 0) continue;
      if (f.ref) { f.ref = false; continue; }
      if (f.dirty) {
        disk_->write(f.pid, 1, f.data.get());
        ++stats_.writebacks;
      }
      table_.erase(f.pid);
      ++stats_.evictions;
      f.pid = kInvalidPage;
      f.dirty = false;
      return idx;
    }
    throw DbError("buffer pool exhausted: every frame is pinned");
  }

  DiskManager* disk_;
  std::vector<Frame> frames_;
  std::unordered_map<page_id_t, size_t> table_;
  std::vector<size_t> free_;
  size_t hand_ = 0;
  BufferStats stats_;
};

// RAII pin on a buffer-pool page.
class PageGuard {
 public:
  PageGuard(BufferPool* bp, page_id_t pid) : bp_(bp), pid_(pid), data_(bp->fetch(pid)) {}
  ~PageGuard() { if (bp_) bp_->unpin(pid_, dirty_); }
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;

  char* data() { return data_; }
  page_id_t id() const { return pid_; }
  void markDirty() { dirty_ = true; }

 private:
  BufferPool* bp_;
  page_id_t pid_;
  char* data_;
  bool dirty_ = false;
};

}  // namespace sysr
