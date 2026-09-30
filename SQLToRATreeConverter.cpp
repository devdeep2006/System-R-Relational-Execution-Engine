#include "SQLToRATreeConverter.h"
#include <iostream>
#include "SQLParser.h"

// =============================================================================
// RA Node Implementation
// =============================================================================

// TableNode
std::string TableNode::toString(int indent) const {
    std::string spaces(indent, ' ');
    return spaces + "TableNode: " + tableName;
}

// ProjectNode
ProjectNode::~ProjectNode() {
    for (auto child : children) {
        delete child;
    }
}

std::string ProjectNode::toString(int indent) const {
    std::string spaces(indent, ' ');
    std::string result = spaces + "ProjectNode: [";
    for (size_t i = 0; i < columns.size(); ++i) {
        result += columns[i];
        if (i < columns.size() - 1) result += ", ";
    }
    result += "]\n";
    for (auto child : children) {
        result += child->toString(indent + 2) + "\n";
    }
    return result;
}

// SelectNode
SelectNode::~SelectNode() {
    delete child;
}

std::string SelectNode::toString(int indent) const {
    std::string spaces(indent, ' ');
    std::string result = spaces + "SelectNode: WHERE " + condition + "\n";
    result += child->toString(indent + 2);
    return result;
}

// JoinNode
JoinNode::~JoinNode() {
    delete left;
    delete right;
}

std::string JoinNode::toString(int indent) const {
    std::string spaces(indent, ' ');
    std::string result = spaces + "JoinNode: JOIN ON " + joinCondition + "\n";
    result += left->toString(indent + 2) + "\n";
    result += right->toString(indent + 2);
    return result;
}

// =============================================================================
// SQL to RA Tree Converter
// =============================================================================

// Helper to extract column names from Expr vector
static std::vector<std::string> extractColumns(std::vector<hsql::Expr*>* exprList) {
    std::vector<std::string> cols;
    if (!exprList) return cols;

    for (auto expr : *exprList) {
        if (!expr) continue;

        switch (expr->type) {
            case hsql::kExprStar:
                cols.push_back("*");
                break;
            case hsql::kExprColumnRef: {
                std::string colName = expr->name ? expr->name : "";
                if (expr->table && expr->table[0] != '\0') {
                    colName = std::string(expr->table) + "." + colName;
                }
                cols.push_back(colName);
                break;
            }
            case hsql::kExprLiteralInt: {
                std::string val = std::to_string(expr->ival);
                cols.push_back(val);
                break;
            }
            case hsql::kExprLiteralFloat: {
                std::string val = std::to_string(expr->fval);
                cols.push_back(val);
                break;
            }
            case hsql::kExprLiteralString: {
                std::string val = expr->name ? std::string(expr->name) : "";
                cols.push_back("'" + val + "'");
                break;
            }
            default:
                cols.push_back("<expr>");
                break;
        }
    }
    return cols;
}

// Helper to extract WHERE clause condition as string
static std::string extractCondition(hsql::Expr* expr) {
    if (!expr) return "";

    switch (expr->type) {
        case hsql::kExprColumnRef: {
            std::string result = expr->table ? expr->table : "";
            if (!result.empty()) result += ".";
            result += expr->name ? expr->name : "";
            return result;
        }
        case hsql::kExprLiteralInt:
            return std::to_string(expr->ival);
        case hsql::kExprLiteralFloat:
            return std::to_string(expr->fval);
        case hsql::kExprLiteralString: {
            std::string val = expr->name ? expr->name : "";
            return "'" + val + "'";
        }
        case hsql::kExprOperator: {
            std::string left = extractCondition(expr->expr);
            std::string right = extractCondition(expr->expr2);
            std::string op;
            switch (expr->opType) {
                case hsql::kOpEquals: op = "="; break;
                case hsql::kOpNotEquals: op = "!="; break;
                case hsql::kOpLess: op = "<"; break;
                case hsql::kOpLessEq: op = "<="; break;
                case hsql::kOpGreater: op = ">"; break;
                case hsql::kOpGreaterEq: op = ">="; break;
                case hsql::kOpAnd: op = "AND"; break;
                case hsql::kOpOr: op = "OR"; break;
                default: op = "?"; break;
            }
            return "(" + left + " " + op + " " + right + ")";
        }
        default:
            return "<condition>";
    }
}

