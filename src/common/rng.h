#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "common/value.h"

namespace sysr {

// Deterministic PRNG so generated data, samples and randomized tests are reproducible.
class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed ? seed : 1) {}
  uint64_t next() { state_ += 0x9e3779b97f4a7c15ULL; return mix64(state_); }
  uint64_t uniform(uint64_t n) { return n ? next() % n : 0; }
  int64_t range(int64_t lo, int64_t hi) { return lo + static_cast<int64_t>(uniform(static_cast<uint64_t>(hi - lo + 1))); }
  double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  double normal() {  // Box-Muller
    double u1 = std::max(unit(), 1e-12), u2 = unit();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

 private:
  uint64_t state_;
};

// Zipf(n, s) sampler over ranks 0..n-1 via an inverted CDF.
class Zipf {
 public:
  Zipf(size_t n, double s) : cdf_(n) {
    double sum = 0;
    for (size_t i = 0; i < n; ++i) { sum += 1.0 / std::pow(static_cast<double>(i + 1), s); cdf_[i] = sum; }
    for (double& c : cdf_) c /= sum;
  }
  size_t sample(Rng& rng) const {
    double u = rng.unit();
    size_t i = static_cast<size_t>(std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
    return std::min(i, cdf_.size() - 1);
  }

 private:
  std::vector<double> cdf_;
};

}  // namespace sysr
