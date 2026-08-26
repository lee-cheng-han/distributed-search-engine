# Current architecture

## Indexing and documents

The engine is a C++23, single-process lexical-search core. `InMemoryIndex` owns a declared field
schema, per-field analyzers, document records, sorted term dictionaries, positional postings, and
field-level BM25 statistics. Client-facing string IDs are mapped explicitly to compact `uint32_t`
internal IDs used by postings. Higher document versions replace lower versions; deletes are
versioned tombstones.

Mutations use prepare-then-publish behavior. Schema validation and analysis finish before copies of
the visible dictionaries, documents, mappings, and statistics are changed and swapped into place.
Per-document term references remove old postings without scanning the full dictionary. This is
correctness-first and currently has single-writer semantics.

## Query execution and ranking

The lexer and recursive-descent parser produce a syntax AST. A schema-aware planner validates fields
and typed ranges, analyzes query text once, expands default fields, removes equivalent clauses,
canonicalizes plans, and orders conjunctions by posting estimates. The executor consumes a read-only
`SearchIndexView`, performs sorted posting-list merges and positional phrase checks, scores with
field-specific BM25 statistics, and collects deterministic bounded top-K results.

Both mutable indexes and reopened immutable segment files implement the same read interface. The
reference evaluator scans and reanalyzes documents independently; seeded differential tests compare
matches, scores, hit counts, and ordering against optimized execution.

## Persistence

One immutable version-1 `.dseg` file can persist a complete index snapshot and be reopened without
reanalyzing documents or rebuilding postings. It stores schema/analyzer identity, documents,
tombstones, ID mappings, lengths and statistics, terms, postings, and positions. Readers validate
magic, version, feature flags, sizes, section layout, CRC32C, resource limits, ordering, uniqueness,
and posting invariants before exposing data.

Segment files and checksummed manifests use temporary writes, file fsync, atomic rename, and directory
fsync. A checksummed `CURRENT` pointer selects one monotonically increasing generation; retained
generation objects keep old readers alive. A bounded local writer publishes threshold or explicit
flushes, recovers version state after restart, resolves latest versions and tombstones across
segments, and supports explicit full compaction. Generation resolution currently materializes a
fresh in-memory view; background work, obsolete-file reclamation, compression, and memory mapping
remain unimplemented.

## Application, testing, and operations

`dse_index_cli` can build an in-memory index from deterministic TSV, optionally persist it, or open a
segment in a later process and execute the same query pipeline. GoogleTest covers analysis, schemas,
transactional mutations, parsing, planning, ranking, execution, differential correctness, bounded
arbitrary input, segment round trips, and corruption rejection. CI runs the normal and ASan/UBSan
suites. A libFuzzer-compatible query target exists, although the local Clang installation may require
separate compiler-runtime packages.

There is no concurrency, benchmark harness, measured performance result, network API, sharding,
replication, cluster membership, cache, metrics exporter, tracing, Docker deployment, or distributed
behavior yet.
