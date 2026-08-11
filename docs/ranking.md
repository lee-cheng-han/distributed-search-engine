# Ranking

The local engine uses Robertson/Sparck Jones BM25 independently per field. For one query term in one
field:

```text
idf = log(1 + (N - df + 0.5) / (df + 0.5))

term_score = idf *
             (tf * (k1 + 1)) /
             (tf + k1 * (1 - b + b * document_length / average_field_length))
```

`N`, `df`, document length, and average length are field-specific. A document contributes to `N` for
a field when that field is present, including an explicitly present empty value. Deleted documents do
not contribute. The implementation uses `log1p` for the IDF calculation and defaults to `k1 = 1.2`
and `b = 0.75`. Configuration requires finite `k1 > 0` and `b` in `[0, 1]`.

The score for a field is the sum of its query-term scores. Multi-field queries sum
`field_boost * field_score`; a query boost multiplies the completed child contribution. Boosts must
be finite and non-negative. This is an explicit sum of field-specific BM25 scores, not a claim of
full BM25F.

Absent terms score zero. Inconsistent statistics return an explicit ranking error rather than a
NaN, infinity, or silently repaired value.

Top-K collection uses a min-heap bounded to K entries, giving `O(M log K)` processing for M scored
candidates and `O(K)` storage. Results are deterministic: higher scores sort first, and exactly equal
scores sort by ascending external document ID. Distributed execution must use the same field-level
statistics and ordering so its scores remain comparable with the single-node reference.
