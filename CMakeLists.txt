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
    src/event_pipeline.cpp
    src/event_recorder.cpp
    src/event_replayer.cpp
    src/baseline/baseline_order_book.cpp
    src/baseline/baseline_matching_engine.cpp
    src/broker/angel_decoder.cpp
    src/broker/mock_angel_feed.cpp
    src/broker/angel_client.cpp
    src/market_data_pipeline.cpp
    src/market_recorder.cpp
    src/market_replayer.cpp
)

if (WIN32)
    target_link_libraries(hft_core PUBLIC winhttp)
endif()

target_include_directories(hft_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Example simulation executable
add_executable(hft_sim
    examples/basic_simulation.cpp
)
target_link_libraries(hft_sim PRIVATE hft_core)

# Binary log tool executable
add_executable(hft_log_tool
    examples/hft_log_tool.cpp
)
target_link_libraries(hft_log_tool PRIVATE hft_core)

# Market data CLI tool executable
add_executable(hft_market_data
    examples/hft_market_data.cpp
)
target_link_libraries(hft_market_data PRIVATE hft_core)

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
    tests/test_spsc_queue.cpp
    tests/test_pipeline_equivalence.cpp
    tests/test_binary_log.cpp
    tests/test_angel_market_data.cpp
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

# SPSC Queue microbenchmark executable
add_executable(queue_benchmark
    benchmarks/queue_benchmark.cpp
)
target_link_libraries(queue_benchmark PRIVATE hft_core)

# Event pipeline benchmark executable
add_executable(pipeline_benchmark
    benchmarks/pipeline_benchmark.cpp
)
target_link_libraries(pipeline_benchmark PRIVATE hft_core)

# Binary replay benchmark executable
add_executable(replay_benchmark
    benchmarks/replay_benchmark.cpp
)
target_link_libraries(replay_benchmark PRIVATE hft_core)

# Market data benchmark executable
add_executable(market_data_benchmark
    benchmarks/market_data_benchmark.cpp
)
target_link_libraries(market_data_benchmark PRIVATE hft_core)

# Enable CTest integration
enable_testing()
add_test(NAME CoreUnitAndDeterminismTests COMMAND hft_tests)

