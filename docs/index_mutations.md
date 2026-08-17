# Transactional index mutations

PUT, UPDATE, and DELETE use prepare-then-publish semantics. The writer validates the schema and
version, selects or allocates the internal document ID without advancing visible allocation state,
analyzes every indexed field into temporary positional postings, records each document's unique
field/term references, and computes field-length changes before touching visible structures.

Expected validation, allocation-limit, length, or analyzer failures return an explicit `IndexError`.
The previous document version, postings, ID mappings, and statistics remain unchanged. Analyzer
exceptions are translated to `analysis_failed` at the indexing boundary.

Publication currently stages copies of the mutable dictionaries, document maps, ID maps, and field
statistics. It removes the old document only through that record's field/term references, installs the
prepared postings, adjusts statistics, and swaps the complete staged state into visibility. This is a
correctness-first implementation with a strong publication boundary; immutable segment builders will
replace the whole-state copy before performance claims are made.

Updates and tombstones retain the original internal ID. New IDs advance only after publication.
Deletes remove posting and field-statistic contributions but preserve the versioned tombstone and ID
mapping.

Field statistics are maintained incrementally as `(live document count, total analyzed length)` per
indexed field. Average length is derived in constant time. Invariant validation independently
re-analyzes live documents and recomputes statistics, verifies sorted unique per-document term
references, and ensures every reference resolves to a posting for the same internal document.
