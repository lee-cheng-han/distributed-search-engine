cmake_minimum_required(VERSION 3.25)
project(distributed_search_engine VERSION 0.1.0 LANGUAGES CXX)

option(DSE_BUILD_TESTS "Build tests" ON)
option(DSE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(DSE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(DSE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(DSE_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

if(DSE_ENABLE_TSAN AND (DSE_ENABLE_ASAN OR DSE_ENABLE_UBSAN))
  message(FATAL_ERROR "ThreadSanitizer must be enabled on its own")
endif()

add_library(dse
  src/analysis/analyzer.cpp
  src/index/document_file.cpp
  src/index/schema.cpp
  src/index/in_memory_index.cpp
  src/query/lexer.cpp
  src/query/executor.cpp
  src/query/parser.cpp
  src/query/planner.cpp
  src/ranking/bm25.cpp
  src/ranking/top_k.cpp
  src/storage/segment.cpp
  src/storage/manifest.cpp
  src/storage/generation.cpp
  src/storage/index_writer.cpp
)
add_library(dse::dse ALIAS dse)
target_compile_features(dse PUBLIC cxx_std_23)
target_include_directories(dse PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

if(DSE_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "DSE_BUILD_FUZZERS requires Clang")
  endif()
  target_compile_options(dse PRIVATE -fsanitize=fuzzer-no-link,address,undefined
    -fno-omit-frame-pointer)
  target_link_options(dse INTERFACE -fsanitize=fuzzer-no-link,address,undefined)
  add_executable(dse_query_fuzz fuzz/query_pipeline_fuzz.cpp)
  target_link_libraries(dse_query_fuzz PRIVATE dse::dse)
  target_compile_options(dse_query_fuzz PRIVATE -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer)
  target_link_options(dse_query_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(dse_segment_fuzz fuzz/segment_reader_fuzz.cpp)
  target_link_libraries(dse_segment_fuzz PRIVATE dse::dse)
  target_compile_options(dse_segment_fuzz PRIVATE -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer)
  target_link_options(dse_segment_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dse PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(DSE_ENABLE_ASAN)
    target_compile_options(dse PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(dse PUBLIC -fsanitize=address)
  endif()
  if(DSE_ENABLE_UBSAN)
    target_compile_options(dse PUBLIC -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(dse PUBLIC -fsanitize=undefined)
  endif()
  if(DSE_ENABLE_TSAN)
    target_compile_options(dse PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(dse PUBLIC -fsanitize=thread)
  endif()
endif()

add_executable(dse_index_cli apps/index_cli/main.cpp)
target_link_libraries(dse_index_cli PRIVATE dse::dse)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dse_index_cli PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(DSE_BUILD_TESTS)
  include(CTest)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND AND EXISTS /usr/src/googletest/CMakeLists.txt)
    add_subdirectory(/usr/src/googletest "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
  elseif(NOT GTest_FOUND)
    message(FATAL_ERROR "GoogleTest not found; install it or configure with -DDSE_BUILD_TESTS=OFF")
  endif()
  add_executable(dse_unit_tests
    tests/support/reference_evaluator.cpp
    tests/unit/analyzer_test.cpp
    tests/unit/bm25_test.cpp
    tests/unit/document_file_test.cpp
    tests/unit/generation_test.cpp
    tests/unit/in_memory_index_test.cpp
    tests/unit/index_writer_test.cpp
    tests/unit/manifest_test.cpp
    tests/unit/query_executor_test.cpp
    tests/unit/query_differential_test.cpp
    tests/unit/query_hardening_test.cpp
    tests/unit/query_parser_test.cpp
    tests/unit/query_planner_test.cpp
    tests/unit/schema_test.cpp
    tests/unit/segment_test.cpp
    tests/unit/top_k_test.cpp
  )
  target_link_libraries(dse_unit_tests PRIVATE dse::dse GTest::gtest_main)
  target_compile_definitions(dse_unit_tests PRIVATE DSE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  include(GoogleTest)
  # PRE_TEST avoids executing sanitizer-instrumented binaries during the build.
  gtest_discover_tests(dse_unit_tests DISCOVERY_MODE PRE_TEST)
  add_test(NAME IndexCli.EndToEnd
    COMMAND dse_index_cli
      --documents "${CMAKE_CURRENT_SOURCE_DIR}/datasets/synthetic/sample.tsv"
      --query "title:\"distributed systems\" OR body:replication"
      --top-k 3)
  set_tests_properties(IndexCli.EndToEnd PROPERTIES
    PASS_REGULAR_EXPRESSION "indexed_documents[^0-9]*5.*total_hits[^0-9]*2.*doc-001.*doc-003")
  set(DSE_CLI_SEGMENT "${CMAKE_CURRENT_BINARY_DIR}/cli-restart-test.dseg")
  add_test(NAME IndexCli.BuildSegment
    COMMAND dse_index_cli --documents "${CMAKE_CURRENT_SOURCE_DIR}/datasets/synthetic/sample.tsv"
      --write-segment "${DSE_CLI_SEGMENT}" --query "search" --top-k 3)
  set_tests_properties(IndexCli.BuildSegment PROPERTIES FIXTURES_SETUP cli_segment
    PASS_REGULAR_EXPRESSION "indexed_documents[^0-9]*5")
  add_test(NAME IndexCli.SearchReopenedSegment
    COMMAND dse_index_cli --segment "${DSE_CLI_SEGMENT}"
      --query "title:distributed OR body:replication" --top-k 3)
  set_tests_properties(IndexCli.SearchReopenedSegment PROPERTIES FIXTURES_REQUIRED cli_segment
    PASS_REGULAR_EXPRESSION "indexed_documents[^0-9]*5.*total_hits[^0-9]*2.*doc-001.*doc-003")
  find_program(DSE_SHA256SUM sha256sum)
  if(DSE_SHA256SUM)
    add_test(NAME Segment.GoldenFormat
      COMMAND "${DSE_SHA256SUM}" --check
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/segment_v1.sha256")
    set_tests_properties(Segment.GoldenFormat PROPERTIES FIXTURES_REQUIRED cli_segment
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endif()
