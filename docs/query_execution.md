# Query execution

The in-memory executor evaluates the typed query AST over sorted field-specific posting lists. It
returns explicit execution errors and never converts postings to hash sets.

Term queries are analyzed with the same analyzer used for indexing. Unfielded terms search the
configured default fields and sum field-specific BM25 contributions when a document matches more than
one field or term. Fielded expressions restrict all descendant term, phrase, and Boolean nodes to that
field. Field boosts come from `SearchOptions`; AST boosts multiply the completed child score.

Boolean operations use two-way merge algorithms over document-ID-sorted candidate lists:

- `AND` intersects lists and sums the matching child scores;
- `OR` unions lists and sums scores for documents present on both sides;
- `NOT` subtracts its operand from the live-document snapshot and contributes zero score.

This defines standalone `NOT x` as “all live documents except x.” Deleted documents are absent from
the universe and cannot reappear through negation or match-all.

Phrase execution analyzes the phrase, intersects its term postings within one field, and then checks
positions. A match requires every document position difference to equal the corresponding analyzed
query position difference. Consequently, stop-word position gaps are preserved: removing a stop word
does not make the surrounding terms adjacent.

Range filters compare indexed field values first and stored metadata second. Bounds are inclusive and
compared lexicographically in this version; `*` means unbounded. Numeric and date-aware schemas are
future work, so callers must use consistently sortable encodings such as fixed-width ISO dates.

After evaluation, a bounded min-heap collects top K in `O(M log K)` time and `O(K)` additional space
for M candidates. `total_hits` is counted before truncation. Results sort by score descending, then
external document ID ascending for exact score ties.

The executor currently operates on an immutable-by-convention in-memory view. Deadline checks,
snapshot ownership, concurrent publication, and distributed global statistics arrive in later
changes.
