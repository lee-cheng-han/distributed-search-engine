# Document identity

The engine distinguishes client-facing and posting-list identity:

- `DocumentId` is the external string supplied by clients and returned in results;
- `InternalDocumentId` is a nonzero segment-local `uint32_t` stored in postings and execution
  intermediates.

An index maintains a checked bidirectional mapping. New external IDs receive monotonically increasing
internal IDs beginning at one. Updates and tombstones retain the original internal ID, so every
version of one external document has one identity within a segment builder. Internal ID zero is
reserved as invalid. Exhaustion returns `internal_id_exhausted` without changing visible state.

Posting lists sort by internal ID. This makes document deltas compact and removes repeated external
strings from postings. Search execution also uses internal IDs for intersections, unions, exclusions,
phrase verification, and scoring. External IDs are resolved only when collecting public results;
equal scores still use external ID ascending as the deterministic tie-break.

Current allocation is deterministic for a given ingestion order. Persistent segment finalization
will define whether byte-identical output across different ingestion orders is required; visible
documents and search results must be equivalent regardless of insertion order. Every reader validates
that both mapping directions agree and every posting resolves to a live document record.
