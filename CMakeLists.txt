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

# ---------------------------------------------------------------------------
# Core library
# angel_client.cpp uses WinHTTP which is Windows-only. The rest of the core
# (order book, matching engine, market data decoder, pipeline) is portable.
# ---------------------------------------------------------------------------
set(HFT_CORE_SOURCES
    src/order_book.cpp
    src/flat_order_book.cpp
    src/matching_engine.cpp
    src/market_data.cpp
    src/trading_pipeline.cpp
)

if (WIN32)
    list(APPEND HFT_CORE_SOURCES src/angel_client.cpp)
endif()

add_library(hft_core STATIC ${HFT_CORE_SOURCES})

if (WIN32)
    target_link_libraries(hft_core PUBLIC winhttp)
endif()

target_include_directories(hft_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------
# Example executables
# ---------------------------------------------------------------------------

# Example simulation executable
add_executable(hft_sim
    examples/basic_simulation.cpp
)
target_link_libraries(hft_sim PRIVATE hft_core)

# Market data CLI tool executable
add_executable(hft_market_data
    examples/hft_market_data.cpp
)
target_link_libraries(hft_market_data PRIVATE hft_core)

# Unified execution pipeline and integrated demonstration executable
add_executable(hft_pipeline_demo
    examples/hft_pipeline_demo.cpp
)
target_link_libraries(hft_pipeline_demo PRIVATE hft_core)

# ---------------------------------------------------------------------------
# Test suite
# ---------------------------------------------------------------------------
add_executable(hft_tests
    tests/main.cpp
    tests/test_engine.cpp
    tests/test_market_data.cpp
    tests/test_pipeline.cpp
    tests/test_alloc.cpp
)
target_link_libraries(hft_tests PRIVATE hft_core)
target_include_directories(hft_tests PRIVATE tests)

# ---------------------------------------------------------------------------
# Benchmark executables
# All benchmarks include benchmarks/ for bench_timer.hpp
# ---------------------------------------------------------------------------
add_executable(hft_benchmark
    benchmarks/benchmark.cpp
)
target_link_libraries(hft_benchmark PRIVATE hft_core)
target_include_directories(hft_benchmark PRIVATE benchmarks)

add_executable(price_level_benchmark
    benchmarks/price_level_benchmark.cpp
)
target_link_libraries(price_level_benchmark PRIVATE hft_core)
target_include_directories(price_level_benchmark PRIVATE benchmarks)

add_executable(queue_benchmark
    benchmarks/queue_benchmark.cpp
)
target_link_libraries(queue_benchmark PRIVATE hft_core)
target_include_directories(queue_benchmark PRIVATE benchmarks)

add_executable(market_data_benchmark
    benchmarks/market_data_benchmark.cpp
)
target_link_libraries(market_data_benchmark PRIVATE hft_core)
target_include_directories(market_data_benchmark PRIVATE benchmarks)

add_executable(execution_pipeline_benchmark
    benchmarks/execution_pipeline_benchmark.cpp
)
target_link_libraries(execution_pipeline_benchmark PRIVATE hft_core)
target_include_directories(execution_pipeline_benchmark PRIVATE benchmarks)

# ---------------------------------------------------------------------------
# Optional fuzzer target (libFuzzer on Clang, standalone driver on MSVC/GCC)
# ---------------------------------------------------------------------------
option(HFT_ENABLE_FUZZING "Build decoder fuzz target" OFF)

if (HFT_ENABLE_FUZZING)
    add_executable(fuzz_decoder
        tests/fuzz_decoder.cpp
    )
    target_link_libraries(fuzz_decoder PRIVATE hft_core)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(fuzz_decoder PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_options(fuzz_decoder PRIVATE -fsanitize=fuzzer,address,undefined)
    else()
        target_compile_definitions(fuzz_decoder PRIVATE FUZZ_STANDALONE_DRIVER=1)
    endif()
endif()

# ---------------------------------------------------------------------------
# CTest integration
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME CoreUnitAndDeterminismTests COMMAND hft_tests)
