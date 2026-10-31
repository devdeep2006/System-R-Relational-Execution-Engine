#include "engine/datagen.h"

#include <cmath>

#include "common/rng.h"

namespace sysr {

void loadDemoData(Database& db, const DemoDataOptions& opt) {
  const TypeId I = TypeId::Int, D = TypeId::Double, S = TypeId::String;
  auto str = [](const std::string& s) { return Value::String(s); };
  auto num = [](int64_t v) { return Value::Int(v); };

  db.createTable("students", {{"id", I}, {"name", S}, {"age", I}});
  for (int i = 1; i <= 100; ++i) db.insert("students", {num(i), str("Student" + std::to_string(i)), num(18 + i % 5)});

  db.createTable("grades", {{"student_id", I}, {"course", S}, {"grade", I}});
  for (int i = 1; i <= 500; ++i)
    db.insert("grades", {num(1 + i % 100), str("CS" + std::to_string(i % 10)), num(60 + i % 41)});

  db.createTable("enrollments", {{"student_id", I}, {"course_id", S}, {"semester", S}});
  for (int i = 1; i <= 100; ++i)
    db.insert("enrollments", {num(1 + i % 100), str("CS" + std::to_string(i % 10)), str("Fall2024")});

  db.createTable("courses", {{"id", S}, {"title", S}});
  for (int i = 1; i <= 10; ++i) db.insert("courses", {str("CS" + std::to_string(i)), str("Course" + std::to_string(i))});

  db.createIndex("students", "id");

  if (opt.large) {
    db.createTable("huge_users", {{"id", I}, {"region", S}});
    for (int i = 1; i <= 100000; ++i) db.insert("huge_users", {num(i), str("Region" + std::to_string(i % 50))});

    db.createTable("huge_transactions", {{"tx_id", I}, {"user_id", I}, {"amount", I}});
    for (int i = 1; i <= 500000; ++i)
      db.insert("huge_transactions", {num(i), num(1 + i % 100000), num(10 + i % 900)});

    db.createIndex("huge_users", "id");
    db.createIndex("huge_transactions", "user_id");
  }

  if (opt.skewed) {
    Rng rng(opt.seed);
    static const char* kRegions[] = {"north", "south", "east", "west", "central", "coast", "valley", "metro",
                                     "harbor", "plains", "delta", "ridge", "lakes", "forest", "desert", "island",
                                     "canyon", "summit", "bay", "prairie", "tundra", "basin", "mesa", "grove", "fjord"};
    Zipf regionZipf(25, 1.0);
    db.createTable("customers", {{"c_id", I}, {"region", S}, {"tier", I}, {"signup_day", I}});
    for (int i = 1; i <= 10000; ++i) {
      double u = rng.unit();
      int tier = u < 0.60 ? 1 : u < 0.85 ? 2 : u < 0.95 ? 3 : 4;
      db.insert("customers", {num(i), str(kRegions[regionZipf.sample(rng)]), num(tier), num(rng.range(1, 1000))});
    }

    Zipf customerZipf(10000, 1.1);
    static const char* kStatus[] = {"DELIVERED", "SHIPPED", "PROCESSING", "CANCELLED", "RETURNED"};
    db.createTable("orders", {{"o_id", I}, {"customer_id", I}, {"amount", D}, {"status", S}, {"day", I}});
    for (int i = 1; i <= 200000; ++i) {
      double amount = std::round(std::exp(3.5 + rng.normal()) * 100.0) / 100.0;  // log-normal
      double u = rng.unit();
      int status = u < 0.70 ? 0 : u < 0.85 ? 1 : u < 0.93 ? 2 : u < 0.98 ? 3 : 4;
      double w = rng.unit();
      int day = 365 - static_cast<int>(365.0 * w * w);  // more orders late in the year
      if (day < 1) day = 1;
      db.insert("orders", {num(i), num(static_cast<int64_t>(customerZipf.sample(rng)) + 1), Value::Double(amount),
                           str(kStatus[status]), num(day)});
    }
    db.createIndex("customers", "c_id");
    db.createIndex("orders", "customer_id");
  }

  db.analyzeAll();
  db.pool().flushAll();
  db.disk().resetStats();
  db.pool().resetStats();
}

}  // namespace sysr
