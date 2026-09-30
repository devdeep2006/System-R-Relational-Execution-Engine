#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include "SQLParser.h"
#include "SQLToRATreeConverter.h"
#include "optimizer/InMemoryDatabase.h"
#include "optimizer/Catalog.h"
#include "optimizer/SizeEstimator.h"
#include "optimizer/CostModel.h"
#include "optimizer/Optimizer.h"
#include "optimizer/JoinGraph.h"
#include "optimizer/PlanNode.h"
#include "optimizer/ExecutionEngine.h"

// Helper function to collect table names from a plan subtree
void collectTableNames(std::shared_ptr<PlanNode> node, std::vector<std::string>& tables) {
    if (!node) return;
    if (node->type == SCAN) {
        tables.push_back(node->tableName);
    } else if (node->type == JOIN) {
        collectTableNames(node->left, tables);
        collectTableNames(node->right, tables);
    } else if (node->type == FILTER || node->type == PROJECT) {
        collectTableNames(node->left, tables);
    }
}

// Helper function to print the join sequence
void printJoinSequence(std::shared_ptr<PlanNode> node, int& joinCounter, std::ostream& out) {
    if (!node) return;

    if (node->type == JOIN) {
        printJoinSequence(node->left, joinCounter, out);
        printJoinSequence(node->right, joinCounter, out);

        out << "  Join " << ++joinCounter << ": ";

        std::vector<std::string> leftTables, rightTables;
        collectTableNames(node->left, leftTables);
        collectTableNames(node->right, rightTables);

        out << "[";
        for (size_t i = 0; i < leftTables.size(); i++) {
            if (i > 0) out << ",";
            out << leftTables[i];
        }
        out << "] X [";
        for (size_t i = 0; i < rightTables.size(); i++) {
            if (i > 0) out << ",";
            out << rightTables[i];
        }
        out << "] ON " << node->condition;

        std::string algStr = (node->algorithm == HASH) ? "HashJoin" :
                             (node->algorithm == BNLJ) ? "BNLJ" : "MergeJoin";
        out << " (" << algStr << ")" << std::endl;
    }
}

void runTestQuery(const std::string& query, const Optimizer& opt, const Catalog& cat, std::ostream& out) {
    out << "\n==================================================================\n";
    out << "TEST QUERY: " << query << "\n";
    out << "------------------------------------------------------------------\n";

    // 1. Parse SQL to RA Tree (to get basic structure)
    RANode* raTree = parseSQLToRA(query);
    if (!raTree) {
        out << "Parse failed!\n";
        return;
    }

    // 2. Build JoinGraph from query
    // In a full system, the SQLToRATreeConverter would do this.
    // For testing, we manually simulate the extraction of tables and conditions.
    JoinGraph graph;

    // Simple logic to extract tables for our test cases
    if (query.find("students") != std::string::npos) graph.base_tables.push_back("students");
    if (query.find("grades") != std::string::npos) graph.base_tables.push_back("grades");
    if (query.find("courses") != std::string::npos) graph.base_tables.push_back("courses");
    if (query.find("enrollments") != std::string::npos) graph.base_tables.push_back("enrollments");
    if (query.find("huge_users") != std::string::npos) graph.base_tables.push_back("huge_users");
    if (query.find("huge_transactions") != std::string::npos) graph.base_tables.push_back("huge_transactions");

    // Mocking join conditions based on common test patterns
    if (query.find("students.id = grades.student_id") != std::string::npos) {
        graph.join_conditions.emplace_back("students", "id", "grades", "student_id");
    }
    if (query.find("enrollments.course_id = courses.id") != std::string::npos) {
        graph.join_conditions.emplace_back("enrollments", "course_id", "courses", "id");
    }
    if (query.find("students.id = enrollments.student_id") != std::string::npos) {
        graph.join_conditions.emplace_back("students", "id", "enrollments", "student_id");
    }
    if (query.find("huge_users.id = huge_transactions.user_id") != std::string::npos) {
        graph.join_conditions.emplace_back("huge_users", "id", "huge_transactions", "user_id");
    }

    // Mocking Selection conditions (WHERE clause)
    if (query.find("WHERE") != std::string::npos) {
        size_t pos = query.find("WHERE");
        graph.selection_conditions.push_back(query.substr(pos + 6));
    }

    out << "Optimizer is processing heuristics and DP...\n";
    DPState result = opt.optimize(graph);

    if (result.plan) {
        out << "\nOPTIMAL PHYSICAL PLAN (Tree):\n";
        // result.plan->print() is hardcoded to std::cout in PlanNode.h, keeping it as is or we can skip it.
        // For simplicity, we just output the join sequence to `out`
        
        out << "\nJOIN SEQUENCE (Order of Operations):\n";
        int joinCounter = 0;
        printJoinSequence(result.plan, joinCounter, out);

        out << "\nEstimated Cost: " << result.cost << "\n";
        out << "Resulting Tuple Size: " << result.size << "\n";
        out << "Best Algorithm Selected: " << result.bestAlg << "\n";

        // --- Execute Query with Execution Engine ---
        out << "\n--- EXECUTION ENGINE RESULTS ---\n";
        InMemoryDatabase execDb;
        executeAndPrintQuery(*result.plan, execDb, out);
        out << "-------------------------------\n";
    } else {
        out << "Optimizer could not find a valid plan.\n";
    }

    // Cleanup RA tree
    // Note: In a real system, use a proper smart pointer or recursive delete
}

int main() {
    // Setup Database and Catalog
    InMemoryDatabase db;
    Catalog catalog(&db);
    SizeEstimator sizeEst(&catalog);
    CostModel costModel;
    Optimizer optimizer(&catalog, &sizeEst, &costModel);

    std::cout << "--- DBMS Heuristic Optimizer Test Suite ---\n";
    catalog.printStats();

    std::ifstream testFile("test.txt");
    if (!testFile.is_open()) {
        std::cerr << "Could not open test.txt\n";
        return 1;
    }

    std::string line;
    while (std::getline(testFile, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == '\r') {
            continue;
        }
        
        // Remove trailing carriage return if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t pipePos = line.find('|');
        if (pipePos != std::string::npos) {
            std::string testId = line.substr(0, pipePos);
            std::string query = line.substr(pipePos + 1);

            if (testId == "TC11_Sorted") {
                catalog.setSortedColumn("huge_users", "id");
                catalog.setSortedColumn("huge_transactions", "user_id");
            } else {
                catalog.removeSortedColumn("huge_users");
                catalog.removeSortedColumn("huge_transactions");
            }

            std::ofstream outFile(testId + ".out");
            if (outFile.is_open()) {
                runTestQuery(query, optimizer, catalog, outFile);
            } else {
                runTestQuery(query, optimizer, catalog, std::cout);
            }
        } else {
            runTestQuery(line, optimizer, catalog, std::cout);
        }
    }

    return 0;
}
