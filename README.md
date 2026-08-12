# Distributed Search Engine

An incremental C++23 implementation of a distributed lexical search engine built from first
principles. The repository currently contains the tested foundation: configurable text
analysis, strongly typed identifiers, versioned documents, field-specific inverted indexes,
positional postings, document lengths, updates, tombstone deletes, BM25 ranking primitives, bounded
top-K collection, a typed query parser, and Boolean, positional phrase, field, range, boost, and
match-all execution.

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

## Implemented invariants

- every posting references a live document;
- posting lists are sorted by strongly typed document ID;
- term frequency equals the number of positions;
- positions are strictly increasing;
- document frequency equals posting-list size;
- field length equals analyzed token count;
- stale document versions cannot overwrite newer state;
- deleted documents have no searchable postings.

The next planned commit adds a local indexing and search CLI over the tested in-memory engine.
