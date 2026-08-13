#!/usr/bin/env bash
set -euo pipefail
query=${1:-'distributed OR replication'}
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/dse_index_cli --documents datasets/synthetic/sample.tsv --query "$query" --top-k 10
