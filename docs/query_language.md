# Query language

## Resource limits

Parsing is bounded before AST construction. Defaults permit at most 16 KiB of query text, 2,048
lexemes, 4 KiB per lexeme, and 128 levels of parenthesis nesting. Applications can supply stricter
`QueryLimits`; every required limit must be nonzero. Violations return a structured
`ParseErrorCode::resource_limit` rather than attempting unbounded allocation or work. Planning adds
separate bounds for AST nodes, analyzed tokens, phrase tokens, and plan depth.

The query language is tokenized and parsed with a bounded recursive-descent parser. It is not parsed
with regular expressions. Positions in parse errors are zero-based UTF-8 byte offsets.

## Grammar

```text
query       = or_expression EOF ;
or          = and, { ("OR" | implicit_or), and } ;
and         = unary, { "AND", unary } ;
unary       = [ "NOT" ], unary | postfix ;
postfix     = primary, { "^", positive_number } ;
primary     = term
            | quoted_phrase
            | "*"
            | "(", or, ")"
            | field, ":", unary
            | field, ":", "[", bound, "TO", bound, "]" ;
bound       = term | "*" ;
```

Keywords are ASCII case-insensitive. `NOT` binds most tightly, then `AND`, then `OR`. Adjacent
expressions use implicit `OR`, so `distributed systems` means `distributed OR systems`.
Parentheses override precedence.

Supported examples:

```text
distributed
distributed systems
distributed AND search
gpu OR cpu
gpu AND NOT cpu
(gpu OR cpu) AND fast
"dynamic batching"
title:"distributed systems"
tag:systems
title:(search OR engine)
title:search^2
year:[2024 TO 2026]
year:[2024 TO *]
*
```

Quoted phrases preserve their contents for the configured field analyzer during execution. Within a
phrase, `\"` represents a quote and `\\` a backslash; other escape sequences are rejected. Empty or
whitespace-only phrases are invalid.

Ranges are inclusive in this version. `*` is an unbounded endpoint. Numeric/date interpretation is
the executor's field-schema responsibility; the parser retains bounds as strings without guessing a
type. The current executor therefore compares consistently sortable string encodings
lexicographically.

Boosts must be finite numbers greater than zero. A boost following a fielded expression applies to
the completed field query. All documented constructs are supported by the in-memory executor.

To keep hostile input bounded, parentheses may nest at most 128 levels. Errors report an error code,
the offending byte position, and a stable human-readable explanation.
