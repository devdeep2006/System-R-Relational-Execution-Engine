#include <chrono>
#include <cstring>

#include "test_framework.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int passed = 0, failed = 0;
  auto start = std::chrono::steady_clock::now();
  for (const auto& tc : sysrtest::registry()) {
    if (filter && !std::strstr(tc.name, filter)) continue;
    auto t0 = std::chrono::steady_clock::now();
    try {
      tc.fn();
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::cout << "[PASS] " << tc.name << " (" << static_cast<int>(ms) << " ms)\n";
      ++passed;
    } catch (const sysrtest::Failure& f) {
      std::cout << "[FAIL] " << tc.name << "\n       " << f.where << ": " << f.what << "\n";
      ++failed;
    } catch (const std::exception& e) {
      std::cout << "[FAIL] " << tc.name << "\n       unexpected exception: " << e.what() << "\n";
      ++failed;
    }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::cout << "\n" << passed << " passed, " << failed << " failed (" << secs << " s)\n";
  return failed == 0 ? 0 : 1;
}
