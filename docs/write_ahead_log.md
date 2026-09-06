# Write-ahead recovery

`WRITE-AHEAD.log` records each accepted mutation before the writer returns success. Records carry a
fixed magic value, bounded payload length, CRC32C, document ID, version, delete flag, fields, and
stored metadata. Appends are flushed and file-fsynced. Startup replays records newer than the
published segments; already-published versions are idempotently skipped.

A final incomplete record is treated as an interrupted append, truncated to the last valid boundary,
and fsynced before later appends. Invalid magic, checksum
damage, malformed records, duplicate map keys, and configured file/record/count/string limit
violations reject startup. After `refresh()` has durably published every frozen mutation, the log is
truncated and fsynced as a checkpoint.

The current reference path performs one fsync per mutation. Group commit and batched durability
modes require explicit acknowledgement semantics and benchmarks before being added.
