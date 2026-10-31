#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace sysr {

// Every user-facing failure (parse, bind, execution) is reported through DbError.
struct DbError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

enum class TypeId : uint8_t { Null = 0, Int = 1, Double = 2, String = 3 };

inline const char* typeName(TypeId t) {
  switch (t) {
    case TypeId::Null: return "NULL";
    case TypeId::Int: return "INT";
    case TypeId::Double: return "DOUBLE";
    case TypeId::String: return "TEXT";
  }
  return "?";
}

// SplitMix64 finalizer: cheap, well-distributed 64-bit mixing for hash tables,
// partitioning and sketches.
inline uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

class Value {
 public:
  Value() = default;
  static Value Int(int64_t v) { Value x; x.type_ = TypeId::Int; x.i_ = v; return x; }
  static Value Double(double v) { Value x; x.type_ = TypeId::Double; x.d_ = v; return x; }
  static Value String(std::string v) { Value x; x.type_ = TypeId::String; x.s_ = std::move(v); return x; }

  TypeId type() const { return type_; }
  bool isNull() const { return type_ == TypeId::Null; }
  bool isNumeric() const { return type_ == TypeId::Int || type_ == TypeId::Double; }
  int64_t asInt() const { return type_ == TypeId::Double ? static_cast<int64_t>(d_) : i_; }
  double asDouble() const { return type_ == TypeId::Int ? static_cast<double>(i_) : d_; }
  const std::string& asString() const { return s_; }

  bool truthy() const {
    switch (type_) {
      case TypeId::Int: return i_ != 0;
      case TypeId::Double: return d_ != 0.0;
      case TypeId::String: return !s_.empty();
      default: return false;
    }
  }

  // Three-way comparison. INT and DOUBLE compare by numeric value; NULL sorts first;
  // values of incomparable types order by type id (the binder rejects such comparisons).
  int compare(const Value& o) const {
    if (type_ == o.type_) {
      switch (type_) {
        case TypeId::Null: return 0;
        case TypeId::Int: return i_ < o.i_ ? -1 : (i_ > o.i_ ? 1 : 0);
        case TypeId::Double: return d_ < o.d_ ? -1 : (d_ > o.d_ ? 1 : 0);
        case TypeId::String: {
          int c = s_.compare(o.s_);
          return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
      }
    }
    if (isNumeric() && o.isNumeric()) {
      double a = asDouble(), b = o.asDouble();
      return a < b ? -1 : (a > b ? 1 : 0);
    }
    return type_ < o.type_ ? -1 : 1;
  }
  bool operator==(const Value& o) const { return compare(o) == 0; }
  bool operator!=(const Value& o) const { return compare(o) != 0; }
  bool operator<(const Value& o) const { return compare(o) < 0; }

  // Equal values hash equally, including INT 3 and DOUBLE 3.0.
  uint64_t hash() const {
    switch (type_) {
      case TypeId::Null: return 0x5bd1e995ULL;
      case TypeId::Int: return mix64(static_cast<uint64_t>(i_));
      case TypeId::Double: {
        if (d_ == std::floor(d_) && std::fabs(d_) < 9.0e18)
          return mix64(static_cast<uint64_t>(static_cast<int64_t>(d_)));
        uint64_t bits;
        std::memcpy(&bits, &d_, sizeof bits);
        return mix64(bits);
      }
      case TypeId::String: {
        uint64_t h = 1469598103934665603ULL;  // FNV-1a
        for (unsigned char c : s_) { h ^= c; h *= 1099511628211ULL; }
        return mix64(h);
      }
    }
    return 0;
  }

  std::string toString() const {
    switch (type_) {
      case TypeId::Null: return "NULL";
      case TypeId::Int: return std::to_string(i_);
      case TypeId::Double: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.4f", d_);
        std::string s(buf);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.push_back('0');
        return s;
      }
      case TypeId::String: return s_;
    }
    return "?";
  }

 private:
  TypeId type_ = TypeId::Null;
  int64_t i_ = 0;
  double d_ = 0.0;
  std::string s_;
};

using Tuple = std::vector<Value>;

struct ValueHash {
  size_t operator()(const Value& v) const { return static_cast<size_t>(v.hash()); }
};
struct ValueEq {
  bool operator()(const Value& a, const Value& b) const { return a.compare(b) == 0; }
};
struct TupleHash {
  size_t operator()(const Tuple& t) const {
    uint64_t h = 0x84222325cbf29ce4ULL;
    for (const Value& v : t) h = mix64(h ^ v.hash());
    return static_cast<size_t>(h);
  }
};
struct TupleEq {
  bool operator()(const Tuple& a, const Tuple& b) const {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (a[i].compare(b[i]) != 0) return false;
    return true;
  }
};

struct Column {
  std::string name;
  TypeId type;
};

struct Schema {
  std::vector<Column> cols;
  int indexOf(const std::string& name) const {
    for (size_t i = 0; i < cols.size(); ++i)
      if (cols[i].name == name) return static_cast<int>(i);
    return -1;
  }
  size_t size() const { return cols.size(); }
};

}  // namespace sysr
