#ifndef EXECUTION_ENGINE_H
#define EXECUTION_ENGINE_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
#include <iostream>
#include "InMemoryDatabase.h"
#include "PlanNode.h"

// Row and Table types from InMemoryDatabase.h
using Row = std::map<std::string, std::string>;
using Table = std::vector<Row>;

// =============================================================================
// Base Operator Class (Volcano Iterator Model)
// =============================================================================

class Operator {
public:
    virtual ~Operator() = default;

    // Open the operator - initialize state
    virtual void open() = 0;

    // Next - returns a row, or nullptr when done
    virtual Row* next() = 0;

    // Close the operator - clean up state
    virtual void close() = 0;
};

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * Parse condition string 'table1.col1 = table2.col2' into components
 * @param condition Condition string
 * @param leftTable Output: left table name
 * @param leftCol Output: left column name
 * @param rightTable Output: right table name
 * @param rightCol Output: right column name
 * @return true if parsing succeeded
 */
inline bool parseCondition(
    const std::string& condition,
    std::string& leftTable, std::string& leftCol,
    std::string& rightTable, std::string& rightCol) {

    leftTable = ""; leftCol = "";
    rightTable = ""; rightCol = "";

    size_t eqPos = condition.find(" = ");
    if (eqPos == std::string::npos) {
        return false;
    }

    std::string left = condition.substr(0, eqPos);
    std::string right = condition.substr(eqPos + 3);

    // Trim whitespace
    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" ");
        size_t end = s.find_last_not_of(" ");
        if (start != std::string::npos) {
            s = s.substr(start, end - start + 1);
        }
    };
    trim(left);
    trim(right);

    // Parse table.column format
    auto parseTableCol = [](const std::string& s, std::string& table, std::string& col) {
        size_t dotPos = s.find('.');
        if (dotPos != std::string::npos) {
            table = s.substr(0, dotPos);
            col = s; // PRESERVE FULL PREFIX s
        } else {
            table = "";
            col = s;
        }
    };

    parseTableCol(left, leftTable, leftCol);
    parseTableCol(right, rightTable, rightCol);

    return !leftCol.empty() && !rightCol.empty();
}

/**
 * Merge two rows - uses column names as-is (no prefix)
 * If both left and right have the same column name, right wins
 * @param left Left row
 * @param right Right row
 * @return Merged row
 */
inline Row mergeRows(const Row& left, const Row& right) {
    Row result = left;  // Start with left
    for (const auto& kv : right) {
        result[kv.first] = kv.second;  // Overwrite with right values
    }
    return result;
}

/**
 * Merge two rows without prefixing (for same-table joins or projections)
 * @param left Left row
 * @param right Right row (may be empty)
 * @param columns List of columns to include (for projection)
 * @return Merged row
 */
inline Row mergeRows(const Row& left, const Row& right, const std::vector<std::string>& columns) {
    Row result;
    for (const auto& col : columns) {
        auto it = left.find(col);
        if (it != left.end()) {
            result[col] = it->second;
        } else {
            it = right.find(col);
            if (it != right.end()) {
                result[col] = it->second;
            }
        }
    }
    return result;
}

/**
 * Extract value from row by column name (with or without table prefix)
 * @param row Row to extract from
 * @param colName Column name (with or without table.prefix)
 * @return Value string, or empty if not found
 */
inline std::string getColValue(const Row& row, const std::string& colName) {
    // Try with table prefix first
    auto it = row.find(colName);
    if (it != row.end()) {
        return it->second;
    }

    // Try without table prefix if string has one
    size_t dotPos = colName.find('.');
    if (dotPos != std::string::npos) {
        std::string colOnly = colName.substr(dotPos + 1);
        it = row.find(colOnly);
        if (it != row.end()) {
            return it->second;
        }
    } else {
        // Try suffix match if string does not have prefix (e.g. searching for "id" should match "students.id")
        std::string suffix = "." + colName;
        for (const auto& kv : row) {
            if (kv.first.length() >= suffix.length()) {
                if (kv.first.compare(kv.first.length() - suffix.length(), suffix.length(), suffix) == 0) {
                    return kv.second;
                }
            }
        }
    }

    return "";
}

/**
 * Check if a row matches a simple condition 'table.col = value'
 * Handles both table.col and plain col formats
 * @param row Row to check
 * @param condition Condition string like 'students.id = 123'
 * @return true if condition matches
 */
