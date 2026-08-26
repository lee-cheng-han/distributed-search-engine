# Current limitations

This repository implements an in-memory lexical search engine, a local CLI, and one immutable
checksummed segment that can be reopened and searched after restart. Checksummed manifests and an
atomic `CURRENT` pointer publish durable generations. Bounded mutable writes can flush into multiple
segments, reopen with stale-version protection, resolve versions/tombstones, and compact to one
segment. Flush and compaction are synchronous, resolved query views are materialized in memory, and
obsolete-file reclamation is not implemented. There is no sharding, replication, network API,
metrics, Docker deployment, or measured benchmark yet. Range bounds are schema-validated for keyword, int64, and timestamp fields; persisted
typed values are still scanned rather than read from columnar doc-value structures. The standard
analyzer has documented byte-oriented UTF-8 behavior and no stemming.

The planned distributed design intentionally has no consensus protocol, automatic membership or
shard rebalancing, distributed transactions, learning-to-rank, or Elasticsearch compatibility.
Results will not be added until benchmarks have actually been run.
