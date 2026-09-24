cmake_minimum_required(VERSION 3.20)
project(market-data-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- MySQL connector (X DevAPI / JDBC-style API) via vcpkg ---
find_package(unofficial-mysql-connector-cpp CONFIG REQUIRED)

# --- ITCH 5.0 binary parser ---
add_library(parser STATIC
    src/parser/itch_parser.cpp
)
target_include_directories(parser PUBLIC src)

# --- In-memory order-book engine ---
# Defined before persistence: SnapshotWriter (persistence) writes from an
# OrderBook (orderbook), so persistence depends on this target.
add_library(orderbook STATIC
    src/orderbook/order_book.cpp
    src/orderbook/book_manager.cpp
)
target_include_directories(orderbook PUBLIC src)
target_link_libraries(orderbook PUBLIC parser)

# --- MySQL persistence layer ---
add_library(persistence STATIC
    src/persistence/db_connection.cpp
    src/persistence/ingest_writer.cpp
    src/persistence/snapshot_writer.cpp
    src/persistence/loader.cpp
)
target_include_directories(persistence PUBLIC src)
target_link_libraries(persistence PUBLIC parser orderbook unofficial::mysql-connector-cpp::connector)

# --- SQL analytics layer ---
# Depends on persistence for DbConnection/DbConfig (query_runner executes
# against an already-open DbConnection; it doesn't open its own).
add_library(analytics STATIC
    src/analytics/query_runner.cpp
)
target_include_directories(analytics PUBLIC src)
target_link_libraries(analytics PUBLIC persistence unofficial::mysql-connector-cpp::connector)

# --- Performance benchmarks ---
add_executable(bench_inserts bench/bench_inserts.cpp)
target_link_libraries(bench_inserts PRIVATE persistence unofficial::mysql-connector-cpp::connector)

add_executable(bench_queries bench/bench_queries.cpp)
target_link_libraries(bench_queries PRIVATE persistence analytics unofficial::mysql-connector-cpp::connector)
# Needs an absolute path to sql/002_indexes.sql regardless of the binary's
# working directory when run -- same pattern as mde_tests below.
target_compile_definitions(bench_queries PRIVATE MDE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

add_executable(bench_throughput bench/bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE parser orderbook)
# Needs an absolute path to data/itch_sample_12302019_100mb.bin regardless
# of the binary's working directory when run. No DB link needed -- Part C
# is pure in-memory parsing/order-book work.
target_compile_definitions(bench_throughput PRIVATE MDE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

# --- Tests ---
enable_testing()
find_package(GTest CONFIG REQUIRED)

add_executable(mde_tests
    tests/test_binary_reader.cpp
    tests/test_parser.cpp
    tests/test_persistence.cpp
    tests/test_order_book.cpp
    tests/test_analytics.cpp
)
target_link_libraries(mde_tests PRIVATE persistence parser orderbook analytics GTest::gtest GTest::gtest_main)

# test_persistence.cpp applies sql/001_schema.sql against the live database;
# it needs an absolute path to that file regardless of ctest's working directory.
target_compile_definitions(mde_tests PRIVATE MDE_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

include(GoogleTest)
gtest_discover_tests(mde_tests)
