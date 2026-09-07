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

The current RPC API serves one request on an already connected socket. Connection management, TLS,
authentication, multiplexing, cancellation propagation, persistent mutation-log storage, elections,
quorum consensus, and snapshot transfer remain future work.
