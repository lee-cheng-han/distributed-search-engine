# Target architecture

The target remains a search system rather than a general-purpose distributed database.

```text
client -> gateway -> coordinator -> parallel shard requests -> global top-K
                              |-> shard replica -> immutable generation
                                                  |- active mutable index
                                                  |- frozen flushes
                                                  `- persistent segments
```

## Local storage engine

Documents first enter a bounded mutable index. A threshold freezes it, immediately installs a new
active index, and flushes the frozen view to an immutable segment. An atomically published manifest
names one generation of segments and delete state. Every query retains one immutable generation for
its lifetime. Background size-tiered merging rewrites live versions, removes safe tombstones, and
retires old files only after readers release them.

Persistent segments evolve through versioned codecs: the fixed-width reference representation comes
first, followed by measured delta/variable-byte posting compression and optional memory-mapped reads.
Crash safety covers temporary writes, checksums, file and directory fsync, atomic rename, startup
validation, and deterministic failure at every publication boundary.

## Distributed execution

Stable hashing routes each external document ID to exactly one shard. A coordinator parses and plans
once, captures a shard-map and global-statistics epoch, fans out concurrently with deadlines and
cancellation, and merges bounded shard-local top-K results. Immutable global field statistics make
BM25 scores comparable; seeded tests require distributed results to match a single-index oracle.

Each shard has a primary and ordered replicas. Idempotent, checksummed mutation records carry shard,
epoch, operation ID, sequence, version, and payload. Acknowledgement modes are explicit and do not
claim consensus. Lagging replicas replay retained operations or receive a verified segment snapshot.
Health-aware routing avoids suspect, unhealthy, and draining nodes.

## Operations and qualification

All queues, caches, readers, requests, and recovery tasks are bounded. Metrics cover query work,
indexing, segments, merging, caches, replication, health, overload, and latency histograms. Structured
logs and traces carry request, shard, node, generation, and mutation identities without document
content. Qualification includes deterministic corruption/crash matrices, randomized local and
distributed reference tests, fuzzing, sanitizers, sustained load, and reproducible raw benchmarks.

Hybrid vector retrieval is optional and begins only after the persistent replicated lexical engine
is stable and measured.
