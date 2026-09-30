#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include <string>
#include <vector>
#include <map>
#include <limits>
#include <iostream>
#include <memory>
#include "InMemoryDatabase.h"
#include "Catalog.h"
#include "SizeEstimator.h"
#include "CostModel.h"
#include "JoinGraph.h"
#include "PlanNode.h"

/**
 * DPState - State in the DP table for a subset of tables
 */
struct DPState {
    int mask;
    double cost;
    int64_t size;
    std::shared_ptr<PlanNode> plan;
    std::string bestAlg;
    std::string sortedOn;

    DPState() : mask(0), cost(0), size(0), plan(nullptr), sortedOn("") {}
    DPState(int m, double c, int64_t s, std::shared_ptr<PlanNode> p, const std::string& a, const std::string& sorted = "")
        : mask(m), cost(c), size(s), plan(p), bestAlg(a), sortedOn(sorted) {}
};

/**
 * Optimizer - Bitmask DP to find optimal join order
 */
class Optimizer {
private:
    const Catalog* catalog;
    SizeEstimator* sizeEst;
    CostModel* costModel;

    // Get table names in a mask
    std::vector<std::string> getTablesInMask(const std::vector<std::string>& tables, int mask) const {
        std::vector<std::string> result;
        for (int i = 0; i < (int)tables.size(); i++) {
            if (mask & (1 << i)) {
                result.push_back(tables[i]);
            }
        }
        return result;
    }

public:
    Optimizer(const Catalog* c, SizeEstimator* se, CostModel* cm)
        : catalog(c), sizeEst(se), costModel(cm) {}

