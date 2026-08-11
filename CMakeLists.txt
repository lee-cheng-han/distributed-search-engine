cmake_minimum_required(VERSION 3.25)
project(distributed_search_engine VERSION 0.1.0 LANGUAGES CXX)

option(DSE_BUILD_TESTS "Build tests" ON)
option(DSE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(DSE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(DSE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(DSE_ENABLE_TSAN AND (DSE_ENABLE_ASAN OR DSE_ENABLE_UBSAN))
  message(FATAL_ERROR "ThreadSanitizer must be enabled on its own")
endif()

add_library(dse
  src/analysis/analyzer.cpp
  src/index/in_memory_index.cpp
  src/ranking/bm25.cpp
  src/ranking/top_k.cpp
)
add_library(dse::dse ALIAS dse)
target_compile_features(dse PUBLIC cxx_std_23)
target_include_directories(dse PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

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

if(DSE_BUILD_TESTS)
  include(CTest)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND AND EXISTS /usr/src/googletest/CMakeLists.txt)
    add_subdirectory(/usr/src/googletest "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
  elseif(NOT GTest_FOUND)
    message(FATAL_ERROR "GoogleTest not found; install it or configure with -DDSE_BUILD_TESTS=OFF")
  endif()
  add_executable(dse_unit_tests
    tests/unit/analyzer_test.cpp
    tests/unit/bm25_test.cpp
    tests/unit/in_memory_index_test.cpp
    tests/unit/top_k_test.cpp
  )
  target_link_libraries(dse_unit_tests PRIVATE dse::dse GTest::gtest_main)
  include(GoogleTest)
  # PRE_TEST avoids executing sanitizer-instrumented binaries during the build.
  gtest_discover_tests(dse_unit_tests DISCOVERY_MODE PRE_TEST)
endif()
