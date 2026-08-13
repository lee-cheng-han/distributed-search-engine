# Datasets

`synthetic/sample.tsv` is a small deterministic demo corpus safe to keep in the repository. The
escaped TSV schema is:

```text
document_id  version  deleted  title  body  tags  timestamp
```

Columns are separated by literal tab bytes. Values may encode tab, newline, carriage return, and
backslash as `\t`, `\n`, `\r`, and `\\`. Other escape sequences are rejected. Versions must be
positive integers and `deleted` must be `0` or `1`. Timestamp values use ISO-8601 so the current
lexicographic range filtering remains meaningful.

This sample is for correctness demonstrations, not performance or relevance claims.
