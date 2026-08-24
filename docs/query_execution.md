# Query execution

The in-memory executor evaluates an immutable typed query plan over sorted field-specific posting
lists. It returns explicit execution errors and never converts postings to hash sets. See
[query planning](query_planning.md) for the validation and normalization performed before execution.

Term and phrase text is already analyzed in the plan with the same schema-owned analyzer used for
indexing. Execution does not analyze it again. Expanded unfielded terms sum field-specific BM25
contributions when a document matches more than one field or term. Fielded expressions restrict all
descendant term, phrase, and Boolean nodes to that field. Schema field boosts and query boosts are
folded into planned scoring clauses.

Boolean operations use two-way merge algorithms over document-ID-sorted candidate lists:

- `AND` intersects lists and sums the matching child scores;
- `OR` unions lists and sums scores for documents present on both sides;
- `NOT` subtracts its operand from the live-document snapshot and contributes zero score.

This defines standalone `NOT x` as “all live documents except x.” Deleted documents are absent from
the universe and cannot reappear through negation or match-all.

Phrase execution starts with the rarest planned term posting list, intersects the remaining postings
within one field, and then checks positions. A match requires every document position difference to
equal the corresponding planned token position difference. Consequently, stop-word position gaps are
preserved: removing a stop word does not make the surrounding terms adjacent.

Range filters compare indexed field values first and stored metadata second. Bounds are inclusive;
`*` means unbounded. The schema rejects text ranges and validates integer or `YYYY-MM-DD` timestamp
bounds. The current executor still scans and compares canonical stored encodings; columnar typed doc
values arrive with persistent segments.

After evaluation, a bounded min-heap collects top K in `O(M log K)` time and `O(K)` additional space
for M candidates. `total_hits` is counted before truncation. Results sort by score descending, then
external document ID ascending for exact score ties.

The optimized posting-list executor is differentially tested against a deliberately simple
document-at-a-time evaluator that reanalyzes source fields without reading postings. The oracle
independently computes BM25 statistics and compares match sets, `total_hits`, top-K ordering, and
scores with an absolute tolerance of `1e-12`. Fixed generator seeds make failures reproducible.

The executor currently operates on an immutable-by-convention in-memory view. Deadline checks,
snapshot ownership, concurrent publication, and distributed global statistics arrive in later
changes.
