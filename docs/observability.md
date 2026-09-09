# Observability and overload control

`MetricsRegistry` provides thread-safe saturating counters, signed gauges, cumulative histograms,
snapshots, and deterministic Prometheus text export. The TCP shard service reports accepted/rejected
connections, queued and active requests, completed/failed requests, and request duration. Its fixed
worker pool and bounded connection queue are the overload boundary.

Metric names are validated and values are bounded to finite observations. The library does not start
an unauthenticated metrics HTTP listener; an embedding service should expose `prometheus()` through
its existing administrative endpoint. Distributed trace propagation and a production log sink remain
outside the current internal binary protocol.
