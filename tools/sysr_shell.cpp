// Interactive SQL shell and script runner.
//
//   sysr_shell                 REPL over the demo database
//   sysr_shell script.sql      run a script, echoing each statement and its result
//   --small                    skip the 600K-row huge_* tables
//   --empty                    start with no tables
//   --rows N                   rows to print per result (default 20)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "engine/database.h"
#include "engine/datagen.h"

using namespace sysr;

namespace {

size_t maxRows = 20;

void printTable(const QueryResult& r) {
  std::vector<size_t> width;
  for (const auto& c : r.columns) width.push_back(c.size());
  size_t shown = std::min(r.rows.size(), maxRows);
  std::vector<std::vector<std::string>> cells;
  for (size_t i = 0; i < shown; ++i) {
    std::vector<std::string> row;
    for (size_t c = 0; c < r.rows[i].size(); ++c) {
      std::string s = r.rows[i][c].toString();
      if (c < width.size()) width[c] = std::max(width[c], s.size());
      row.push_back(std::move(s));
    }
    cells.push_back(std::move(row));
  }
  auto line = [&]() {
    std::string s = "+";
    for (size_t w : width) s += std::string(w + 2, '-') + "+";
    std::cout << s << "\n";
  };
  auto emit = [&](const std::vector<std::string>& row) {
    std::cout << "|";
    for (size_t c = 0; c < width.size(); ++c) {
      std::string v = c < row.size() ? row[c] : "";
      std::cout << " " << v << std::string(width[c] - v.size(), ' ') << " |";
    }
    std::cout << "\n";
  };
  line();
  emit(r.columns);
  line();
  for (const auto& row : cells) emit(row);
  line();
  std::cout << r.rows.size() << " row" << (r.rows.size() == 1 ? "" : "s");
  if (shown < r.rows.size()) std::cout << " (showing " << shown << ")";
  if (r.planningMs > 0 || r.executionMs > 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "  [plan %.2f ms, exec %.2f ms, I/O %llu transfers / %llu seeks]", r.planningMs,
                  r.executionMs, static_cast<unsigned long long>(r.io.transfers()),
                  static_cast<unsigned long long>(r.io.seeks));
    std::cout << buf;
  }
  std::cout << "\n";
}

void runText(Database& db, const std::string& sql, bool echo) {
  std::vector<Statement> stmts;
  try {
    stmts = parseScript(sql);
  } catch (const DbError& e) {
    std::cout << "ERROR: " << e.what() << "\n";
    return;
  }
  for (const Statement& st : stmts) {
    if (echo) std::cout << "\nsysr> " << st.text << ";\n";
    try {
      QueryResult r = db.run(st);
      if (r.hasRows) printTable(r);
      else std::cout << r.message << (r.message.empty() || r.message.back() == '\n' ? "" : "\n");
    } catch (const DbError& e) {
      std::cout << "ERROR: " << e.what() << "\n";
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  DemoDataOptions opt;
  bool empty = false;
  std::vector<std::string> scripts;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--small") opt.large = false;
    else if (a == "--empty") empty = true;
    else if (a == "--rows" && i + 1 < argc) maxRows = static_cast<size_t>(std::stoul(argv[++i]));
    else scripts.push_back(a);
  }

  Database db;
  if (!empty) {
    auto t0 = std::chrono::steady_clock::now();
    loadDemoData(db, opt);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "Loaded demo database in %.1f s (%zu pages on disk).\n", secs, db.disk().livePages());
  }

  if (!scripts.empty()) {
    for (const auto& path : scripts) {
      std::ifstream in(path);
      if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      runText(db, ss.str(), true);
    }
    return 0;
  }

  std::cout << "System R query engine. End statements with ';'. Type \\q to quit.\n";
  std::string buffer, line;
  while (true) {
    std::cout << (buffer.empty() ? "sysr> " : "  ... ") << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (buffer.empty() && (line == "\\q" || line == "quit" || line == "exit")) break;
    buffer += line + "\n";
    if (line.find(';') == std::string::npos) continue;
    runText(db, buffer, false);
    buffer.clear();
  }
  return 0;
}
