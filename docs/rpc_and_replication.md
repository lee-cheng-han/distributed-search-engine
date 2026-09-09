# RPC, replication, and replica routing

Shard search RPC uses a versioned length-prefixed binary protocol over connected sockets. Both peers
enforce frame, query, and hit-count limits. Every read and write is governed by one monotonic
deadline, disconnects are explicit errors, and writes suppress `SIGPIPE`. A fork-based acceptance
test executes the server and client in separate processes.

Replication records carry shard ID, epoch, operation ID, contiguous sequence, version, and document
payload. Each replica rejects the wrong shard or epoch, sequence gaps, stale unknown operations, and
operation-ID reuse with different content. Exact retries are idempotent. The primary retains a
bounded replay log; a recovering replica replays it in order before returning to healthy or suspect
service.

Acknowledgement modes are deliberately explicit: `primary_only` returns after the primary applies;
`all_replicas` requires every configured replica. An all-replica failure may still mean the mutation
is committed on the primary, and the error says so. This is ordered primary replication, not a
consensus protocol.

Read routing excludes unhealthy and draining replicas and replicas beyond the permitted lag. It
prefers healthy over suspect, then lower lag, lower in-flight work, and finally node ID for stable
ties.

`TcpShardServer` owns a listener, bounded admission queue, fixed worker pool, and per-request deadline;
`TcpShardClient` performs deadline-aware address resolution/connect and one search. The standalone
server and client expose this path to containers and separate hosts. Connections currently carry one
request and excess admitted work is rejected by closing the connection. TLS, authentication,
multiplexing, and cancellation propagation remain future work.

`PersistentMutationLog` stores the full ordered mutation in checksummed fsynced records and repairs
only torn tails. A restarted replica can replay the log, or restore a verified compressed segment
snapshot whose segment ID is its applied sequence before accepting subsequent catch-up records.
Atomic log-prefix truncation, elections, and quorum consensus are not implemented.
