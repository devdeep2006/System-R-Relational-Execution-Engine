# Relational Query Engine and Heuristic Optimizer

A high-performance C++ implementation of a cost-based relational query engine. This project features a System-R style optimizer using dynamic programming and a physical execution engine based on the Volcano Iterator model.

##  Key Features

- **Cost-Based Optimizer**: Uses Dynamic Programming with bitmasking to find the optimal join order (Left-Deep Trees), minimizing total I/O cost.
- **Volcano Iterator Execution**: Implement iterative `next()` calls for data operators, enabling pipelined query execution with minimal memory overhead.
- **Physical Operators**: Full implementation of `Selection`, `Projection`, and multiple Join algorithms:
  - **Hash Join**: High performance for large unsorted datasets.
  - **Block Nested Loop Join (BNLJ)**: Robust join algorithm for arbitrary predicates.
  - **Merge Join**: Optimal for pre-sorted inputs.
- **Advanced Cost Modeling**: Simulated disk I/O costs calculating seek times ($t_s$) and block transfer times ($t_b$), including complexity analysis for External Merge Sort.
- **Scalability**: Tested against "Massive" table scenarios (up to 500,000 rows) to verify optimizer decisions at scale.
- **Namespace-Aware Engine**: Robustly handles column name collisions (e.g., `students.id` vs `courses.id`) across multi-table joins.

## Architecture Overview

The system consists of three major layers:

1. **Logical Layer**:
   - `SQLToRATreeConverter`: Translates SQL AST into Relational Algebra nodes.
   - `JoinGraph`: Represents the logical dependencies and filters of the query.

2. **Optimization Layer**:
   - `Optimizer`: The core DP engine that explores the plan space.
   - `SizeEstimator`: Uses catalog statistics to estimate intermediate result sizes.
   - `CostModel`: Evaluates physical costs based on hardware profiles (Seeks vs Transfers).
   - `Catalog`: Manages metadata and statistics for base tables.

3. **Physical Layer**:
   - `ExecutionEngine`: Builds and runs the operator tree.
   - `InMemoryDatabase`: Manages the actual data storage and retrieval.

## 📁 Directory Structure

```text
├── main.cpp                  # Entry point & automated test runner
├── run_tests.sh              # Build and execution automation script
├── test.txt                  # Test query definitions
├── optimizer/
│   ├── Optimizer.h           # DP-based join ordering logic
│   ├── ExecutionEngine.h     # Physical operator implementations
│   ├── CostModel.h           # Disk-based I/O cost math
│   ├── Catalog.h             # Metadata & Table statistics
│   ├── InMemoryDatabase.h    # Mock data & result set management
│   ├── SizeEstimator.h       # Cardinality estimation heuristics
│   ├── JoinGraph.h           # Logical query representation
│   └── PlanNode.h            # Physical plan tree structure
└── sql-parser/               # SQL parsing submodule (hsql)
```

## Installation & Running

### Prerequisites
- C++17 compliant compiler (GCC/Clang)
- `make` or `cmake` (if applicable)
- macOS/Linux environment

### Automated Setup & Test
The easiest way to run the project is using the provided bash script:

```bash
chmod +x run_tests.sh
./run_tests.sh
```

This will:
1. Compile the engine.
2. Link the required SQL parser libraries.
3. Execute all test cases defined in `test.txt`.
4. Generate detailed output logs (`TC*.out`) containing the Optimal Physical Plan, JOIN Sequence, Estimated Costs, and actual result sets.

## Test Suite Highlights

The engine is validated through several TCs (Test Cases):
- **TC1 - TC4**: Basic selection and projection filters.
- **TC5 - TC8**: Complex multi-table joins with selection push-down.
- **TC10_Massive**: A 100k x 500k join which forces the Optimizer to select **Hash Join** over BNLJ due to massive I/O savings.
- **TC11_Sorted**: Demonstrates **Merge Join** superiority when the Catalog reports pre-sorted columns.

##  Design Decisions

- **Left-Deep Strategy**: The optimizer restricts the search space to Left-Deep trees to ensure pipelining compatibility and efficient buffer management.
- **Namespaced Columns**: All internal row lookups use `table.column` format to prevent data corruption when joining tables with identical column names.
- **External Merge Sort Simulation**: The `Merge Join` cost includes the realistic overhead of multi-pass external sorting if the inputs are not already ordered.
