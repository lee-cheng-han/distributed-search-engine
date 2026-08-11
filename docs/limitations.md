# Current limitations

This repository currently implements the bootstrap and in-memory indexing slice only. It does not
yet implement query parsing/execution, BM25, persistence, compression, segment merging,
concurrency, sharding, replication, network APIs, metrics, Docker deployment, or measured
benchmarks. The standard analyzer has documented byte-oriented UTF-8 behavior and no stemming.

The planned distributed design intentionally has no consensus protocol, automatic membership or
shard rebalancing, distributed transactions, learning-to-rank, or Elasticsearch compatibility.
Results will not be added until benchmarks have actually been run.
