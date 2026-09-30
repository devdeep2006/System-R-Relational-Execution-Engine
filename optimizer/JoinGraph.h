#ifndef JOIN_GRAPH_H
#define JOIN_GRAPH_H

#include <string>
#include <vector>
#include <iostream>
#include <map>

/**
 * JoinPredicate - A join condition between two tables
 * e.g., students.id = grades.student_id
 */
struct JoinPredicate {
    std::string table1;
    std::string col1;
    std::string table2;
    std::string col2;

    JoinPredicate(const std::string& t1, const std::string& c1,
                  const std::string& t2, const std::string& c2)
        : table1(t1), col1(c1), table2(t2), col2(c2) {}
};

/**
 * JoinGraph - Flattened representation of a query's join structure
 * - base_tables: list of table names
 * - join_conditions: list of join predicates
 */
struct JoinGraph {
    std::vector<std::string> base_tables;
    std::vector<JoinPredicate> join_conditions;
    std::vector<std::string> selection_conditions;
    std::map<std::string, std::vector<std::string>> required_columns;
};

/**
 * Parse join condition string like "students.id = grades.student_id"
 * into table1.col1 = table2.col2
 */
inline void parseJoinCondition(const std::string& cond,
                               std::string& t1, std::string& c1,
                               std::string& t2, std::string& c2) {
    // Reset outputs
    t1 = ""; c1 = ""; t2 = ""; c2 = "";

    // Find the " = " (with spaces) - this is the most reliable separator
    size_t eqPos = cond.find(" = ");

    if (eqPos != std::string::npos) {
        std::string left = cond.substr(0, eqPos);
        std::string right = cond.substr(eqPos + 3);  // Skip " = "

        // Trim leading/trailing whitespace from left and right
        size_t start1 = left.find_first_not_of(" ");
        size_t end1 = left.find_last_not_of(" ");
        if (start1 != std::string::npos) {
            left = left.substr(start1, end1 - start1 + 1);
        }

        size_t start2 = right.find_first_not_of(" ");
        size_t end2 = right.find_last_not_of(" ");
        if (start2 != std::string::npos) {
            right = right.substr(start2, end2 - start2 + 1);
        }

        // Parse left: table.column
        size_t dotPos1 = left.find('.');
        if (dotPos1 != std::string::npos) {
            t1 = left.substr(0, dotPos1);
            c1 = left.substr(dotPos1 + 1);
        } else {
            c1 = left;
        }

        // Parse right: table.column
        size_t dotPos2 = right.find('.');
        if (dotPos2 != std::string::npos) {
            t2 = right.substr(0, dotPos2);
            c2 = right.substr(dotPos2 + 1);
        } else {
            c2 = right;
        }
    }
}

/**
 * Extract join graph from a join condition string and list of tables
 * This is a simple version that works with the RA tree from main.cpp
 */
inline JoinGraph buildJoinGraph(const std::vector<std::string>& tables,
                                const std::vector<std::string>& joinConditions) {
    JoinGraph graph;
    graph.base_tables = tables;

    for (const auto& cond : joinConditions) {
        std::string t1, c1, t2, c2;
        parseJoinCondition(cond, t1, c1, t2, c2);

        // Handle the case where joinCondition has "INNER " prefix etc.
        // Strip any prefix like "INNER ", "LEFT ", "FULL "
        if (t1.empty()) {
            // Try removing prefix
            size_t spacePos = cond.find(' ');
            if (spacePos != std::string::npos) {
                std::string stripped = cond.substr(spacePos + 1);
                parseJoinCondition(stripped, t1, c1, t2, c2);
            }
        }

        if (!t1.empty() && !t2.empty() && !c1.empty() && !c2.empty()) {
            graph.join_conditions.emplace_back(t1, c1, t2, c2);
        }
    }

    return graph;
}

/**
 * Print join graph for debugging
 */
inline void printJoinGraph(const JoinGraph& graph) {
    std::cout << "=== Join Graph ===" << std::endl;
    std::cout << "Base tables: ";
    for (const auto& t : graph.base_tables) std::cout << t << " ";
    std::cout << std::endl;

    std::cout << "Join conditions:" << std::endl;
    for (const auto& pred : graph.join_conditions) {
        std::cout << "  " << pred.table1 << "." << pred.col1
                  << " = " << pred.table2 << "." << pred.col2 << std::endl;
    }
    std::cout << "==================" << std::endl;
}

#endif
