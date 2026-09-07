#pragma once
#include "dse/index/in_memory_index.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace dse::distributed {
enum class NodeHealth { healthy, suspect, unhealthy, draining };
struct ReplicaStatus { NodeId node_id; NodeHealth health{NodeHealth::healthy}; std::uint64_t applied_sequence{}; std::uint64_t lag{}; std::size_t inflight{}; };
enum class ReplicationErrorCode { invalid_topology, wrong_shard, wrong_epoch, sequence_gap, stale_sequence, conflicting_operation, mutation_error, log_full, insufficient_acknowledgements, no_eligible_replica };
struct ReplicationError { ReplicationErrorCode code; std::string message; };
struct MutationRecord { ShardId shard; std::uint64_t epoch{}; std::string operation_id; std::uint64_t sequence{}; Document document; };
enum class ApplyOutcome { applied, duplicate };
class OrderedReplica {
 public:
  OrderedReplica(NodeId node,ShardId shard,std::uint64_t epoch,index::IndexSchema schema=index::IndexSchema::default_schema()):node_(std::move(node)),shard_(shard),epoch_(epoch),index_(std::move(schema)){}
  [[nodiscard]] std::expected<ApplyOutcome,ReplicationError> apply(const MutationRecord& record);
  [[nodiscard]] ReplicaStatus status(std::uint64_t primary_sequence=0)const;
  void set_health(NodeHealth health)noexcept{health_=health;}
  void set_inflight(std::size_t inflight)noexcept{inflight_=inflight;}
  [[nodiscard]] const index::InMemoryIndex& index()const noexcept{return index_;}
  [[nodiscard]] const NodeId& node_id()const noexcept{return node_;}
 private:NodeId node_;ShardId shard_;std::uint64_t epoch_;index::InMemoryIndex index_;std::uint64_t applied_sequence_{};NodeHealth health_{NodeHealth::healthy};std::size_t inflight_{};std::map<std::string,MutationRecord,std::less<>> operations_;
};
class ReplicaRouter { public:[[nodiscard]] static std::expected<NodeId,ReplicationError> select(const std::vector<ReplicaStatus>& replicas,std::uint64_t maximum_lag); };
enum class AcknowledgementMode { primary_only, all_replicas };
struct MutationReceipt { std::uint64_t sequence{};std::size_t acknowledgements{}; };
class ReplicationGroup {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<ReplicationGroup>,ReplicationError> create(ShardId shard,std::uint64_t epoch,std::vector<NodeId> nodes,std::size_t maximum_retained_records=100'000,index::IndexSchema schema=index::IndexSchema::default_schema());
  [[nodiscard]] std::expected<MutationReceipt,ReplicationError> submit(std::string operation_id,Document document,AcknowledgementMode mode);
  [[nodiscard]] std::expected<NodeId,ReplicationError> select_read_replica(std::uint64_t maximum_lag)const;
  [[nodiscard]] std::expected<void,ReplicationError> set_health(const NodeId& node,NodeHealth health);
  [[nodiscard]] std::vector<ReplicaStatus> statuses()const;
  [[nodiscard]] std::vector<MutationRecord> retained_log()const;
 private:
  ReplicationGroup(ShardId shard,std::uint64_t epoch,std::vector<NodeId> nodes,std::size_t limit,index::IndexSchema schema);
  ShardId shard_;std::uint64_t epoch_;std::size_t limit_;std::uint64_t next_sequence_{1};std::vector<std::unique_ptr<OrderedReplica>> replicas_;std::vector<MutationRecord> log_;std::map<std::string,std::size_t,std::less<>> operation_indices_;mutable std::mutex mutex_;
};
}  // namespace dse::distributed
