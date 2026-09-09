# Reproducible local benchmark

Configure a Release build and run `dse_benchmark --output benchmarks/results/local-baseline.json`.
The runner uses a fixed seed and records its workload, compiler, available hardware concurrency,
indexing throughput, p50/p95/p99 search latency, cache-hit cost, fixed-width/compressed segment sizes,
and a result checksum. Raw JSON is checked in only after an actual run.

This deterministic synthetic workload is a regression baseline, not a claim about web-scale search.
The library also provides verified precision/recall, MAP, MRR, and nDCG evaluators.

For the recognized 1,400-document Cranfield collection, run:

```sh
./benchmarks/fetch_cranfield.sh /tmp/dse-cranfield
./build-release/dse_cranfield_eval /tmp/dse-cranfield \
  benchmarks/results/cranfield-bm25.json 100
```

The fetcher pins the archive SHA-256. Cranfield relevance codes are converted to increasing grades
and all reported values are macro-averages across the 225 queries.
