#ifndef IN_MEMORY_DATABASE_H
#define IN_MEMORY_DATABASE_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <iostream>

// Row = single record (map of column -> value)
using Row = std::map<std::string, std::string>;

// Table = vector of rows
using Table = std::vector<Row>;

// Database = map of table_name -> table
using Database = std::map<std::string, Table>;

/**
 * Simple In-Memory Database with mock data for testing
 * Students, Grades, Courses, Enrollments tables
 */
class InMemoryDatabase {
private:
    Database db;

    // Generate statistics (nr, v) from actual table data
    void generateStats(const std::string& tableName, int64_t& nr, std::map<std::string, int64_t>& v) const {
        auto it = db.find(tableName);
        if (it == db.end()) {
            nr = 0;
            v.clear();
            return;
        }

        nr = it->second.size();

        // Count distinct values for each column
        v.clear();
        if (nr > 0) {
            const Table& table = it->second;
            const Row& firstRow = table[0];

            for (const auto& col : firstRow) {
                std::set<std::string> distinctValues;
                for (const auto& row : table) {
                    distinctValues.insert(row.at(col.first));
                }
                v[col.first] = distinctValues.size();
            }
        }
    }

public:
    InMemoryDatabase() {
        // Initialize mock data
        initializeTables();
    }

    void initializeTables() {
        // students(id, name, age)
        Table students;
        for (int i = 1; i <= 100; i++) {
            Row r;
            r["id"] = std::to_string(i);
            r["name"] = "Student" + std::to_string(i);
            r["age"] = std::to_string(18 + (i % 5));
            students.push_back(r);
        }
        db["students"] = students;

        // grades(student_id, course, grade)
        Table grades;
        for (int i = 1; i <= 500; i++) {
            Row r;
            r["student_id"] = std::to_string(1 + (i % 100));
            r["course"] = "CS" + std::to_string(i % 10);
            r["grade"] = std::to_string(60 + (i % 41));
            grades.push_back(r);
        }
        db["grades"] = grades;

        // enrollments(student_id, course_id, semester)
        Table enrollments;
        for (int i = 1; i <= 100; i++) {
            Row r;
            r["student_id"] = std::to_string(1 + (i % 100));
            r["course_id"] = "CS" + std::to_string(i % 10);
            r["semester"] = "Fall2024";
            enrollments.push_back(r);
        }
        db["enrollments"] = enrollments;

        // courses(id, title)
        Table courses;
        for (int i = 1; i <= 10; i++) {
            Row r;
            r["id"] = "CS" + std::to_string(i);
            r["title"] = "Course" + std::to_string(i);
            courses.push_back(r);
        }
        db["courses"] = courses;
        // ==========================================
        // MASSIVE TABLES FOR OPTIMIZER TESTING
        // ==========================================
        
        // huge_users (100,000 rows -> 1,000 blocks)
        Table huge_users;
        for (int i = 1; i <= 100000; i++) {
            Row r;
            r["id"] = std::to_string(i);
            r["region"] = "Region" + std::to_string(i % 50);
            huge_users.push_back(r);
        }
        db["huge_users"] = huge_users;

        // huge_transactions (500,000 rows -> 5,000 blocks)
        Table huge_tx;
        for (int i = 1; i <= 500000; i++) {
            Row r;
            r["tx_id"] = std::to_string(i);
            r["user_id"] = std::to_string(1 + (i % 100000));
            r["amount"] = std::to_string(10 + (i % 900));
            huge_tx.push_back(r);
        }
        db["huge_transactions"] = huge_tx;
    }

    // Get table by name
    const Table* getTable(const std::string& tableName) const {
        auto it = db.find(tableName);
        if (it != db.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Get row count for a table
    int64_t getRowCount(const std::string& tableName) const {
        auto it = db.find(tableName);
        if (it != db.end()) {
            return it->second.size();
        }
        return 0;
    }

    // Get distinct value count for a column
    int64_t getDistinctCount(const std::string& tableName, const std::string& colName) const {
        auto it = db.find(tableName);
        if (it == db.end()) return 0;

        const Table& table = it->second;
        if (table.empty()) return 0;

        std::set<std::string> distinct;
        for (const auto& row : table) {
            auto colIt = row.find(colName);
            if (colIt != row.end()) {
                distinct.insert(colIt->second);
            }
        }
        return distinct.size();
    }

    // Print a table (for debugging)
    void printTable(const std::string& tableName) const {
        const Table* t = getTable(tableName);
        if (!t) {
            std::cout << "Table " << tableName << " not found" << std::endl;
            return;
        }

        if (t->empty()) {
            std::cout << "Table " << tableName << " is empty" << std::endl;
            return;
        }

        // Print header
        const Row& firstRow = (*t)[0];
        for (const auto& col : firstRow) {
            std::cout << col.first << "\t";
        }
        std::cout << std::endl;

        // Print rows (max 10)
        int count = 0;
        for (const auto& row : *t) {
            if (count >= 10) break;
            for (const auto& col : row) {
                std::cout << col.second << "\t";
            }
            std::cout << std::endl;
            count++;
        }
    }

    // Print all tables
    void printAllTables() const {
        for (const auto& p : db) {
            std::cout << "\n=== Table: " << p.first << " (" << p.second.size() << " rows) ===" << std::endl;
            printTable(p.first);
        }
    }
};

#endif
