# Distributed Search Engine

An incremental C++23 implementation of a distributed lexical search engine built from first
principles. The repository currently contains the tested foundation: configurable text
analysis, strongly typed identifiers, versioned documents, field-specific inverted indexes,
positional postings, document lengths, updates, tombstone deletes, BM25 ranking primitives, bounded
top-K collection, a typed query parser, and Boolean, positional phrase, field, range, boost, and
match-all execution. A local CLI provides a complete ingestion-to-ranked-results path.
Fields are schema-validated with owned per-field analyzers, exact keyword tags, and typed ISO dates.
Posting lists and execution use compact segment-local `uint32_t` document IDs while APIs preserve
external string IDs and deterministic external-ID tie-breaking.
Document mutations are prepared and analyzed before publication, remove old postings through targeted
term references, and maintain field statistics incrementally.

This is not a wrapper around an existing search engine. See [current limitations](docs/limitations.md)
for an honest implementation boundary. The [implementation roadmap](docs/roadmap.md) defines the
ordered commit plan, architectural decisions, outcome checks, and acceptance gates.
Its final completion bar requires differential correctness, crash and fault matrices, public relevance
evaluation, reproducible raw benchmark data, a standout measured systems result, and a five-minute
failure-aware demonstration.

## Build and test

Requirements are CMake 3.25+, a C++23 compiler, and GoogleTest. On systems where GoogleTest is not
installed as a CMake package, the build can use `/usr/src/googletest`.

```bash
./scripts/build.sh
./scripts/test.sh
./scripts/sanitize.sh
```

## Local demo

Index the deterministic sample corpus and run a query:

```bash
./scripts/index_sample.sh 'title:"distributed systems" OR body:replication'
```

The command prints one JSON object containing the number of live indexed documents, total matches,
scores, document IDs, and stored fields. To use another escaped TSV corpus directly:

```bash
./build/dse_index_cli --documents path/to/documents.tsv --query 'search AND systems' --top-k 20
```

See [datasets/README.md](datasets/README.md) for the input schema and
[query_language.md](docs/query_language.md) for syntax.

## Implemented invariants

- every posting references a live document;
- posting lists are sorted by strongly typed document ID;
- term frequency equals the number of positions;
- positions are strictly increasing;
- document frequency equals posting-list size;
- field length equals analyzed token count;
- stale document versions cannot overwrite newer state;
- deleted documents have no searchable postings.

Before immutable segments lock in the storage model, the next planned changes add typed query
planning and reference-model differential tests. Persistent segments follow
those prerequisites.
