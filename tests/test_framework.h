#pragma once

// Minimal self-contained test harness (no external dependencies).

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace sysrtest {

struct Failure {
  std::string where, what;
};

struct TestCase {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template <typename A, typename B>
std::string describe(const A& a, const B& b) {
  std::ostringstream s;
  s << a << " vs " << b;
  return s.str();
}

}  // namespace sysrtest

#define SYSR_CAT2(a, b) a##b
#define SYSR_CAT(a, b) SYSR_CAT2(a, b)
#define TEST(name)                                                              \
  static void name();                                                           \
  static ::sysrtest::Registrar SYSR_CAT(registrar_, name)(#name, name);         \
  static void name()

#define SYSR_WHERE (std::string(__FILE__) + ":" + std::to_string(__LINE__))
#define CHECK(cond) \
  do { if (!(cond)) throw ::sysrtest::Failure{SYSR_WHERE, "CHECK(" #cond ")"}; } while (0)
#define CHECK_EQ(a, b)                                                                                    \
  do {                                                                                                    \
    auto&& _a = (a);                                                                                      \
    auto&& _b = (b);                                                                                      \
    if (!(_a == _b)) throw ::sysrtest::Failure{SYSR_WHERE, "CHECK_EQ(" #a ", " #b "): " + ::sysrtest::describe(_a, _b)}; \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                             \
  do {                                                                                                    \
    double _a = (a), _b = (b);                                                                            \
    if (std::fabs(_a - _b) > (tol)) throw ::sysrtest::Failure{SYSR_WHERE, "CHECK_NEAR(" #a ", " #b "): " + ::sysrtest::describe(_a, _b)}; \
  } while (0)
#define CHECK_THROWS(stmt)                                                                \
  do {                                                                                    \
    bool _threw = false;                                                                  \
    try { stmt; } catch (...) { _threw = true; }                                          \
    if (!_threw) throw ::sysrtest::Failure{SYSR_WHERE, "expected exception: " #stmt};     \
  } while (0)
