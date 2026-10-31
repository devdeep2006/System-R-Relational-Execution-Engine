#pragma once

#include <cstring>
#include <utility>

#include "common/value.h"
#include "storage/disk_manager.h"

namespace sysr {

// ---------------------------------------------------------------------------
// Tuple encoding: [u16 column count] then per value [u8 type tag][payload], where
// INT/DOUBLE carry 8 bytes and TEXT carries [u16 length][bytes].
// ---------------------------------------------------------------------------

inline size_t encodedSize(const Tuple& t) {
  size_t n = 2;
  for (const Value& v : t) {
    n += 1;
    switch (v.type()) {
      case TypeId::Int:
      case TypeId::Double: n += 8; break;
      case TypeId::String: n += 2 + v.asString().size(); break;
      case TypeId::Null: break;
    }
  }
  return n;
}

inline void encodeTuple(const Tuple& t, char* out) {
  uint16_t count = static_cast<uint16_t>(t.size());
  std::memcpy(out, &count, 2);
  size_t off = 2;
  for (const Value& v : t) {
    out[off++] = static_cast<char>(v.type());
    switch (v.type()) {
      case TypeId::Int: { int64_t x = v.asInt(); std::memcpy(out + off, &x, 8); off += 8; break; }
      case TypeId::Double: { double x = v.asDouble(); std::memcpy(out + off, &x, 8); off += 8; break; }
      case TypeId::String: {
        const std::string& s = v.asString();
        if (s.size() > 0xFFFF) throw DbError("string value longer than 65535 bytes");
        uint16_t len = static_cast<uint16_t>(s.size());
        std::memcpy(out + off, &len, 2);
        std::memcpy(out + off + 2, s.data(), s.size());
        off += 2 + s.size();
        break;
      }
      case TypeId::Null: break;
    }
  }
}

inline void decodeTuple(const char* in, Tuple& out) {
  uint16_t count;
  std::memcpy(&count, in, 2);
  out.resize(count);
  size_t off = 2;
  for (uint16_t i = 0; i < count; ++i) {
    TypeId type = static_cast<TypeId>(in[off++]);
    switch (type) {
      case TypeId::Int: { int64_t x; std::memcpy(&x, in + off, 8); off += 8; out[i] = Value::Int(x); break; }
      case TypeId::Double: { double x; std::memcpy(&x, in + off, 8); off += 8; out[i] = Value::Double(x); break; }
      case TypeId::String: {
        uint16_t len;
        std::memcpy(&len, in + off, 2);
        out[i] = Value::String(std::string(in + off + 2, len));
        off += 2 + len;
        break;
      }
      case TypeId::Null: out[i] = Value(); break;
    }
  }
}

// ---------------------------------------------------------------------------
// Slotted page: header [u16 slot count][u16 free-space end], a slot array of
// [u16 offset][u16 length] growing forward, and records packed from the end.
// ---------------------------------------------------------------------------
namespace slotted {

constexpr size_t kHeader = 4;
constexpr size_t kSlot = 4;
// Bytes a tuple of encoded size n consumes on a page (record + slot entry).
inline size_t footprint(size_t n) { return n + kSlot; }
constexpr size_t kUsable = kPageSize - kHeader;

inline uint16_t rd16(const char* p, size_t off) { uint16_t v; std::memcpy(&v, p + off, 2); return v; }
inline void wr16(char* p, size_t off, uint16_t v) { std::memcpy(p + off, &v, 2); }

inline void init(char* p) {
  wr16(p, 0, 0);
  wr16(p, 2, static_cast<uint16_t>(kPageSize));
}
inline uint16_t count(const char* p) { return rd16(p, 0); }
inline size_t freeSpace(const char* p) { return rd16(p, 2) - (kHeader + kSlot * count(p)); }

// Returns the slot number, or -1 if the record does not fit.
inline int insert(char* p, const char* rec, size_t len) {
  if (freeSpace(p) < len + kSlot) return -1;
  uint16_t n = count(p);
  uint16_t end = static_cast<uint16_t>(rd16(p, 2) - len);
  std::memcpy(p + end, rec, len);
  wr16(p, kHeader + kSlot * n, end);
  wr16(p, kHeader + kSlot * n + 2, static_cast<uint16_t>(len));
  wr16(p, 0, static_cast<uint16_t>(n + 1));
  wr16(p, 2, end);
  return n;
}

inline const char* record(const char* p, uint16_t slot) {
  if (slot >= count(p)) throw DbError("slotted page: bad slot");
  return p + rd16(p, kHeader + kSlot * slot);
}

}  // namespace slotted

// Upper bound for a single tuple so it always fits on an empty page.
constexpr size_t kMaxTupleBytes = slotted::kUsable - slotted::kSlot;

}  // namespace sysr