// Helper to extract JOIN condition as string (for ON clause)
static std::string extractJoinCondition(hsql::Expr* expr) {
    if (!expr) return "";
    return extractCondition(expr);
}

// Recursively build RA tree from TableRef (handles nested JOINs)
static RANode* buildRAFromTableRef(const hsql::TableRef* tableRef) {
    if (!tableRef) return nullptr;

    switch (tableRef->type) {
        case hsql::kTableName: {
            // Base table
            if (tableRef->name) {
                return new TableNode(tableRef->name);
            }
            return nullptr;
        }

        case hsql::kTableJoin: {
            // JOIN operation
            const hsql::JoinDefinition* join = tableRef->join;
            if (!join) return nullptr;

            // Recursively build left and right sides
            RANode* leftSide = buildRAFromTableRef(join->left);
            RANode* rightSide = buildRAFromTableRef(join->right);

            if (!leftSide || !rightSide) {
                delete leftSide;
                delete rightSide;
                return nullptr;
            }

            // Extract JOIN condition
            std::string joinCond = extractJoinCondition(join->condition);

            // Determine JOIN type
            std::string joinTypeStr = "";
            switch (join->type) {
                case hsql::kJoinInner: joinTypeStr = "INNER "; break;
                case hsql::kJoinLeft: joinTypeStr = "LEFT "; break;
                case hsql::kJoinRight: joinTypeStr = "RIGHT "; break;
                case hsql::kJoinFull: joinTypeStr = "FULL "; break;
                case hsql::kJoinCross: joinTypeStr = "CROSS "; break;
                case hsql::kJoinNatural: joinTypeStr = "NATURAL "; break;
                default: break;
            }

            // Build the JoinNode with join type in condition for clarity
            std::string fullCondition = joinTypeStr + joinCond;
            if (joinCond.empty()) {
                fullCondition = joinTypeStr + "JOIN";
            }

            return new JoinNode(leftSide, rightSide, fullCondition);
        }

        default:
            return nullptr;
    }
}

// Main conversion function - parses SQL and returns RA tree
RANode* parseSQLToRA(const std::string& query) {
    hsql::SQLParserResult result;
    hsql::SQLParser::parse(query, &result);

    if (!result.isValid() || result.size() == 0) {
        std::cerr << "Parse failed: " << result.errorMsg() << std::endl;
        return nullptr;
    }

    // Handle only SELECT statements for now
    auto stmt = result.getStatement(0);
    if (!stmt->isType(hsql::kStmtSelect)) {
        std::cerr << "Only SELECT statements supported in this demo" << std::endl;
        return nullptr;
    }

    auto selectStmt = static_cast<const hsql::SelectStatement*>(stmt);

    // Build RA tree top-down (root first, then children)

    // 1. Build the table side (handles FROM with JOINs)
    RANode* tableNode = buildRAFromTableRef(selectStmt->fromTable);

    // 2. Create ProjectNode for SELECT columns
    std::vector<std::string> selectCols = extractColumns(selectStmt->selectList);
    ProjectNode* projectNode = new ProjectNode(selectCols);
    if (tableNode) {
        projectNode->children.push_back(tableNode);
    } else {
        // If no table node (edge case), just return project
        return projectNode;
    }

    // 3. Create SelectNode for WHERE clause if present
    if (selectStmt->whereClause) {
        std::string whereCond = extractCondition(selectStmt->whereClause);
        SelectNode* selectNode = new SelectNode(projectNode, whereCond);
        return selectNode;
    }

    return projectNode;
}
