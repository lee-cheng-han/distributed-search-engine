# Implementation roadmap

This roadmap turns the full project specification into an ordered series of independently useful,
buildable commits. Correctness and deterministic behavior take precedence over feature count. Every
commit must leave the repository compiling and tested; unfinished later work does not weaken an
earlier result.

## Outcome overview

| Outcome | Evidence that it works |
| --- | --- |
| Search works end to end | Documents become deterministic, correctly ranked query results |
| The index survives change and restarts | Crash-safe, incremental, compressed local search |
| Search scales across nodes without changing results | Distributed top-K matches a single-node reference |
| The cluster stays useful during failures | Replicas recover and routing avoids unhealthy nodes |
| Claims are backed by evidence | Reproducible relevance, performance, and failure results |
| Lexical search gains an optional semantic layer | Measured vector and hybrid retrieval |

### Search works end to end

Given documents and a query, the engine returns deterministic, correctly ranked results. This
includes the engineering foundation, analysis, positional indexing, query parsing, Boolean and
phrase execution, field boosts, BM25, and a usable CLI.

### The index survives change and restarts

The local engine becomes durable and operationally credible: committed data survives crashes,
updates and deletes remain correct across immutable segments, merges preserve live results, and
compression and concurrency improve performance without changing behavior.

This is the first portfolio-ready release. Networking must not delay or destabilize it.

### Search scales across nodes without changing results

Deterministic sharding, concurrent scatter/gather, global corpus statistics, compatible BM25 scores
across shards, deadlines, bounded queues, and distributed results that match a single-node reference
corpus.

The coordinator and shards are tested in-process before transport is introduced. Networking is an
adapter over the already-tested interfaces.

### The cluster stays useful during failures

Synchronous primary/replica writes, ordered replication logs, recovery, health-aware routing,
partial-result semantics, fault injection, metrics, tracing, and explicit overload behavior.

### Claims are backed by evidence

Relevance, indexing, latency, scaling, straggler, and failure claims come from reproducible
experiments with complete environment metadata. No invented or extrapolated results are published.

### Lexical search gains an optional semantic layer

Vector or hybrid retrieval begins only after the lexical and distributed engine is stable and
measured. It must remain an
extension of the search engine rather than turning the project into a generic RAG application.

## Architectural decisions to settle early

### Document identity

Use two distinct identifiers:

- `ExternalDocumentId`: stable client-facing string used for API semantics and deterministic ties;
- `InternalDocumentId`: compact segment-local unsigned integer used in posting lists.

Each segment owns an explicit mapping between them. Posting compression operates on sorted internal
IDs, while update and delete resolution uses external ID plus version.

### Errors

Fallible core operations return an explicit result, preferably `std::expected<T, Error>`. The shared
error taxonomy starts with:

```cpp
enum class ErrorCode {
  invalid_argument,
  parse_error,
  deadline_exceeded,
  overloaded,
  not_found,
  stale_version,
  corruption,
  io_error,
  unavailable,
};
```

Exceptions may enforce local programming invariants but must be translated before storage, process,
or service boundaries.

### Multi-field ranking

The local engine scores each field independently with field-specific document frequency, average length, and BM25
parameters, then sums `field_boost * field_score`. Query boosts multiply the completed child score.
This is deliberately simpler than claiming full BM25F. Distributed statistics must preserve these
same field-level meanings.

### Deterministic ordering

Identical input and configuration must produce identical tokens, segment contents, merge output, and
search order. Search hits sort by score descending and then external document ID ascending. Tests
must cover score ties explicitly.

### Snapshots and versions

An index generation is an immutable manifest plus its referenced segments and tombstone state. A
reader retains one generation snapshot for its entire request. Across segments, the highest document
version wins; at equal versions, duplicate state is corruption rather than an arbitrary choice. Old
files may be reclaimed only after publication succeeds and no reader owns the old snapshot.

### Format evolution

The first persistent format is fixed-width, length-prefixed, little-endian, checksummed, and strictly
bounds-checked. Compression is a versioned posting codec added only after this reference format is
correct. Readers reject unsupported major versions and corruption explicitly; compatibility across
minor versions must be documented and covered by golden files.

## Commit plan

Commits are ordered by dependency and sized around one coherent technical result. The text in each
code span is the intended commit-message subject. Outcome headings are organizational only and must
not appear in commit messages.

## Search works end to end

### `bootstrap C++23 search core and positional index`

