cmake_minimum_required(VERSION 3.16)
project(SystemRQueryEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(sysr STATIC ${ENGINE_SOURCES})
target_include_directories(sysr PUBLIC src)

add_executable(sysr_shell tools/sysr_shell.cpp)
target_link_libraries(sysr_shell PRIVATE sysr)

add_executable(sysr_bench tools/sysr_bench.cpp)
target_link_libraries(sysr_bench PRIVATE sysr)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(sysr_tests ${TEST_SOURCES})
target_include_directories(sysr_tests PRIVATE tests)
target_link_libraries(sysr_tests PRIVATE sysr)

enable_testing()
add_test(NAME sysr_tests COMMAND sysr_tests)
