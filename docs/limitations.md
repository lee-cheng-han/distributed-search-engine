# Current limitations

This repository currently implements an in-memory lexical search engine and local CLI. It does not yet implement
persistence, compression, segment merging,
concurrency, sharding, replication, network APIs, metrics, Docker deployment, or measured
benchmarks. Range filtering is lexicographic and has no typed field schema. The standard analyzer has
documented byte-oriented UTF-8 behavior and no stemming.

The planned distributed design intentionally has no consensus protocol, automatic membership or
shard rebalancing, distributed transactions, learning-to-rank, or Elasticsearch compatibility.
Results will not be added until benchmarks have actually been run.
