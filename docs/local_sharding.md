# Local shard execution

`StableShardRouter` uses a specified FNV-1a mapping from external document ID to a fixed shard count.
Updates and tombstones therefore remain on the same shard. `LocalShardCluster` owns disjoint shard
indexes plus an immutable-at-query-time global statistics view.

Queries fan out concurrently to every shard. Each shard evaluates local postings and documents but
uses global document frequency, field document counts, and average lengths for BM25. Every shard
returns at most the requested K; the coordinator sums exact hit counts and applies the normal global
score/ID top-K ordering. Differential tests require scores and results to match one combined-index
oracle to `1e-12`.

This is an in-process qualification layer, not network distribution. It has no RPC protocol,
deadline, cancellation, shard-map epoch, replication, membership, or failure policy yet.
