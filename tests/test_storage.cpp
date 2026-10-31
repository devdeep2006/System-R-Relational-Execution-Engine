#include <cstring>

#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "storage/spill_file.h"
#include "test_framework.h"

using namespace sysr;

TEST(tuple_codec_roundtrip) {
  Tuple t{Value::Int(-5), Value::Double(2.5), Value::String("it's"), Value(), Value::String("")};
  std::vector<char> buf(encodedSize(t));
  encodeTuple(t, buf.data());
  Tuple u;
  decodeTuple(buf.data(), u);
  CHECK_EQ(u.size(), t.size());
  for (size_t i = 0; i < t.size(); ++i) CHECK(u[i].compare(t[i]) == 0 && u[i].type() == t[i].type());
}

TEST(value_hash_consistent_with_equality) {
  CHECK(Value::Int(3) == Value::Double(3.0));
  CHECK_EQ(Value::Int(3).hash(), Value::Double(3.0).hash());
  CHECK(Value::Int(2) < Value::Double(2.5));
  CHECK(Value::String("abc") < Value::String("abd"));
}

TEST(slotted_page_fills_and_reads_back) {
  std::vector<char> page(kPageSize);
  slotted::init(page.data());
  int n = 0;
  while (true) {
    std::string s = "record-" + std::to_string(n);
    if (slotted::insert(page.data(), s.data(), s.size()) < 0) break;
    ++n;
  }
  CHECK(n > 200);
  for (int i = 0; i < n; ++i) {
    std::string s = "record-" + std::to_string(i);
    CHECK(std::memcmp(slotted::record(page.data(), static_cast<uint16_t>(i)), s.data(), s.size()) == 0);
  }
}

TEST(heap_file_scan_and_rid_lookup_under_eviction) {
  DiskManager disk;
  BufferPool pool(&disk, 8);
  HeapFile heap(&pool);
  std::vector<RID> rids;
  for (int i = 0; i < 5000; ++i) rids.push_back(heap.insert({Value::Int(i), Value::String("row" + std::to_string(i))}));
  CHECK(heap.numPages() > 8);  // more pages than frames: inserts and scans must evict
  HeapFile::Scanner scan(&heap);
  Tuple t;
  int expect = 0;
  while (scan.next(t)) {
    CHECK_EQ(t[0].asInt(), int64_t{expect});
    ++expect;
  }
  CHECK_EQ(expect, 5000);
  heap.get(rids[1234], t);
  CHECK_EQ(t[1].asString(), std::string("row1234"));
  CHECK_EQ(pool.pinnedFrames(), size_t{0});
}

TEST(buffer_pool_clock_pins_and_writeback) {
  DiskManager disk;
  BufferPool pool(&disk, 4);
  std::vector<page_id_t> ids(5);
  for (int i = 0; i < 4; ++i) {
    char* d = pool.create(&ids[static_cast<size_t>(i)]);
    d[0] = static_cast<char>('a' + i);
  }
  page_id_t extra;
  CHECK_THROWS(pool.create(&extra));  // every frame pinned
  for (int i = 0; i < 4; ++i) pool.unpin(ids[static_cast<size_t>(i)], true);
  char* d = pool.create(&ids[4]);  // evicts one dirty page -> one write-back
  d[0] = 'e';
  pool.unpin(ids[4], true);
  CHECK_EQ(pool.stats().evictions, uint64_t{1});
  CHECK_EQ(disk.stats().writes, uint64_t{1});
  pool.evictAll();
  for (int i = 0; i < 5; ++i) {
    char* p = pool.fetch(ids[static_cast<size_t>(i)]);
    CHECK_EQ(p[0], static_cast<char>('a' + i));
    pool.unpin(ids[static_cast<size_t>(i)], false);
  }
}

TEST(buffer_pool_unpin_errors_and_hits) {
  DiskManager disk;
  BufferPool pool(&disk, 4);
  page_id_t id;
  pool.create(&id);
  pool.unpin(id, true);
  CHECK_THROWS(pool.unpin(id, false));
  pool.fetch(id);
  pool.fetch(id);
  pool.unpin(id, false);
  pool.unpin(id, false);
  CHECK_EQ(pool.stats().hits, uint64_t{2});
}

TEST(spill_file_sequential_and_interleaved_io) {
  DiskManager disk;
  {
    SpillFile f(&disk, 4);
    for (int i = 0; i < 10000; ++i) f.append({Value::Int(i), Value::String("payload")});
    f.finish();
    CHECK_EQ(disk.stats().writes, uint64_t{f.pages()});
    CHECK_EQ(disk.stats().seeks, uint64_t{1});  // one stream of adjacent extents
    disk.resetStats();
    auto r = f.reader(2);
    Tuple t;
    int n = 0;
    while (r.next(t)) CHECK_EQ(t[0].asInt(), int64_t{n++});
    CHECK_EQ(n, 10000);
    CHECK_EQ(disk.stats().reads, uint64_t{f.pages()});
  }
  CHECK_EQ(disk.livePages(), size_t{0});  // spill pages are freed

  // Two files written alternately: their extents interleave, so reading one back costs a
  // seek per extent.
  disk.resetStats();
  SpillFile a(&disk, 2), b(&disk, 2);
  for (int i = 0; i < 4000; ++i) {
    a.append({Value::Int(i)});
    b.append({Value::Int(i)});
  }
  a.finish();
  b.finish();
  disk.resetStats();
  auto r = a.reader(2);
  Tuple t;
  while (r.next(t)) {}
  CHECK_EQ(disk.stats().seeks, uint64_t{(a.pages() + 1) / 2});
}