    DPState optimize(const JoinGraph& graph) const {
        const std::vector<std::string>& tables = graph.base_tables;
        const std::vector<JoinPredicate>& predicates = graph.join_conditions;

        int n = tables.size();
        if (n == 0) return DPState();

        // --- Step 2: Virtual Stats Pass ---
        std::map<std::string, int64_t> virtualSizes;
        double totalSelectionCost = 0;

        for (const std::string& tableName : tables) {
            int64_t rows = catalog->getRowCount(tableName);

            // Apply Selection Push-Down heuristics
            for (const std::string& cond : graph.selection_conditions) {
                // Simple heuristic: if condition contains tableName, it might apply
                if (cond.find(tableName) != std::string::npos) {
                    // In a real system, we'd parse the condition to see if it's an equality on a key
                    // For this project, we simulate a 10% selectivity for general filters
                    // and a higher selectivity for key matches.
                    // Parse column name from condition like "students.age = 20"
                    size_t dotPos = cond.find('.');
                    size_t eqPos = cond.find(' ');
                    if (dotPos != std::string::npos && eqPos != std::string::npos) {
                        std::string colName = cond.substr(dotPos + 1, eqPos - dotPos - 1);
                        int64_t distinctVals = catalog->getDistinct(tableName, colName);
                        if (distinctVals > 0) rows = rows / distinctVals;
                    }
                    totalSelectionCost += (double)catalog->getRowCount(tableName) - rows;
                }
            }
            virtualSizes[tableName] = rows;
        }
        // -----------------------------------

        std::map<int, DPState> dp;

        // Base case: single tables
        for (int i = 0; i < n; i++) {
    int mask = 1 << i;
    std::string tableName = tables[i];
    int64_t rows = virtualSizes[tableName];

    auto scanNode = std::make_shared<PlanNode>(SCAN);
    scanNode->tableName = tableName;

    // ADD THIS BLOCK ↓
    std::shared_ptr<PlanNode> baseNode = scanNode;
    for (const std::string& cond : graph.selection_conditions) {
        if (cond.find(tableName) != std::string::npos || tables.size() == 1) {
            auto filterNode = std::make_shared<PlanNode>(FILTER);
            filterNode->condition = cond;
            filterNode->left = baseNode;
            baseNode = filterNode;
        }
    }
    // ADD THIS BLOCK ↑

    dp[mask] = DPState(mask, 0.0, rows, baseNode, "SCAN", catalog->getSortedColumn(tableName));
}

        // Fill DP table for all subsets (process by increasing bit count)
        for (int bits = 2; bits <= n; bits++) {
            for (int mask = 1; mask < (1 << n); mask++) {
                int bitCount = 0;
                for (int tmp = mask; tmp > 0; tmp >>= 1) {
                    bitCount += (tmp & 1);
                }
                if (bitCount != bits) continue;

                double minCost = std::numeric_limits<double>::max();
                int64_t minSize = 0;
                std::shared_ptr<PlanNode> bestPlan = nullptr;
                std::string bestAlg = "";
                bool found = false;
                std::string leftCol, rightCol;

                // Enumerate all ways to split mask into two non-empty submasks
                for (int submask = mask & (mask - 1); submask > 0; submask = (submask - 1) & mask) {
                    int rightMask = mask ^ submask;
                    if (rightMask == 0) continue;

                    // Check if both submasks have valid plans (they must be joinable)
                    if (dp.find(submask) == dp.end() || dp[submask].plan == nullptr) {
                        continue;
                    }
                    if (dp.find(rightMask) == dp.end() || dp[rightMask].plan == nullptr) {
                        continue;
                    }

                    // Check if there's a predicate connecting these two groups
                    bool foundPred = false;
                    std::string pred_t1, pred_t2, pred_c1, pred_c2;

                    for (const JoinPredicate& p : predicates) {
                        bool p_t1_in_sub = false, p_t2_in_sub = false;
                        bool p_t1_in_right = false, p_t2_in_right = false;

                        for (int i = 0; i < (int)tables.size(); i++) {
                            if (submask & (1 << i)) {
                                if (tables[i] == p.table1) p_t1_in_sub = true;
                                if (tables[i] == p.table2) p_t2_in_sub = true;
                            }
                            if (rightMask & (1 << i)) {
                                if (tables[i] == p.table1) p_t1_in_right = true;
                                if (tables[i] == p.table2) p_t2_in_right = true;
                            }
                        }

                        if ((p_t1_in_sub && p_t2_in_right) || (p_t2_in_sub && p_t1_in_right)) {
                            pred_t1 = p.table1;
                            pred_t2 = p.table2;
                            pred_c1 = p.col1;
                            pred_c2 = p.col2;
                            foundPred = true;
                            break;
                        }
                    }

                    double crossProductPenalty = 1000000.0;
                    if (!foundPred) {
                        // Heuristic: Penalize Cross Products
                        if (minCost < crossProductPenalty) {
                            continue;
                        }
                    } else {
                        // We have a join predicate, use it for size estimation
                    }

                    // Determine left/right for size estimation
                    std::string leftTable, rightTable, leftCol, rightCol;
                    if (foundPred) {
                        // Identify which table is in submask and which is in rightMask
                        bool t1_in_sub = false;
                        for(int i=0; i<(int)tables.size(); ++i) {
                            if((submask & (1<<i)) && tables[i] == pred_t1) { t1_in_sub = true; break; }
                        }
                        if (t1_in_sub) {
                            leftTable = pred_t1; leftCol = pred_c1;
                            rightTable = pred_t2; rightCol = pred_c2;
                        } else {
                            leftTable = pred_t2; leftCol = pred_c2;
                            rightTable = pred_t1; rightCol = pred_c1;
                        }
                    } else {
                        // Cross product: use the actual table names from each side for size estimation
                        // Get table names from the dp state
                        const std::vector<std::string>& subTables = getTablesInMask(tables, submask);
                        const std::vector<std::string>& rightTables = getTablesInMask(tables, rightMask);
                        leftTable = subTables.empty() ? "" : subTables[0];
                        rightTable = rightTables.empty() ? "" : rightTables[0];
                        leftCol = ""; rightCol = "";  // Empty columns indicate cross product
                    }

                    // Constrain to Left-Deep Joins:
                    // One side of the join must be a base table (mask with only 1 bit set)
                    int submaskBits = 0;
                    for (int tmp = submask; tmp > 0; tmp >>= 1) if (tmp & 1) submaskBits++;
                    int rightMaskBits = 0;
                    for (int tmp = rightMask; tmp > 0; tmp >>= 1) if (tmp & 1) rightMaskBits++;

                    if (submaskBits != 1 && rightMaskBits != 1) {
                        continue; // Not a left-deep (or right-deep) split
                    }

                    // Calculate join size
                    int64_t leftRows = dp[submask].size;
                    int64_t rightRows = dp[rightMask].size;
                    int64_t joinSize = sizeEst->estimateJoinSize(leftTable, rightTable, leftCol, rightCol);

                    // Find best algorithm
                    double joinCost;
                    bool leftSorted = (dp[submask].sortedOn == leftCol);
                    bool rightSorted = (dp[rightMask].sortedOn == rightCol);
                    std::string alg = costModel->findBestAlgorithm(leftRows, rightRows, joinCost, leftSorted, rightSorted);

                    // Total cost
                    double totalCost = dp[submask].cost + dp[rightMask].cost + joinCost;

                    if (totalCost < minCost) {
                        minCost = totalCost;
                        minSize = joinSize;

                        auto joinNode = std::make_shared<PlanNode>(JOIN);
                        joinNode->algorithm = (alg == "Hash") ? HASH : (alg == "BNLJ") ? BNLJ : MERGE;
                        joinNode->condition = (foundPred) ? (leftTable + "." + leftCol + " = " + rightTable + "." + rightCol) : "CROSS PRODUCT";
                        joinNode->left = dp[submask].plan;
                        joinNode->right = dp[rightMask].plan;

                        bestPlan = joinNode;
                        bestAlg = alg;
                        found = true;
                    }
                }

                if (found) {
                    std::string finalSortedOn = (bestAlg == "Merge") ? leftCol : "";
                    dp[mask] = DPState(mask, minCost, minSize, bestPlan, bestAlg, finalSortedOn);
                }
            }
        }

        int finalMask = (1 << n) - 1;
        return dp[finalMask];
    }
};

#endif
