# Shard TCP service

`dse_shard_server` opens a previously published immutable segment and serves the versioned,
length-prefixed search protocol over TCP. It binds to `0.0.0.0:9090` by default and supports
`--bind` and `--port`. Each request has a deadline and byte/hit limits. The listener feeds a bounded
connection queue drained by a fixed worker pool; excess connections are closed rather than allowing
unbounded memory or thread growth. SIGINT and SIGTERM stop acceptance and join workers cleanly.

Build an image with `docker build -t dse .`, mount a segment read-only, and run:

```sh
docker run --rm -p 9090:9090 -v /absolute/index.dseg:/data/index.dseg:ro \
  dse --segment /data/index.dseg
```

The wire protocol is intentionally internal and currently has no TLS or authentication. Put the
service behind an authenticated TLS proxy on untrusted networks. Each TCP connection currently
carries one request; persistent connection pooling and topology discovery remain future work.

`docker compose up --build` deterministically builds the sample segment, starts two independently
health-checked shard processes, and publishes them on host ports 9090 and 9091. `dse_rpc_client`
provides the same health/smoke path outside containers. These two nodes demonstrate deployment and
replica endpoints; the current Compose file does not claim automatic partitioning or discovery.
