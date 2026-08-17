# Architecture

The current implementation is a single-process in-memory correctness core. Documents enter an
`InMemoryIndex`, which analyzes each field and builds independent field/term dictionaries. Posting
lists contain compact segment-local internal document IDs, term frequency, and positions. A checked
bidirectional mapping resolves external client IDs at API boundaries. Document records retain fields,
metadata, versions, deletion state, and analyzed field lengths.

Queries pass through a lexer and recursive-descent parser into a typed AST. The executor performs
merge-style Boolean operations, field-specific BM25 scoring, positional phrase checks, filters, and
bounded deterministic top-K collection.

The local `dse_index_cli` is the first complete application path. It validates and loads an escaped
TSV corpus, builds the index, validates its invariants, parses one query, executes it, and writes a
single JSON result to standard output. Input, query, and execution failures are sent to standard error
with nonzero exit codes. The index exists only for that process lifetime.

Updates require a strictly newer version and use prepare-then-publish replacement. Each document
records its field/term references, so old postings are removed without scanning the full dictionary;
field counts and total lengths are updated incrementally. Deletes are versioned tombstones: the record
remains, but all searchable postings are removed. Ordered maps and an explicit posting sort make
output deterministic. The index currently has single-writer semantics;
immutable snapshots and concurrent publication arrive with persistent segments.

The intended dependency direction is analysis and data-model primitives into indexing, followed
by query/ranking, storage, cluster, replication, transport, and telemetry. No distributed or
persistent behavior is claimed yet.