inline bool matchesCondition(const Row& row, const std::string& condition) {
    std::string cond = condition;
    while (!cond.empty() && cond.front() == '(' && cond.back() == ')') {
        cond = cond.substr(1, cond.length() - 2);
    }

    std::string op = "";
    size_t opPos = std::string::npos;
    std::vector<std::string> ops = {"!=", ">=", "<=", "=", ">", "<"};
    for (const auto& o : ops) {
        opPos = cond.find(o);
        if (opPos != std::string::npos) {
            op = o;
            break;
        }
    }

    if (opPos == std::string::npos) return true;

    std::string leftSide = cond.substr(0, opPos);
    std::string rightSide = cond.substr(opPos + op.length());

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" ");
        size_t end = s.find_last_not_of(" ");
        if (start != std::string::npos) s = s.substr(start, end - start + 1);
        else s = "";
    };
    trim(leftSide);
    trim(rightSide);

    if (!rightSide.empty() && rightSide.front() == '\'' && rightSide.back() == '\'') {
        rightSide = rightSide.substr(1, rightSide.length() - 2);
    }

    std::string val = getColValue(row, leftSide);
    if (!val.empty() || row.find(leftSide) != row.end() || leftSide.empty() == false) { // Assuming val can be empty but valid
        // Actually, let's just make getColValue return a bool indicator too or assume if it's not empty it resolved, but let's check properly:
        bool found = false;
        if (row.find(leftSide) != row.end()) { val = row.at(leftSide); found = true; }
        else {
            std::string suf = "." + leftSide;
            for (const auto& kv : row) {
                if (kv.first == leftSide || (kv.first.length() >= suf.length() && kv.first.compare(kv.first.length() - suf.length(), suf.length(), suf) == 0)) {
                    val = kv.second;
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            bool res = false;
            if (op == "=") res = (val == rightSide);
            else if (op == "!=") res = (val != rightSide);
            else {
                try {
                    int leftInt = std::stoi(val);
                    int rightInt = std::stoi(rightSide);
                    if (op == ">") res = leftInt > rightInt;
                    else if (op == "<") res = leftInt < rightInt;
                    else if (op == ">=") res = leftInt >= rightInt;
                    else if (op == "<=") res = leftInt <= rightInt;
                } catch (...) {
                    if (op == ">") res = val > rightSide;
                    else if (op == "<") res = val < rightSide;
                    else if (op == ">=") res = val >= rightSide;
                    else if (op == "<=") res = val <= rightSide;
                }
            }
            return res;
        }
    }

    return false;
}

// =============================================================================
// ScanOperator - Scans a table
// =============================================================================

class ScanOperator : public Operator {
private:
    const Table& table;
    std::string tableName;
    int currentIndex;

public:
    ScanOperator(const Table& t, const std::string& name) : table(t), tableName(name), currentIndex(0) {}

    void open() override {
        currentIndex = 0;
    }

    Row* next() override {
        if (currentIndex >= (int)table.size()) {
            return nullptr;
        }
        Row* row = new Row();
        for (const auto& kv : table[currentIndex]) {
            (*row)[tableName + "." + kv.first] = kv.second;
        }
        currentIndex++;
        return row;
    }

    void close() override {
        currentIndex = 0;
    }
};

// =============================================================================
// FilterOperator - Filters rows based on condition
// =============================================================================

class FilterOperator : public Operator {
private:
    std::unique_ptr<Operator> child;
    std::string condition;

public:
    FilterOperator(std::unique_ptr<Operator> c, const std::string& cond)
        : child(std::move(c)), condition(cond) {}

    void open() override {
        child->open();
    }

    Row* next() override {
        Row* row;
        while ((row = child->next()) != nullptr) {
            if (matchesCondition(*row, condition)) {
                return row;
            }
            delete row; // Release memory if not matched
        }
        return nullptr;
    }

    void close() override {
        child->close();
    }
};

// =============================================================================
// HashJoinOperator - Hash-based join
// =============================================================================

class HashJoinOperator : public Operator {
private:
    std::unique_ptr<Operator> leftOp;
    std::unique_ptr<Operator> rightOp;
    std::string leftCol;
    std::string rightCol;
    bool needsRightPrefix;

    // Hash table: map<rightColValue, vector<Rows>>
    std::map<std::string, std::vector<Row>> hashTable;
    std::vector<Row> currentMatches;
    int matchIndex;
    Row* currentLeftRow;

    // Track if we've started
    bool openCalled;

public:
    HashJoinOperator(
        std::unique_ptr<Operator> left,
        std::unique_ptr<Operator> right,
        const std::string& condition)
        : leftOp(std::move(left)),
          rightOp(std::move(right)),
          currentLeftRow(nullptr),
          matchIndex(0),
          openCalled(false) {

        std::string t1, t2;
        parseCondition(condition, t1, leftCol, t2, rightCol);
    }

    void open() override {
        openCalled = true;
        matchIndex = 0;

        // Build hash table from right side
        hashTable.clear();
        rightOp->open();
        Row* r;
        while ((r = rightOp->next()) != nullptr) {
            std::string key = getColValue(*r, rightCol);
            hashTable[key].push_back(*r);
            delete r;
        }
        rightOp->close();

        // Open left side
        leftOp->open();
    }

    Row* next() override {
        if (!openCalled) {
            open();
        }

        // If we have remaining matches from current left row
        if (currentLeftRow != nullptr && matchIndex < (int)currentMatches.size()) {
            Row result = mergeRows(*currentLeftRow, currentMatches[matchIndex]);
            matchIndex++;
            return new Row(result);
        }

        // Get next left row
        Row* leftRow = leftOp->next();
        if (leftRow == nullptr) {
            // Done with all left rows
            close();
            return nullptr;
        }

        // Find matching right rows
        std::string leftKey = getColValue(*leftRow, leftCol);
        currentMatches = hashTable[leftKey];
        matchIndex = 0;

        if (currentMatches.empty()) {
            // No matches, get next left row
            delete leftRow;
            return next();
        }

        // Return first match
        currentLeftRow = leftRow;
        Row result = mergeRows(*currentLeftRow, currentMatches[matchIndex]);
        matchIndex++;
        return new Row(result);
    }

    void close() override {
        if (currentLeftRow) {
            delete currentLeftRow;
            currentLeftRow = nullptr;
        }
        if (openCalled) {
            leftOp->close();
            openCalled = false;
        }
    }
};

// =============================================================================
// MergeJoinOperator - Merge-based join (requires sorted inputs)
// =============================================================================

class MergeJoinOperator : public Operator {
private:
    std::unique_ptr<Operator> leftOp;
    std::unique_ptr<Operator> rightOp;
    std::string leftCol;
    std::string rightCol;

    Row* currentLeft;
    Row* currentRight;
    bool leftDone;
    bool rightDone;
    bool openCalled;

public:
    MergeJoinOperator(
        std::unique_ptr<Operator> left,
        std::unique_ptr<Operator> right,
        const std::string& condition)
        : leftOp(std::move(left)),
          rightOp(std::move(right)),
          currentLeft(nullptr),
          currentRight(nullptr),
          leftDone(false),
          rightDone(false),
          openCalled(false) {

        std::string t1, t2;
        parseCondition(condition, t1, leftCol, t2, rightCol);
    }

    void open() override {
        openCalled = true;
        leftDone = false;
        rightDone = false;

        leftOp->open();
        rightOp->open();

        // Get first rows
        currentLeft = leftOp->next();
        currentRight = rightOp->next();

        if (currentLeft == nullptr) leftDone = true;
        if (currentRight == nullptr) rightDone = true;
    }

    Row* next() override {
        if (!openCalled) {
            open();
        }

        while (!leftDone && !rightDone) {
            std::string leftVal = getColValue(*currentLeft, leftCol);
            std::string rightVal = getColValue(*currentRight, rightCol);

            int cmp = leftVal.compare(rightVal);

            if (cmp == 0) {
                // Equal - return merged row
                Row result = mergeRows(*currentLeft, *currentRight);

                // Advance right to find more matches
                Row* tempRight = rightOp->next();
                if (tempRight == nullptr) {
                    rightDone = true;
                } else {
                    delete currentRight;
                    currentRight = tempRight;
                }

                // Also advance left since we've consumed this left row
                Row* tempLeft = leftOp->next();
                if (tempLeft == nullptr) {
                    leftDone = true;
                } else {
                    delete currentLeft;
                    currentLeft = tempLeft;
                }

                return new Row(result);
            } else if (cmp < 0) {
                // left < right - advance left
                Row* tempLeft = leftOp->next();
                if (tempLeft == nullptr) {
                    leftDone = true;
                } else {
                    delete currentLeft;
                    currentLeft = tempLeft;
                }
            } else {
                // left > right - advance right
                Row* tempRight = rightOp->next();
                if (tempRight == nullptr) {
                    rightDone = true;
                } else {
                    delete currentRight;
                    currentRight = tempRight;
                }
            }
        }

        // Done
        close();
        return nullptr;
    }

    void close() override {
        if (currentLeft) {
            delete currentLeft;
            currentLeft = nullptr;
        }
        if (currentRight) {
            delete currentRight;
            currentRight = nullptr;
        }
        if (openCalled) {
            leftOp->close();
            rightOp->close();
            openCalled = false;
        }
    }
};

// =============================================================================
// BNLJOperator - Block Nested Loop Join
// =============================================================================

class BNLJOperator : public Operator {
private:
    std::unique_ptr<Operator> leftOp;
    std::unique_ptr<Operator> rightOp;
    std::string leftCol;
    std::string rightCol;

    std::vector<Row> leftRows;
    std::vector<Row>::iterator leftIter;
    Row* currentRight;
    bool rightDone;
    bool leftExhausted;
    bool openCalled;

public:
    BNLJOperator(
        std::unique_ptr<Operator> left,
        std::unique_ptr<Operator> right,
        const std::string& condition)
        : leftOp(std::move(left)),
          rightOp(std::move(right)),
          currentRight(nullptr),
          rightDone(false),
          leftExhausted(false),
          openCalled(false) {

        std::string t1, t2;
        parseCondition(condition, t1, leftCol, t2, rightCol);
    }

    void open() override {
        openCalled = true;

        // Materialize left side
        leftOp->open();
        Row* row;
        while ((row = leftOp->next()) != nullptr) {
            leftRows.push_back(*row);
            delete row;
        }
        leftOp->close();

        leftIter = leftRows.begin();

        // Open right side
        rightOp->open();
        currentRight = rightOp->next();
        if (currentRight == nullptr) {
            rightDone = true;
        }
    }

    Row* next() override {
        if (!openCalled) {
            open();
        }

        while (!leftExhausted && !rightDone) {
            // Check if current left and right match
            std::string leftVal = getColValue(*leftIter, leftCol);
            std::string rightVal = getColValue(*currentRight, rightCol);

            if (leftVal == rightVal) {
                // Match found
                Row result = mergeRows(*leftIter, *currentRight);

                // Advance right
                Row* temp = rightOp->next();
                if (temp == nullptr) {
                    rightDone = true;
                } else {
                    delete currentRight;
                    currentRight = temp;
                }

                return new Row(result);
            } else {
                // No match - advance right
                Row* temp = rightOp->next();
                if (temp == nullptr) {
                    rightDone = true;
                } else {
                    delete currentRight;
                    currentRight = temp;
                }
            }
        }

        // Right side exhausted, advance left and reset right
        if (!leftExhausted) {
            leftIter++;
            if (leftIter == leftRows.end()) {
                leftExhausted = true;
            } else {
                // Reset right side
                rightDone = false;
                currentRight = rightOp->next();
                if (currentRight == nullptr) {
                    rightDone = true;
                }
                return next();
            }
        }

        // Done
        close();
        return nullptr;
    }

    void close() override {
        if (currentRight) {
            delete currentRight;
            currentRight = nullptr;
        }
        if (openCalled) {
            rightOp->close();
            openCalled = false;
        }
    }
};

// =============================================================================
// ProjectOperator - Projects specific columns
// =============================================================================

class ProjectOperator : public Operator {
private:
    std::unique_ptr<Operator> child;
    std::vector<std::string> columns;

public:
    ProjectOperator(std::unique_ptr<Operator> c, const std::vector<std::string>& cols)
        : child(std::move(c)), columns(cols) {}

    void open() override {
        child->open();
    }

    Row* next() override {
        Row* row = child->next();
        if (row == nullptr) {
            return nullptr;
        }

        // Create new row with only specified columns
        Row result;
        for (const auto& col : columns) {
            auto it = row->find(col);
            if (it != row->end()) {
                result[col] = it->second;
            } else {
                // Try with table prefix
                std::string colName = col;
                size_t dotPos = col.find('.');
                if (dotPos != std::string::npos) {
                    std::string colOnly = col.substr(dotPos + 1);
                    it = row->find(colOnly);
                    if (it != row->end()) {
                        result[col] = it->second;
                    }
                }
            }
        }

        delete row;
        return new Row(result);
    }

    void close() override {
        child->close();
    }
};

// =============================================================================
// OperatorBuilder - Builds operator tree from PlanNode
// =============================================================================

/**
 * Recursively build an operator tree from a PlanNode tree
 */
inline std::unique_ptr<Operator> buildOperatorTree(
    const PlanNode* node,
    InMemoryDatabase& db,
    const std::vector<std::string>& tableAliases = {}) {

    if (node == nullptr) {
        return nullptr;
    }

    switch (node->type) {
        case SCAN: {
            const Table* table = db.getTable(node->tableName);
            if (table == nullptr) {
                std::cerr << "Error: Table '" << node->tableName << "' not found\n";
                return nullptr;
            }
            return std::make_unique<ScanOperator>(*table, node->tableName);
        }

        case FILTER: {
            auto child = buildOperatorTree(node->left.get(), db, tableAliases);
            if (!child) return nullptr;
            return std::make_unique<FilterOperator>(std::move(child), node->condition);
        }

        case PROJECT: {
            auto child = buildOperatorTree(node->left.get(), db, tableAliases);
            if (!child) return nullptr;
            return std::make_unique<ProjectOperator>(std::move(child), node->projectedColumns);
        }

        case JOIN: {
            auto leftOp = buildOperatorTree(node->left.get(), db, tableAliases);
            auto rightOp = buildOperatorTree(node->right.get(), db, tableAliases);

            if (!leftOp || !rightOp) {
                return nullptr;
            }

            // Determine join algorithm
            switch (node->algorithm) {
                case HASH:
                    return std::make_unique<HashJoinOperator>(
                        std::move(leftOp), std::move(rightOp), node->condition);

                case MERGE:
                    return std::make_unique<MergeJoinOperator>(
                        std::move(leftOp), std::move(rightOp), node->condition);

                case BNLJ:
                    return std::make_unique<BNLJOperator>(
                        std::move(leftOp), std::move(rightOp), node->condition);

                default:
                    // Default to hash join
                    return std::make_unique<HashJoinOperator>(
                        std::move(leftOp), std::move(rightOp), node->condition);
            }
        }

        default:
            std::cerr << "Error: Unknown operator type\n";
            return nullptr;
    }
}

/**
 * Build operator tree from PlanNode with table aliases for join condition resolution
 */
inline std::unique_ptr<Operator> buildOperatorTree(
    const PlanNode& node,
    InMemoryDatabase& db) {
    return buildOperatorTree(&node, db);
}

// =============================================================================
// Query Execution
// =============================================================================

/**
 * Execute a query and collect all results
 */
inline std::vector<Row> executeQuery(const PlanNode& node, InMemoryDatabase& db) {
    auto root = buildOperatorTree(node, db);
    if (!root) {
        return {};
    }

    std::vector<Row> results;
    root->open();

    Row* row;
    while ((row = root->next()) != nullptr) {
        results.push_back(*row);
        delete row;
    }

    root->close();
    return results;
}

/**
 * Execute a query and print results (for debugging)
 */
inline void executeAndPrintQuery(const PlanNode& node, InMemoryDatabase& db, std::ostream& out = std::cout) {
    std::vector<Row> results = executeQuery(node, db);

    if (results.empty()) {
        out << "No results.\n";
        return;
    }

    // Get all column names
    std::vector<std::string> columns;
    for (const auto& row : results) {
        for (const auto& kv : row) {
            if (std::find(columns.begin(), columns.end(), kv.first) == columns.end()) {
                columns.push_back(kv.first);
            }
        }
    }

    // Print header
    for (const auto& col : columns) {
        out << col << "\t";
    }
    out << "\n";

    // Print rows
    for (const auto& row : results) {
        for (const auto& col : columns) {
            auto it = row.find(col);
            if (it != row.end()) {
                out << it->second << "\t";
            } else {
                out << "NULL\t";
            }
        }
        out << "\n";
    }

    out << results.size() << " row(s) returned.\n";
}

#endif // EXECUTION_ENGINE_H