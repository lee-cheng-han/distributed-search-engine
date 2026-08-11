#!/usr/bin/env bash
set -euo pipefail
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DDSE_ENABLE_ASAN=ON -DDSE_ENABLE_UBSAN=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
