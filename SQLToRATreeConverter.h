#ifndef SQL_TO_RA_TREE_CONVERTER_H
#define SQL_TO_RA_TREE_CONVERTER_H

#include <string>
#include <vector>

// Forward declarations to avoid including SQLParser.h here
// The actual parser will be included in the .cpp file

// =============================================================================
// Relational Algebra Tree (RA Tree) Node Structures
// =============================================================================

struct RANode {
    virtual ~RANode() = default;
    virtual std::string toString(int indent = 0) const = 0;
};

// Represents a base table in the query
struct TableNode : RANode {
    std::string tableName;

    TableNode(std::string name) : tableName(name) {}

    std::string toString(int indent = 0) const override;
};

// Represents a projection (SELECT columns)
struct ProjectNode : RANode {
    std::vector<RANode*> children;
    std::vector<std::string> columns;

    ProjectNode(std::vector<std::string> cols) : columns(cols) {}

    ~ProjectNode() override;

    std::string toString(int indent = 0) const override;
};

// Represents a selection (WHERE clause - filter)
struct SelectNode : RANode {
    RANode* child;
    std::string condition;

    SelectNode(RANode* c, std::string cond) : child(c), condition(cond) {}

    ~SelectNode() override;

    std::string toString(int indent = 0) const override;
};

// Represents a JOIN operation
struct JoinNode : RANode {
    RANode* left;
    RANode* right;
    std::string joinCondition;

    JoinNode(RANode* l, RANode* r, std::string cond) : left(l), right(r), joinCondition(cond) {}

    ~JoinNode() override;

    std::string toString(int indent = 0) const override;
};

// =============================================================================
// SQL to RA Tree Converter
// =============================================================================

/**
 * Converts a SQL query string to an RA tree.
 * @param query The SQL query to parse
 * @return Pointer to the root of the RA tree, or nullptr on failure
 *         Caller is responsible for deleting the tree to free memory
 */
RANode* parseSQLToRA(const std::string& query);

#endif // SQL_TO_RA_TREE_CONVERTER_H
