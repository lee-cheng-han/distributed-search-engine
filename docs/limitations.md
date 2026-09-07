# Current limitations

This repository implements an in-memory lexical search engine, a local CLI, and one immutable
checksummed segment that can be reopened and searched after restart. Checksummed manifests and an
atomic `CURRENT` pointer publish durable generations. Bounded mutable writes can flush into multiple
segments, reopen with stale-version protection, resolve versions/tombstones, and compact to one
segment. Flushes run on one bounded background publisher with explicit refresh semantics; automatic
and explicit compaction serialize against publication and reclaim superseded files after buffered
readers own their data. Resolved query views and compaction are still materialized in memory. There
is a checksummed fsynced mutation log, but no group-commit mode. In-process sharding validates stable
routing, parallel fan-out, global scoring, and top-K merging. Process-boundary search RPC, ordered
in-memory replication, retained-log catch-up, and health-aware routing exist, but there is no
connection manager, TLS, persistent replica log, snapshot transfer, consensus, metrics, Docker
deployment, or measured benchmark yet. Range bounds are schema-validated for keyword, int64, and timestamp fields; persisted
typed values are still scanned rather than read from columnar doc-value structures. The standard
analyzer has documented byte-oriented UTF-8 behavior and no stemming.

The planned distributed design intentionally has no consensus protocol, automatic membership or
shard rebalancing, distributed transactions, learning-to-rank, or Elasticsearch compatibility.
Results will not be added until benchmarks have actually been run.