Establish CMake, warnings, CI, sanitizers, strong IDs, explicit errors, configuration, analyzers,
versioned documents, field-specific positional postings, tombstones, invariant tests, and the
documented UTF-8 boundary behavior.

### `implement field-aware BM25 scoring and top-k collection`

Add configurable BM25 using field-specific statistics and a bounded min-heap. Verify hand-calculated
scores, stable floating-point behavior, score boosts, and deterministic document-ID tie-breaking.

### `parse boolean fielded and phrase queries`

Add a lexer, typed AST, recursive-descent parser, ranges, boosts, precedence rules, and structured
errors containing a code, byte position, and human-readable message. Include property and malformed
input tests.

### `execute boolean phrase and filtered searches`

Implement merge-style posting intersection, union, exclusion, positional phrase matching, filters,
scoring, and top-K collection. Compare optimized execution with a simple reference evaluator.

### `add local indexing and search CLI`

Expose document ingestion and query execution through a small CLI, include deterministic sample data,
and add an end-to-end test covering term, Boolean, phrase, field, filter, and boosted queries.

**Outcome check:** from a clean checkout, the CLI turns a deterministic corpus into correct ranked
results, and the warning-clean correctness suite passes under ASan/UBSan.

## The index survives change and restarts

### `persist immutable index segments`

Define and implement the initial fixed-width, little-endian, length-prefixed segment format with
defensive readers, checksums, format versions, golden files, and malformed-input tests.

### `publish index generations atomically`

Add manifests, temporary segment construction, fsync and rename ordering, startup verification, and
subprocess crash tests at every publication boundary. Recovery must expose the old or new generation,
never a mixture.

### `support incremental updates and tombstones across segments`

Resolve external document versions across multiple immutable segments, retain deletion semantics,
publish reader snapshots, and defer file reclamation until no old snapshot remains.

### `merge segments without changing live search results`

Merge a deterministic set of segments, discard obsolete versions and tombstones, rebuild statistics,
publish atomically, and verify live results before and after merging—including publication crashes.

### `compress posting lists with delta and variable-byte codecs`

Add versioned posting codecs, round-trip and malformed-stream properties, differential tests against
the uncompressed reader, and measurements for size and decode throughput.

### `add bounded concurrent search and generation-aware caching`

Introduce bounded worker queues, immutable search snapshots, a byte-bounded LRU cache keyed by index
generation, and concurrency tests under ASan/UBSan and TSan.

**Outcome check:** the CLI can persist, restart, update, delete, merge, compress, and concurrently
search a corpus without changing results. Crash and corruption tests pass. This is the first
portfolio-ready release.

## Search scales across nodes without changing results

### `route documents with deterministic shard assignments`

Add stable hashing, shard ownership, a `ShardClient` interface, an in-process implementation, and
tests proving that routing does not depend on process or standard-library hash behavior.

### `scatter searches with global BM25 statistics`

Maintain global field statistics, query shards concurrently with absolute deadlines and cancellation
checks, and merge global top-K results. Distributed scores and ordering must match the single-node
reference for queries, ties, updates, and deletes.

### `expose coordinator and shard HTTP APIs`

Add transport adapters over tested in-process interfaces, structured boundary errors, remaining-time
deadline propagation, bounded request queues, and explicit 429/503 overload responses.

### `deploy a reproducible multi-node search cluster`

Add container images, Docker Compose, health checks, configuration, sample indexing/search scripts,
and a clean-checkout smoke test.

**Outcome check:** the Dockerized multi-shard deployment returns the same deterministic top-K and
scores as the reference engine, fans out concurrently, and reports deadlines and overload explicitly.

## The cluster stays useful during failures

### `replicate shard writes with ordered operation logs`

Add primary/replica roles, monotonically increasing sequence numbers, synchronous
`primary_and_replica` acknowledgment, persisted replication state, and rejection of gaps or
out-of-order operations.

### `recover replicas from logs and verified snapshots`

Replay retained operations, fall back to checksummed snapshots, catch up after snapshot transfer, and
mark a replica healthy only once synchronized.

### `route reads using node health and replica load`

Add heartbeats, health-state transitions, round-robin and load-aware selection, draining, replication
lag checks, and tests ensuring unhealthy nodes receive no new work.

### `enforce deadlines backpressure and partial-result semantics`

Bound coordinator, query, indexing, and replication queues; stop expired work; distinguish strict and
partial requests; report every failed shard; and add test-only RPC, disk, latency, and saturation
faults.

