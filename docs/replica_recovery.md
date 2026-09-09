# Replica durability and recovery

`PersistentMutationLog` records the complete shard, epoch, operation ID, sequence, document version,
delete marker, fields, and metadata for every ordered mutation. It uses the same length-bounded,
CRC32C-protected, fsynced record container as the local write-ahead log. Recovery repairs only a torn
tail, rejects checksum damage and identity/sequence mismatches, and replays through `OrderedReplica`
so normal idempotency and schema checks remain authoritative.

An acknowledgement intended to promise replica durability must be sent only after
`PersistentMutationLog::append` succeeds. `recover_replica` reconstructs an independent searchable
replica after process restart. A replica can also publish a compressed, verified immutable snapshot
whose segment ID is the applied sequence; `OrderedReplica::restore` rebuilds the state and accepts
catch-up beginning at the next sequence. Snapshot transport, atomic log-prefix truncation, and a
consensus protocol are not implemented. A restored snapshot intentionally does not retain old
operation IDs, so retries at or before its checkpoint are rejected as stale rather than recognized
as duplicates.
