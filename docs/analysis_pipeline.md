# Analysis pipeline

Analyzers are selected per field by the index schema and owned immutably by that schema. The
`StandardAnalyzer` scans validated `std::string_view` input without copying it first. ASCII
letters and digits and every non-ASCII byte form tokens; ASCII punctuation and whitespace are
boundaries. ASCII letters are lowercased. Non-ASCII UTF-8 bytes are preserved exactly, so this is
UTF-8 safe with respect to memory and token integrity, but it is deliberately not Unicode-aware:
it performs no normalization, case folding, grapheme segmentation, or malformed-UTF-8 rejection.
Offsets are zero-based byte offsets into the original value, not Unicode code-point offsets.

Stop-word matching occurs after ASCII lowercasing. Removed tokens still consume a position. This
preserves position gaps and prevents a phrase query from treating words separated by a removed
stop word as adjacent. Stemming is not implemented in Phase 1.

The `KeywordAnalyzer` emits a non-empty value as one verbatim token at position zero. The default
schema uses it for exact-match `tags`; the entire current field value is one tag. Empty values produce
no tokens.

Analyzer inputs larger than `uint32_t` offsets can represent fail with `std::length_error`; index
callers must translate that error at a service boundary in later phases.
