cmake_minimum_required(VERSION 3.20)

project(
    hft_engine
    VERSION 0.1.0
    DESCRIPTION "Low-Latency C++ Exchange & Trading Engine"
    LANGUAGES CXX
)

# Enforce C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warning flags
if (MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

# Core library target
add_library(hft_core STATIC
    src/order_book.cpp
    src/matching_engine.cpp
    src/flat_order_book.cpp
    src/flat_matching_engine.cpp
    src/baseline/baseline_order_book.cpp
    src/baseline/baseline_matching_engine.cpp
)

target_include_directories(hft_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Example simulation executable
add_executable(hft_sim
    examples/basic_simulation.cpp
)
target_link_libraries(hft_sim PRIVATE hft_core)

# Automated test suite executable
add_executable(hft_tests
    tests/main.cpp
    tests/test_order_book.cpp
    tests/test_matching_engine.cpp
    tests/test_determinism.cpp
    tests/test_order_pool.cpp
    tests/test_differential.cpp
    tests/test_flat_order_book.cpp
    tests/test_differential_flat.cpp
)
target_link_libraries(hft_tests PRIVATE hft_core)
target_include_directories(hft_tests PRIVATE tests)


# Benchmark executable
add_executable(hft_benchmark
    benchmarks/benchmark.cpp
)
target_link_libraries(hft_benchmark PRIVATE hft_core)

# Price-level microbenchmark executable
add_executable(price_level_benchmark
    benchmarks/price_level_benchmark.cpp
)
target_link_libraries(price_level_benchmark PRIVATE hft_core)


# Enable CTest integration
enable_testing()
add_test(NAME CoreUnitAndDeterminismTests COMMAND hft_tests)

