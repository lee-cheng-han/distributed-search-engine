#pragma once
#include "dse/index/search_index_view.hpp"
#include "dse/query/executor.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
namespace dse::distributed {
enum class RpcErrorCode { io_error, timeout, disconnected, protocol_error, resource_limit, remote_error };
struct RpcError { RpcErrorCode code; std::string message; };
struct RpcLimits { std::uint32_t maximum_frame_bytes{16U<<20U}; std::uint32_t maximum_query_bytes{1U<<20U}; std::uint32_t maximum_hits{100'000}; };
class ShardRpcClient {
 public:
  explicit ShardRpcClient(int socket,RpcLimits limits={}):socket_(socket),limits_(limits){}
  [[nodiscard]] std::expected<query::SearchResult,RpcError> search(std::string_view query,std::size_t top_k,std::chrono::steady_clock::time_point deadline)const;
 private:int socket_;RpcLimits limits_;
};
class ShardRpcServer {
 public:
  [[nodiscard]] static std::expected<void,RpcError> serve_one(int socket,const index::SearchIndexView& index,std::chrono::steady_clock::time_point deadline,RpcLimits limits={});
};
}  // namespace dse::distributed
