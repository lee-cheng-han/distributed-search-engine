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

`IndexWriter` maintains a bounded mutable index and publishes a generation at its threshold or on
`refresh()`. Restart reconstructs version guards from published segments. `GenerationView` resolves
duplicate external IDs by highest version and applies tombstones across segments, while
`merge_all()` rewrites the logical result into one segment.

Generation resolution currently rebuilds postings in memory, and flush/merge work runs on the caller
thread. Orphan-manifest and obsolete-segment reclamation waits for a reader-aware deletion policy.
