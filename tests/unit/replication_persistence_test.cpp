#include "dse/distributed/replication.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>

namespace {
class TemporaryLog {
 public:
  TemporaryLog() { static std::atomic_uint64_t next{}; path = std::filesystem::temp_directory_path() / ("dse-replica-" + std::to_string(++next) + ".log"); }
  ~TemporaryLog() { std::error_code ignored; std::filesystem::remove(path, ignored); }
  std::filesystem::path path;
};
}

TEST(ReplicationPersistence, ReplaysChecksummedOrderedMutationsAfterRestart) {
  TemporaryLog file;
  dse::distributed::PersistentMutationLog log(file.path);
  const dse::ShardId shard(3);
  ASSERT_TRUE(log.append({shard, 9, "op-1", 1,
      {.id = dse::DocumentId("doc"), .fields = {{"body", "first"}}, .version = 1}}));
  ASSERT_TRUE(log.append({shard, 9, "op-2", 2,
      {.id = dse::DocumentId("doc"), .fields = {{"body", "durable replica"}},
       .stored_metadata = {{"timestamp", "2026-09-06"}}, .version = 2}}));

  auto recovered = dse::distributed::recover_replica(dse::NodeId("restarted"), shard, 9, log);
  ASSERT_TRUE(recovered.has_value()) << (recovered ? "" : recovered.error().message);
  EXPECT_EQ((*recovered)->status().applied_sequence, 2U);
  const auto* document = (*recovered)->index().document(dse::DocumentId("doc"));
  ASSERT_NE(document, nullptr);
  EXPECT_EQ(document->document.fields.at("body"), "durable replica");
  auto wrong_epoch = log.replay(shard, 10);
  ASSERT_FALSE(wrong_epoch.has_value());
  EXPECT_EQ(wrong_epoch.error().code, dse::distributed::ReplicationErrorCode::wrong_epoch);
}

TEST(ReplicationPersistence, RestoresVerifiedSnapshotThenContinuesAtNextSequence) {
  TemporaryLog file;
  dse::distributed::OrderedReplica source(dse::NodeId("source"), dse::ShardId(4), 2);
  ASSERT_TRUE(source.apply({dse::ShardId(4), 2, "op-1", 1,
      {.id = dse::DocumentId("a"), .fields = {{"body", "snapshot state"}}, .version = 1}}));
  ASSERT_TRUE(source.apply({dse::ShardId(4), 2, "op-2", 2,
      {.id = dse::DocumentId("b"), .fields = {{"body", "second record"}}, .version = 1}}));
  ASSERT_TRUE(dse::distributed::write_replica_snapshot(file.path, source));
  auto snapshot = dse::storage::SegmentReader::open(file.path);
  ASSERT_TRUE(snapshot.has_value());
  auto restored = dse::distributed::OrderedReplica::restore(
      dse::NodeId("target"), dse::ShardId(4), 2, *snapshot);
  ASSERT_TRUE(restored.has_value()) << (restored ? "" : restored.error().message);
  EXPECT_EQ((*restored)->status().applied_sequence, 2U);
  EXPECT_NE((*restored)->index().document(dse::DocumentId("a")), nullptr);
  auto next = (*restored)->apply({dse::ShardId(4), 2, "op-3", 3,
      {.id = dse::DocumentId("c"), .fields = {{"body", "caught up"}}, .version = 1}});
  EXPECT_TRUE(next.has_value());
}
