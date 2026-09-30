#ifndef SIZE_ESTIMATOR_H
#define SIZE_ESTIMATOR_H

#include "InMemoryDatabase.h"
#include "Catalog.h"
#include <algorithm>

/**
 * SizeEstimator - Estimates result sizes for joins and selections
 * Formula for joins: (nr * ns) / max(v_left, v_right)
 * - nr, ns: number of rows in each table
 * - v_left, v_right: distinct values in join columns
 *
 * For selections with MAX/MIN:
 * - MAX on a column: ~10% of rows (selective on high values)
 * - MIN on a column: ~10% of rows (selective on low values)
 */
class SizeEstimator {
private:
    const Catalog* catalog;

public:
    SizeEstimator(const Catalog* c) : catalog(c) {}

    /**
     * Estimate size of joining two tables
     * @param leftTable  Name of left table
     * @param rightTable Name of right table
     * @param leftCol    Join column in left table
     * @param rightCol   Join column in right table
     * @return Estimated number of rows after join
     */
    int64_t estimateJoinSize(const std::string& leftTable,
                             const std::string& rightTable,
                             const std::string& leftCol,
                             const std::string& rightCol) const {
        // Cross product: when leftCol or rightCol is empty
        if (leftCol.empty() || rightCol.empty()) {
            int64_t nr = catalog->getRowCount(leftTable);
            int64_t ns = catalog->getRowCount(rightTable);
            return nr * ns;  // Full cross product
        }

        int64_t nr = catalog->getRowCount(leftTable);
        int64_t ns = catalog->getRowCount(rightTable);

        int64_t vLeft = catalog->getDistinct(leftTable, leftCol);
        int64_t vRight = catalog->getDistinct(rightTable, rightCol);

        // Use max distinct value for conservative estimate
        int64_t maxV = (vLeft > vRight) ? vLeft : vRight;
        if (maxV <= 0) maxV = 1;  // Avoid division by zero

        return (nr * ns) / maxV;
    }

    /**
     * Estimate size of a selection (WHERE clause)
     * Uses MAX/MIN heuristics for better selectivity estimation
     * - Equality on key: ~1 row
     * - Range with MAX/MIN: ~10% of rows
     * - General filter: ~50% of rows
     */
    int64_t estimateSelectionSize(const std::string& tableName, const std::string& condition) const {
        int64_t rowCount = catalog->getRowCount(tableName);
        if (rowCount <= 0) return rowCount;

        std::string upperCond = condition;
        std::transform(upperCond.begin(), upperCond.end(), upperCond.begin(), ::toupper);

        // Check for MAX() or MIN() - these are very selective (top/bottom values)
        bool hasMax = upperCond.find("MAX(") != std::string::npos;
        bool hasMin = upperCond.find("MIN(") != std::string::npos;

        if (hasMax || hasMin) {
            // MAX/MIN operations typically return ~10% of rows (top/bottom values)
            return rowCount / 10;
        }

        // Check for equality conditions on known columns
        // e.g., "age = 20" or "students.id = 5"
        if (condition.find("=") != std::string::npos && condition.find("!=") == std::string::npos) {
            // Likely an equality condition - use distinct value info
            // Simple heuristic: ~10% for equality (can be tuned)
            return rowCount / 10;
        }

        // General case: ~50% selectivity
        return rowCount / 2;
    }
};

#endif
