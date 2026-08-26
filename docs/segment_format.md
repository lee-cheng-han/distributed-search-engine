# Immutable segment format v1

Version 1.0 is a single packed little-endian `.dseg` file. It is deliberately uncompressed and read
with buffered I/O so the reference format can be validated before compression or memory mapping is
introduced.

## Layout

The 64-byte header contains the eight-byte `DSESEG01` magic, major/minor version, header size,
recorded file size, nonzero segment ID, section count, feature flags, and CRC32C. Six 32-byte directory
entries follow in fixed order: schema, documents, field statistics, terms, postings, and positions.
Each entry contains its type, zero reserved word, absolute offset, byte length, and record count.
Sections must be contiguous, ordered, non-overlapping, and consume the file exactly.

Strings are `uint64` byte length followed by uninterpreted bytes. Counts and offsets are fixed-width.
Terms refer to contiguous posting ranges; postings refer to contiguous position ranges. The checksum
covers the directory and every section. Schema records include field type/policy/boost, a deterministic
analyzer descriptor, and a schema fingerprint.

## Compatibility and validation

Readers accept major version 1 and minor versions no newer than their implementation when all feature
flags are known. Unknown major versions or analyzer identities are unsupported. Truncation, trailing
bytes, invalid counts, offset arithmetic, duplicate identifiers or keys, unsorted postings/positions,
checksum mismatch, invalid schema, and term-frequency/position disagreement are corruption.

Reader limits bound file, field, document, term, posting, position, and individual-string sizes before
allocation. A reader exposes no partial index after any failure.

## Publication boundary

The writer serializes a complete snapshot to `TARGET.tmp`, flushes, closes, and fsyncs it, validates
the temporary file through the production reader, renames it to `TARGET`, and fsyncs the containing
directory. Checksummed manifests and `CURRENT` publication are documented in
[index generations](index_generations.md). Multi-segment query snapshots remain later work.
