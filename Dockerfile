FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ ninja-build && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDSE_BUILD_TESTS=OFF && cmake --build /build

FROM debian:bookworm-slim
RUN useradd --system --uid 10001 --create-home dse
RUN mkdir /data && chown dse:dse /data
COPY --from=build /build/dse_index_cli /usr/local/bin/dse_index_cli
COPY --from=build /build/dse_shard_server /usr/local/bin/dse_shard_server
COPY --from=build /build/dse_rpc_client /usr/local/bin/dse_rpc_client
COPY --from=build /src/datasets/synthetic/sample.tsv /usr/local/share/dse/sample.tsv
USER dse
EXPOSE 9090
ENTRYPOINT ["dse_shard_server"]
