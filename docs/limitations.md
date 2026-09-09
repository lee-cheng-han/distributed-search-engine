# Current limitations

This repository implements an in-memory lexical search engine, a local CLI, and one immutable
checksummed segment that can be reopened and searched after restart. Durable writers use delta and
variable-byte compressed postings while retaining support for the fixed-width v1.0 format. Checksummed manifests and an
atomic `CURRENT` pointer publish durable generations. Bounded mutable writes can flush into multiple
segments, reopen with stale-version protection, resolve versions/tombstones, and compact to one
segment. Flushes run on one bounded background publisher with explicit refresh semantics; automatic
and explicit compaction serialize against publication and reclaim superseded files after buffered
readers own their data. Resolved query views and compaction are still materialized in memory. There
is a checksummed fsynced mutation log, but no group-commit mode. In-process sharding validates stable
routing, parallel fan-out, global scoring, and top-K merging. Process-boundary search RPC has a
bounded TCP worker service, container image, and two-node Compose example. Ordered in-memory
replication, retained-log catch-up, health-aware routing, and fsynced replica-log restart recovery
exist, but there is no TLS/authentication, snapshot transfer, consensus, automatic membership,
distributed coordinator service, or distributed tracing. Prometheus-format in-process metrics exist,
but no metrics HTTP endpoint or dashboard is bundled. Range bounds are schema-validated for keyword,
int64, and timestamp fields; persisted
typed values are still scanned rather than read from columnar doc-value structures. The standard
analyzer has documented byte-oriented UTF-8 behavior and no stemming.

The planned distributed design intentionally has no consensus protocol, automatic membership or
shard rebalancing, distributed transactions, learning-to-rank, or Elasticsearch compatibility.
Checked-in synthetic and Cranfield results are single-host baselines. Multi-client scaling,
failure-recovery timing, confidence intervals, external-engine comparison, and adaptive-routing
experiments have not yet been run.
