# Field schema

Every document field is declared before indexing. A definition contains its name, type, indexed and
stored policy, default ranking boost, and—only for searchable text-like fields—an owned immutable
analyzer. The schema owns analyzers with `shared_ptr<const Analyzer>`; indexes and query executors do
not borrow analyzers from caller stack frames.

Supported types are:

- `text`: analyzed into positional postings;
- `keyword`: the complete value is one exact positional token;
- `int64`: validated signed 64-bit value for typed filtering and future doc values;
- `timestamp`: validated calendar date in fixed-width `YYYY-MM-DD` format.

The default schema is:

| Field | Type | Indexed | Stored | Analyzer |
| --- | --- | --- | --- | --- |
| `title` | text | yes | yes | standard |
| `body` | text | yes | yes | standard |
| `tags` | keyword | yes | yes | keyword |
| `timestamp` | timestamp | no | yes | none |

`tags` currently represents one exact keyword value. Multiple independent tags require a future
multi-value document representation; whitespace is not silently interpreted as a tag delimiter.

Unknown document and query fields fail explicitly. Values placed in indexed fields must have
`indexed=true`; stored metadata requires `stored=true`. Text range filters are rejected. Typed range
bounds are validated against their field type before document comparisons occur.

Schema definitions reject empty or duplicate names, invalid boosts, missing analyzers on indexed text
and keyword fields, and analyzers attached to typed or unindexed fields. Schema fingerprints and typed
columnar doc values will be added with persistent segments.
