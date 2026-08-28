# Atomic index generations

`ManifestStore` makes one manifest authoritative through a small checksummed `CURRENT` pointer.
Each binary `MANIFEST-N` records a positive, monotonically increasing generation and a unique list of
segment IDs and relative filenames. Publication validates every referenced segment and its embedded
ID before metadata changes.

Publication writes and fsyncs `MANIFEST-N.tmp`, renames it to `MANIFEST-N`, fsyncs the directory,
then repeats that sequence for `CURRENT.tmp` and `CURRENT`. Failure before the final pointer rename
leaves the preceding generation authoritative. A deterministic fault point after manifest publication
verifies this behavior.

Opening validates the pointer, manifest checksum, metadata rules, every segment checksum, and
segment-ID agreement before returning shared segment readers. Retained `OpenGeneration` objects keep
their readers alive across later publication.

`IndexWriter` rotates a bounded active index into a bounded frozen queue and publishes from one
background worker. Producers receive backpressure when that queue is full; `refresh()` freezes the
active state and waits until publication is durable. Restart reconstructs version guards from published segments. `GenerationView` resolves
duplicate external IDs by highest version and applies tombstones across segments, while
`merge_all()` rewrites the logical result into one segment.

Generation resolution and compaction currently rebuild postings in memory. Automatic compaction is
triggered by segment count rather than measured size tiers. Because readers fully buffer and own
segment state, superseded files can be removed after replacement publication without invalidating
retained readers. Orphan temporary-file cleanup and streaming merges remain future work.
