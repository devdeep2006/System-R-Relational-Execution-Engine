#pragma once

#include "engine/database.h"

namespace sysr {

struct DemoDataOptions {
  bool large = true;    // huge_users (100K) and huge_transactions (500K)
  bool skewed = true;   // customers (10K) and orders (200K) with Zipf / log-normal skew
  uint64_t seed = 7;
};

// Deterministic demo schema. The university tables and the huge_* tables reproduce the
// original project's data exactly; customers/orders add skew for estimator experiments.
void loadDemoData(Database& db, const DemoDataOptions& opt = {});

}  // namespace sysr
