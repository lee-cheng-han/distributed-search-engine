#pragma once
#include "dse/index/in_memory_index.hpp"
#include "dse/query/executor.hpp"
#include <cstddef>
#include <expected>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>
namespace dse::distributed {
enum class ClusterErrorCode { invalid_shard_count, mutation_error, execution_error, result_overflow };
struct ClusterError { ClusterErrorCode code; std::string message; };
class StableShardRouter {
 public:
  explicit StableShardRouter(std::size_t shard_count):shard_count_(shard_count){}
  [[nodiscard]] std::size_t route(const DocumentId& id)const noexcept;
  [[nodiscard]] std::size_t shard_count()const noexcept{return shard_count_;}
 private:std::size_t shard_count_;
};
class LocalShardCluster {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<LocalShardCluster>,ClusterError> create(std::size_t shard_count,index::IndexSchema schema=index::IndexSchema::default_schema());
  [[nodiscard]] std::expected<void,ClusterError> put(Document document);
  [[nodiscard]] std::expected<void,ClusterError> erase(const DocumentId& id,std::uint64_t version);
  [[nodiscard]] std::expected<query::SearchResult,ClusterError> search(const query::QueryNode& query,const query::SearchOptions& options={})const;
  [[nodiscard]] std::size_t route(const DocumentId& id)const noexcept{return router_.route(id);}
  [[nodiscard]] const index::InMemoryIndex& oracle_index()const noexcept{return global_;}
 private:
  LocalShardCluster(std::size_t count,index::IndexSchema schema);
  StableShardRouter router_;index::InMemoryIndex global_;std::vector<std::unique_ptr<index::InMemoryIndex>> shards_;mutable std::shared_mutex mutex_;
};
}  // namespace dse::distributed