### `instrument distributed search operations`

Add structured logs, bounded-cardinality Prometheus metrics, distributed traces, and dashboards that
make request fan-out, cache behavior, replication lag, timeouts, and recovery observable.

**Outcome check:** replicated shards recover from supported failures, routing avoids unavailable
nodes, missing work is explicit, and operational signals explain detection, failover, and recovery.

## Claims are backed by evidence

### `evaluate lexical ranking on labeled queries`

Implement Precision@K, Recall@K, MRR, and NDCG@K; verify the evaluators; and compare a TF-IDF baseline,
default BM25, and tuned BM25 on a deterministic labeled dataset.

### `benchmark indexing query and cache performance`

Measure throughput, latency percentiles, memory, index size, compression, and cache behavior across
document counts and client concurrency. Record commit, compiler, flags, hardware, OS, corpus,
configuration, and seed automatically.

### `measure scaling stragglers and failure recovery`

Automate shard scaling, slow-replica selection, node loss, restart, corruption, and queue saturation
experiments. Populate `results.md` only with actually measured values and explain non-linear behavior.

### `validate adaptive routing under tail-latency stress`

Create a controlled straggler workload and compare round-robin, least-queue-depth, latency-aware, and
adaptive replica selection. Define the adaptive formula before running the experiment, report
confidence intervals and repeated trials, and retain the workload and raw data. This is the preferred
standout systems result: it must either demonstrate a meaningful p99 improvement or honestly explain
why it does not.

### `publish a reproducible release demonstration`

Provide a one-command local cluster, deterministic corpus generation, indexing and query walkthrough,
failure injection, dashboard screenshots, and a concise technical deep dive. Validate the instructions
from a clean checkout and archive machine-readable benchmark output alongside the report.

**Outcome check:** every relevance, performance, scaling, straggler, and failure claim can be
regenerated from documented commands with enough metadata to interpret it.

## Lexical search gains an optional semantic layer

### `add vector and hybrid retrieval modes`

Implement embeddings and an ANN index, preferably HNSW from scratch, plus explicit rank fusion and
optional reranking. Preserve lexical behavior and measure Recall@K, NDCG@K, latency, and memory costs.

## Quality gate for every commit

A commit is ready only when:

- the project builds with no compiler warnings;
- new behavior has positive, boundary, and malformed-input coverage;
- deterministic tests pass repeatedly;
- relevant ASan/UBSan tests pass, with TSan run for concurrency changes;
- persistent changes include compatibility and corruption tests;
- documentation names new semantics, limits, and failure modes;
- performance-sensitive changes are compared with the previous implementation;
- exact commands and outcomes are recorded without claiming unrun checks.

Small benchmarks begin with the local engine to detect regressions. Headline performance, scaling,
and reliability results are published only after their corresponding behavior is stable.

## Final completion bar

The project is complete only when all of the following are true:

- a five-minute clean-checkout demo indexes data, searches it, kills a node, shows failover, and
  exposes the behavior through logs, metrics, and traces;
- single-node and distributed differential tests use the same deterministic corpora and prove equal
  results and scores, including ties, phrases, filters, updates, and deletes;
- parser and segment readers have sustained fuzz coverage, while codecs and merges have property and
  reference-model tests;
- subprocess crash tests exercise every durable-publication boundary, and a documented fault matrix
  covers disk, RPC, delay, queue, corruption, and restart behavior;
- ASan, UBSan, and the concurrency-specific TSan suite pass; warnings and static-analysis findings are
  resolved or narrowly documented;
- relevance is evaluated on at least one recognized public IR dataset in addition to deterministic
  synthetic data;
- performance reports include raw data, scripts, warmup, repetitions, confidence intervals, complete
  environment metadata, and explanations of bottlenecks and non-linear scaling;
- at least one standout technical result is supported by repeatable evidence—preferably adaptive
  replica routing that improves tail latency, rigorous crash-consistency coverage, or a clearly
  measured compression/latency tradeoff;
- format, consistency, recovery, ranking, deadline, overload, and partial-result semantics are precise
  enough for another engineer to predict behavior without reading the implementation;
- limitations remain explicit, and the README claims only behavior and measurements that the checked-in
  system can reproduce.

Optional hybrid retrieval cannot compensate for missing lexical, durability, distributed-correctness,
or reliability requirements. Depth and evidence remain more important than adding another feature.
