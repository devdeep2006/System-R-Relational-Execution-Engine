#include <iostream>
#include "optimizer/ExecutionEngine.h"
#include "optimizer/InMemoryDatabase.h"
#include "optimizer/PlanNode.h"

int main() {
    InMemoryDatabase db;
    
    PlanNode scan(SCAN);
    scan.tableName = "students";
    
    PlanNode filter(FILTER);
    filter.condition = "(id = 5)";
    filter.left = std::make_shared<PlanNode>(scan);
    
    std::cout << "Building tree...\n";
    auto op = buildOperatorTree(&filter, db);
    if (!op) { std::cout << "OP is null\n"; return 1; }
    
    op->open();
    Row* r = op->next();
    if (r) {
        std::cout << "Got row!\n";
    } else {
        std::cout << "No row!\n";
    }
    return 0;
}
