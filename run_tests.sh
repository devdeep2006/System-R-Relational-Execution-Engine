#!/bin/bash

# Clean up old output files
rm -f *.out

# Compile the project
echo "--- Compiling DBMS Engine ---"
clang++ -std=c++17 main.cpp SQLToRATreeConverter.cpp -I. -Ioptimizer -Isql-parser/src -Lsql-parser -lsqlparser -o optimizer_engine

# Check if compilation succeeded
if [ $? -eq 0 ]; then
    echo "--- Running Test Suite ---"
    
    # Set library path for macOS to find sql-parser
    export DYLD_LIBRARY_PATH=./sql-parser:$DYLD_LIBRARY_PATH
    
    # Execute the engine
    ./optimizer_engine
    
    echo "--- All tests completed. Results saved to TC*.out files ---"
else
    echo "Error: Compilation failed."
    exit 1
fi
