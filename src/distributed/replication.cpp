#include "dse/distributed/replication.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <utility>
namespace dse::distributed {namespace {
ReplicationError fail(ReplicationErrorCode code,std::string message){return {code,std::move(message)};}
bool same_record(const MutationRecord& a,const MutationRecord& b){return a.shard==b.shard&&a.epoch==b.epoch&&a.operation_id==b.operation_id&&a.sequence==b.sequence&&a.document.id==b.document.id&&a.document.fields==b.document.fields&&a.document.stored_metadata==b.document.stored_metadata&&a.document.version==b.document.version&&a.document.deleted==b.document.deleted;}
bool same_document(const Document& a,const Document& b){return a.id==b.id&&a.fields==b.fields&&a.stored_metadata==b.stored_metadata&&a.version==b.version&&a.deleted==b.deleted;}
}
std::expected<ApplyOutcome,ReplicationError> OrderedReplica::apply(const MutationRecord& record){if(record.shard!=shard_)return std::unexpected(fail(ReplicationErrorCode::wrong_shard,"mutation targets another shard"));if(record.epoch!=epoch_)return std::unexpected(fail(ReplicationErrorCode::wrong_epoch,"mutation epoch does not match replica"));if(record.operation_id.empty())return std::unexpected(fail(ReplicationErrorCode::conflicting_operation,"operation ID is empty"));if(const auto found=operations_.find(record.operation_id);found!=operations_.end()){if(!same_record(found->second,record))return std::unexpected(fail(ReplicationErrorCode::conflicting_operation,"operation ID was reused with different content"));return ApplyOutcome::duplicate;}if(record.sequence<=applied_sequence_)return std::unexpected(fail(ReplicationErrorCode::stale_sequence,"unknown operation has stale sequence"));if(record.sequence!=applied_sequence_+1U)return std::unexpected(fail(ReplicationErrorCode::sequence_gap,"mutation sequence is not contiguous"));auto applied=index_.put(record.document);if(!applied)return std::unexpected(fail(ReplicationErrorCode::mutation_error,applied.error().message));applied_sequence_=record.sequence;operations_.emplace(record.operation_id,record);return ApplyOutcome::applied;}
std::expected<std::unique_ptr<OrderedReplica>,ReplicationError> OrderedReplica::restore(NodeId node,ShardId shard,std::uint64_t epoch,const storage::SegmentReader& snapshot){if(shard.value()==0U||epoch==0U||snapshot.segment_id().value()==0U)return std::unexpected(fail(ReplicationErrorCode::invalid_topology,"invalid replica snapshot identity"));auto replica=std::make_unique<OrderedReplica>(std::move(node),shard,epoch,snapshot.schema());for(const auto& [id,record]:snapshot.records()){(void)id;auto restored=replica->index_.put(record.document);if(!restored)return std::unexpected(fail(ReplicationErrorCode::mutation_error,"snapshot restore failed: "+restored.error().message));}replica->applied_sequence_=snapshot.segment_id().value();return replica;}
ReplicaStatus OrderedReplica::status(std::uint64_t primary_sequence)const{return {.node_id=node_,.health=health_,.applied_sequence=applied_sequence_,.lag=primary_sequence>applied_sequence_?primary_sequence-applied_sequence_:0U,.inflight=inflight_};}
std::expected<NodeId,ReplicationError> ReplicaRouter::select(const std::vector<ReplicaStatus>& replicas,std::uint64_t maximum_lag){const ReplicaStatus* best=nullptr;for(const auto& candidate:replicas){if(candidate.health==NodeHealth::unhealthy||candidate.health==NodeHealth::draining||candidate.lag>maximum_lag)continue;if(best==nullptr||candidate.health<best->health||(candidate.health==best->health&&(candidate.lag<best->lag||(candidate.lag==best->lag&&(candidate.inflight<best->inflight||(candidate.inflight==best->inflight&&candidate.node_id<best->node_id))))))best=&candidate;}if(best==nullptr)return std::unexpected(fail(ReplicationErrorCode::no_eligible_replica,"no replica satisfies health and lag policy"));return best->node_id;}
ReplicationGroup::ReplicationGroup(ShardId shard,std::uint64_t epoch,std::vector<NodeId> nodes,std::size_t limit,index::IndexSchema schema):shard_(shard),epoch_(epoch),limit_(limit){replicas_.reserve(nodes.size());for(auto& node:nodes)replicas_.push_back(std::make_unique<OrderedReplica>(std::move(node),shard,epoch,schema));}
std::expected<std::unique_ptr<ReplicationGroup>,ReplicationError> ReplicationGroup::create(ShardId shard,std::uint64_t epoch,std::vector<NodeId> nodes,std::size_t maximum_retained_records,index::IndexSchema schema){std::set<NodeId> unique(nodes.begin(),nodes.end());if(shard.value()==0U||epoch==0U||nodes.empty()||nodes.size()>64U||unique.size()!=nodes.size()||maximum_retained_records==0U)return std::unexpected(fail(ReplicationErrorCode::invalid_topology,"invalid replication group"));return std::unique_ptr<ReplicationGroup>(new ReplicationGroup(shard,epoch,std::move(nodes),maximum_retained_records,std::move(schema)));}
std::expected<MutationReceipt,ReplicationError> ReplicationGroup::submit(std::string operation_id,Document document,AcknowledgementMode mode){std::lock_guard lock(mutex_);const MutationRecord* record_ptr=nullptr;if(const auto found=operation_indices_.find(operation_id);found!=operation_indices_.end()){record_ptr=&log_[found->second];if(!same_document(record_ptr->document,document))return std::unexpected(fail(ReplicationErrorCode::conflicting_operation,"operation ID was reused with different content"));}else{if(log_.size()>=limit_)return std::unexpected(fail(ReplicationErrorCode::log_full,"retained mutation log is full"));MutationRecord record{shard_,epoch_,std::move(operation_id),next_sequence_,std::move(document)};auto primary=replicas_.front()->apply(record);if(!primary)return std::unexpected(primary.error());operation_indices_[record.operation_id]=log_.size();log_.push_back(std::move(record));record_ptr=&log_.back();++next_sequence_;}std::size_t acknowledgements{};for(auto& replica:replicas_){const auto health=replica->status().health;if(replica.get()!=replicas_.front().get()&&(health==NodeHealth::unhealthy||health==NodeHealth::draining))continue;auto applied=replica->apply(*record_ptr);if(applied)++acknowledgements;}if(mode==AcknowledgementMode::all_replicas&&acknowledgements!=replicas_.size())return std::unexpected(fail(ReplicationErrorCode::insufficient_acknowledgements,"mutation committed on primary without every replica acknowledgement"));return MutationReceipt{record_ptr->sequence,acknowledgements};}
std::vector<ReplicaStatus> ReplicationGroup::statuses()const{std::lock_guard lock(mutex_);const auto primary=replicas_.front()->status().applied_sequence;std::vector<ReplicaStatus> result;result.reserve(replicas_.size());for(const auto& replica:replicas_)result.push_back(replica->status(primary));return result;}
std::expected<NodeId,ReplicationError> ReplicationGroup::select_read_replica(std::uint64_t maximum_lag)const{return ReplicaRouter::select(statuses(),maximum_lag);}
std::expected<void,ReplicationError> ReplicationGroup::set_health(const NodeId& node,NodeHealth health){std::lock_guard lock(mutex_);for(auto& replica:replicas_)if(replica->node_id()==node){if(health==NodeHealth::healthy||health==NodeHealth::suspect)for(const auto& record:log_){auto replayed=replica->apply(record);if(!replayed)return std::unexpected(replayed.error());}replica->set_health(health);return{};}return std::unexpected(fail(ReplicationErrorCode::invalid_topology,"node is not in replication group"));}
std::vector<MutationRecord> ReplicationGroup::retained_log()const{std::lock_guard lock(mutex_);return log_;}

std::expected<void, ReplicationError> PersistentMutationLog::append(
    const MutationRecord& record) const {
  if (record.shard.value() == 0U || record.epoch == 0U || record.sequence == 0U ||
      record.operation_id.empty() || record.document.id.value().empty())
    return std::unexpected(fail(ReplicationErrorCode::conflicting_operation,
                                "persistent mutation identity is incomplete"));
  Document persisted{.id = DocumentId(record.operation_id),
                     .fields = {},
                     .stored_metadata = {{"@shard", std::to_string(record.shard.value())},
                                         {"@epoch", std::to_string(record.epoch)},
                                         {"@document", record.document.id.value()},
                                         {"@deleted", record.document.deleted ? "1" : "0"}},
                     .version = record.sequence};
  for (const auto& [key, value] : record.document.fields)
    persisted.fields.emplace("f:" + key, value);
  for (const auto& [key, value] : record.document.stored_metadata)
    persisted.fields.emplace("m:" + key, value);
  persisted.stored_metadata.emplace("@version", std::to_string(record.document.version));
  const auto written = wal_.append(persisted);
  if (!written) return std::unexpected(fail(ReplicationErrorCode::mutation_error,
                                            "replica log append failed: " + written.error().message));
  return {};
}

std::expected<std::vector<MutationRecord>, ReplicationError> PersistentMutationLog::replay(
    ShardId shard, std::uint64_t epoch, const storage::WalLimits& limits) const {
  auto persisted = wal_.replay(limits);
  if (!persisted) return std::unexpected(fail(ReplicationErrorCode::mutation_error,
                                              "replica log recovery failed: " + persisted.error().message));
  const auto number = [](std::string_view text) -> std::optional<std::uint64_t> {
    std::uint64_t value{}; const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
    return value;
  };
  std::vector<MutationRecord> records; records.reserve(persisted->size());
  std::uint64_t expected_sequence{1};
  for (auto& wrapper : *persisted) {
    const auto shard_value = wrapper.stored_metadata.find("@shard");
    const auto epoch_value = wrapper.stored_metadata.find("@epoch");
    const auto document_id = wrapper.stored_metadata.find("@document");
    const auto document_version = wrapper.stored_metadata.find("@version");
    const auto deleted = wrapper.stored_metadata.find("@deleted");
    if (wrapper.id.value().empty() || wrapper.version != expected_sequence ||
        shard_value == wrapper.stored_metadata.end() || epoch_value == wrapper.stored_metadata.end() ||
        document_id == wrapper.stored_metadata.end() || document_version == wrapper.stored_metadata.end() ||
        deleted == wrapper.stored_metadata.end())
      return std::unexpected(fail(ReplicationErrorCode::sequence_gap, "invalid persistent replica sequence"));
    const auto parsed_shard = number(shard_value->second); const auto parsed_epoch = number(epoch_value->second);
    const auto parsed_version = number(document_version->second);
    if (!parsed_shard || !parsed_epoch || !parsed_version || *parsed_shard != shard.value() ||
        *parsed_epoch != epoch || (deleted->second != "0" && deleted->second != "1"))
      return std::unexpected(fail(ReplicationErrorCode::wrong_epoch, "persistent replica identity mismatch"));
    Document document{.id = DocumentId(document_id->second), .fields = {}, .stored_metadata = {},
                      .version = *parsed_version, .deleted = deleted->second == "1"};
    for (auto& [key, value] : wrapper.fields) {
      if (key.starts_with("f:")) document.fields.emplace(key.substr(2), std::move(value));
      else if (key.starts_with("m:")) document.stored_metadata.emplace(key.substr(2), std::move(value));
      else return std::unexpected(fail(ReplicationErrorCode::conflicting_operation,
                                       "invalid persistent replica document field"));
    }
    records.push_back({shard, epoch, wrapper.id.value(), wrapper.version, std::move(document)});
    ++expected_sequence;
  }
  return records;
}

std::expected<void, ReplicationError> PersistentMutationLog::reset() const {
  const auto reset = wal_.reset();
  if (!reset) return std::unexpected(fail(ReplicationErrorCode::mutation_error,
                                          "replica log reset failed: " + reset.error().message));
  return {};
}

std::expected<std::unique_ptr<OrderedReplica>, ReplicationError> recover_replica(
    NodeId node, ShardId shard, std::uint64_t epoch, const PersistentMutationLog& log,
    index::IndexSchema schema) {
  auto records = log.replay(shard, epoch);
  if (!records) return std::unexpected(records.error());
  auto replica = std::make_unique<OrderedReplica>(std::move(node), shard, epoch, std::move(schema));
  for (const auto& record : *records) {
    auto applied = replica->apply(record);
    if (!applied) return std::unexpected(applied.error());
  }
  return replica;
}

std::expected<void, ReplicationError> write_replica_snapshot(
    const std::filesystem::path& path, const OrderedReplica& replica) {
  const auto sequence = replica.status().applied_sequence;
  if (sequence == 0U)
    return std::unexpected(fail(ReplicationErrorCode::stale_sequence,
                                "cannot snapshot an empty replica"));
  const auto written = storage::SegmentWriter::write(
      path, replica.index().snapshot(),
      {.segment_id = SegmentId(sequence), .compressed_postings = true});
  if (!written) return std::unexpected(fail(ReplicationErrorCode::mutation_error,
                                             "replica snapshot failed: " + written.error().message));
  return {};
}
}  // namespace dse::distributed
