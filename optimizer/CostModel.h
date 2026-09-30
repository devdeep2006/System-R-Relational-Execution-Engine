#ifndef COST_MODEL_H
#define COST_MODEL_H

#include <string>
#include <algorithm>
#include <cmath>

class CostModel {
private:
    // Simulated Hardware Profile
    const double TUPLES_PER_BLOCK = 100.0; 
    const int MEMORY_BLOCKS = 25; // 'm' in your formulas

    // Disk I/O Cost Weights (s * t_s + b * t_b)
    const double T_S = 10.0;  // Average seek cost (e.g., 10 ms)
    const double T_B = 0.1;   // Average block transfer cost (e.g., 0.1 ms)

    // Helper to convert row counts to disk blocks
    int64_t getBlocks(int64_t rows) const {
        if (rows <= 0) return 0;
        return std::ceil(rows / TUPLES_PER_BLOCK);
    }

    // Core cost formula mapping physical operations to time
    double calculateIO(double seeks, double transfers) const {
        return (seeks * T_S) + (transfers * T_B);
    }

public:
    /**
     * Hash Join Cost (Disk-based)
     * Transfers: 3(br + bs) + 4n 
     * Seeks: kr + ks + k'r + k's + 2n 
     */
    double hashJoinCost(int64_t rowsLeft, int64_t rowsRight) const {
        int64_t b_r = getBlocks(rowsLeft);
        int64_t b_s = getBlocks(rowsRight);
        
        // Number of partitions (n)
        int64_t n = std::ceil((double)b_s / MEMORY_BLOCKS); 
        
        // Transfers: 3(br + bs) + 4n 
        double transfers = 3.0 * (b_r + b_s) + 4.0 * n;
        
        // Seeks: partitioned reads/writes happen in chunks of m blocks 
        double k_r = std::ceil((double)b_r / MEMORY_BLOCKS);
        double k_s = std::ceil((double)b_s / MEMORY_BLOCKS);
        double k_r_prime = std::ceil((double)(b_r + n) / MEMORY_BLOCKS);
        double k_s_prime = std::ceil((double)(b_s + n) / MEMORY_BLOCKS);
        
        double seeks = k_r + k_s + k_r_prime + k_s_prime + (2.0 * n);
        
        return calculateIO(seeks, transfers); 
    }

    /**
     * BNLJ Cost (Block Nested Loop Join)
     * Transfers: br + ceil(br / (m-1)) * bs
     * Seeks: 2 * runs
     */
    double bnljCost(int64_t rowsLeft, int64_t rowsRight) const {
        int64_t b_r = getBlocks(rowsLeft); // Outer
        int64_t b_s = getBlocks(rowsRight); // Inner
        
        if (b_r == 0 || b_s == 0) return 0;

        int64_t runs = std::ceil((double)b_r / (MEMORY_BLOCKS - 1));
        
        double transfers = b_r + (runs * b_s);
        double seeks = 2.0 * runs; 
        
        return calculateIO(seeks, transfers);
    }

    /**
     * Merge Join Cost
     * Transfers: sort_transfers + (br + bs)
     * Seeks: sort_seeks + ceil(br/m) + ceil(bs/m)
     */
    double mergeJoinCost(int64_t rowsLeft, int64_t rowsRight, bool leftSorted, bool rightSorted) const {
        int64_t b_r = getBlocks(rowsLeft);
        int64_t b_s = getBlocks(rowsRight);
        
        double totalTransfers = 0;
        double totalSeeks = 0;

        // External Merge Sort Cost Calculation
        auto addSortCost = [&](int64_t b) {
            if (b <= MEMORY_BLOCKS) {
                totalTransfers += 2.0 * b; 
                totalSeeks += 2.0; // 1 to read, 1 to write
                return;
            }
            double initialRuns = std::ceil((double)b / MEMORY_BLOCKS);
            double passes = std::ceil(std::log(initialRuns) / std::log(MEMORY_BLOCKS - 1));
            
            // Transfers: 2b for initial runs + 2b per pass
            totalTransfers += 2.0 * b * (1.0 + passes);
            
            // Seeks: 2 * initialRuns for pass 0, then 2 * b * passes for the merge phases
            totalSeeks += (2.0 * initialRuns) + (2.0 * b * passes);
        };

        if (!leftSorted && b_r > 0) addSortCost(b_r);
        if (!rightSorted && b_s > 0) addSortCost(b_s);

        // Final merge pass 
        totalTransfers += (b_r + b_s);
        totalSeeks += std::ceil((double)b_r / MEMORY_BLOCKS) + std::ceil((double)b_s / MEMORY_BLOCKS);
        
        return calculateIO(totalSeeks, totalTransfers);
    }

    /**
     * Find best algorithm and return its cost
     */
    std::string findBestAlgorithm(int64_t rowsLeft, int64_t rowsRight,
                                  double& minCost, bool leftSorted, bool rightSorted) const {
        
        double cHash1 = hashJoinCost(rowsLeft, rowsRight);       // Left is build
        double cHash2 = hashJoinCost(rowsRight, rowsLeft);       // Right is build
        double cHash = std::min(cHash1, cHash2);

        double cBnlj1 = bnljCost(rowsLeft, rowsRight);           // Left is outer
        double cBnlj2 = bnljCost(rowsRight, rowsLeft);           // Right is outer
        double cBnlj = std::min(cBnlj1, cBnlj2);

        double cMerge = mergeJoinCost(rowsLeft, rowsRight, leftSorted, rightSorted);

        minCost = cMerge;
        std::string bestAlg = "Merge";

        if (cHash < minCost) {
            minCost = cHash;
            bestAlg = "Hash";
        }
        if (cBnlj < minCost) {
            minCost = cBnlj;
            bestAlg = "BNLJ";
        }

        return bestAlg;
    }
};

#endif