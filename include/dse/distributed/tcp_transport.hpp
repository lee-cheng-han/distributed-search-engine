#pragma once

#include "dse/distributed/shard_rpc.hpp"
#include "dse/telemetry/metrics.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dse::distributed {

struct TcpEndpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{};
};

struct TcpServerOptions {
  std::size_t worker_count{4};
  std::size_t maximum_queued_connections{128};
  std::chrono::milliseconds request_timeout{5'000};
  RpcLimits rpc_limits{};
  std::shared_ptr<telemetry::MetricsRegistry> metrics;
};

class TcpShardClient {
 public:
  explicit TcpShardClient(TcpEndpoint endpoint, RpcLimits limits = {})
      : endpoint_(std::move(endpoint)), limits_(limits) {}
  [[nodiscard]] std::expected<query::SearchResult, RpcError> search(
      std::string_view query, std::size_t top_k,
      std::chrono::steady_clock::time_point deadline) const;

 private:
  TcpEndpoint endpoint_;
  RpcLimits limits_;
};

class TcpShardServer {
 public:
  ~TcpShardServer();
  TcpShardServer(const TcpShardServer&) = delete;
  TcpShardServer& operator=(const TcpShardServer&) = delete;

  [[nodiscard]] static std::expected<std::unique_ptr<TcpShardServer>, RpcError> start(
      TcpEndpoint endpoint, std::shared_ptr<const index::SearchIndexView> index,
      TcpServerOptions options = {});
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  void stop() noexcept;

 private:
  TcpShardServer(int listener, std::uint16_t port,
                 std::shared_ptr<const index::SearchIndexView> index,
                 TcpServerOptions options);
  void accept_loop();
  void worker_loop();

  int listener_;
  std::uint16_t port_;
  std::shared_ptr<const index::SearchIndexView> index_;
  TcpServerOptions options_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<int> connections_;
  bool stopping_{};
  std::thread acceptor_;
  std::vector<std::thread> workers_;
};

}  // namespace dse::distributed
