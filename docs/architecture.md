# Architecture

The current implementation is the Phase 0/1 single-process correctness core. Documents enter an
`InMemoryIndex`, which analyzes each field and builds independent field/term dictionaries. Posting
lists contain document IDs, term frequency, and positions. Document records retain fields,
metadata, versions, deletion state, and analyzed field lengths.

Updates require a strictly newer version and replace all old postings. Deletes are versioned
tombstones: the record remains, but all searchable postings are removed. Ordered maps and an
explicit posting sort make output deterministic. The index currently has single-writer semantics;
immutable snapshots and concurrent publication arrive with persistent segments.

The intended dependency direction is analysis and data-model primitives into indexing, followed
by query/ranking, storage, cluster, replication, transport, and telemetry. No distributed or
persistent behavior is claimed yet.
