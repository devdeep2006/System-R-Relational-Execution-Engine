#ifndef PLAN_NODE_H
#define PLAN_NODE_H

#include <string>
#include <vector>
#include <memory>
#include <iostream>

enum OperatorType {
    SCAN,
    FILTER,
    PROJECT,
    JOIN
};

enum JoinAlgorithm {
    HASH,
    BNLJ,
    MERGE,
    NONE
};

/**
 * PlanNode - Represents a node in the physical query plan tree.
 */
struct PlanNode {
    OperatorType type;
    JoinAlgorithm algorithm;
    std::string tableName;
    std::string condition; // For Filter or Join
    std::vector<std::string> projectedColumns;

    std::shared_ptr<PlanNode> left;
    std::shared_ptr<PlanNode> right;

    PlanNode(OperatorType t) : type(t), algorithm(NONE) {}

    // Helper for printing the plan tree
    void print(int indent = 0) const {
        std::string prefix(indent, ' ');
        switch (type) {
            case SCAN:
                std::cout << prefix << "-> SCAN: " << tableName << "\n";
                break;
            case FILTER:
                std::cout << prefix << "-> FILTER: " << condition << "\n";
                break;
            case PROJECT:
                std::cout << prefix << "-> PROJECT: ";
                for (const auto& col : projectedColumns) std::cout << col << " ";
                std::cout << "\n";
                break;
            case JOIN: {
                std::string algStr = (algorithm == HASH) ? "HashJoin" :
                                     (algorithm == BNLJ) ? "BNLJ" : "MergeJoin";
                std::cout << prefix << "-> JOIN (" << algStr << "): " << condition << "\n";
                // Print execution instruction for the engine
                std::string algExec = (algorithm == HASH) ? "EXECUTE_HASH_JOIN" :
                                      (algorithm == BNLJ) ? "EXECUTE_BNLJ" : "EXECUTE_MERGE_JOIN";
                std::cout << prefix << "   [EXECUTION: " << algExec << " ON " << condition << "]\n";
                break;
            }
        }
        if (left) left->print(indent + 4);
        if (right) right->print(indent + 4);
    }

    // Get the join algorithm name as string
    std::string getAlgorithmName() const {
        switch (algorithm) {
            case HASH: return "HASH";
            case BNLJ: return "BNLJ";
            case MERGE: return "MERGE";
            default: return "NONE";
        }
    }

    // Check if this is a cross product join
    bool isCrossProduct() const {
        return condition.find("CROSS") != std::string::npos || condition.empty();
    }
};

#endif
