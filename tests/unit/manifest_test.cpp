#include "dse/storage/manifest.hpp"

#include "dse/index/in_memory_index.hpp"
#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    static std::atomic_uint64_t next{};
    path_ = std::filesystem::temp_directory_path() /
            ("dse-manifest-test-" + std::to_string(++next));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
 private:
  std::filesystem::path path_;
};

dse::index::InMemoryIndex index_with(std::string id, std::string title) {
  dse::index::InMemoryIndex index;
  EXPECT_TRUE(index.put({.id = dse::DocumentId(std::move(id)),
                         .fields = {{"body", ""}, {"tags", "test"},
                                    {"title", std::move(title)}}}));
  return index;
}

TEST(Manifest, PublishesAndReopensOneAtomicGeneration) {
  TemporaryDirectory directory;
  auto index = index_with("a", "persistent search");
  ASSERT_TRUE(dse::storage::SegmentWriter::write(
      directory.path() / "segment-1.dseg", index.snapshot(),
      {.segment_id = dse::SegmentId(1)}));

  const dse::storage::ManifestStore store(directory.path());
  const dse::storage::IndexManifest manifest{
      dse::GenerationId(1), {{dse::SegmentId(1), "segment-1.dseg"}}};
  ASSERT_TRUE(store.publish(manifest));
  auto loaded = store.load_current();
  ASSERT_TRUE(loaded.has_value()) << (loaded ? "" : loaded.error().message);
  EXPECT_EQ(*loaded, manifest);
  auto stale = store.publish(manifest);
  ASSERT_FALSE(stale.has_value());
  EXPECT_EQ(stale.error().code, dse::storage::ManifestErrorCode::invalid_manifest);

  auto generation = store.open_current();
  ASSERT_TRUE(generation.has_value());
  ASSERT_EQ(generation->segments().size(), 1U);
  EXPECT_EQ(generation->segments()[0]->document(dse::DocumentId("a"))->document.fields.at("title"),
            "persistent search");
}

TEST(Manifest, InterruptedPublicationKeepsPreviousCurrentGeneration) {
  TemporaryDirectory directory;
  auto first = index_with("a", "first");
  auto second = index_with("b", "second");
  ASSERT_TRUE(dse::storage::SegmentWriter::write(
      directory.path() / "segment-1.dseg", first.snapshot(),
      {.segment_id = dse::SegmentId(1)}));
  ASSERT_TRUE(dse::storage::SegmentWriter::write(
      directory.path() / "segment-2.dseg", second.snapshot(),
      {.segment_id = dse::SegmentId(2)}));
  const dse::storage::ManifestStore store(directory.path());
  ASSERT_TRUE(store.publish({dse::GenerationId(1),
                             {{dse::SegmentId(1), "segment-1.dseg"}}}));
  auto retained = store.open_current();
  ASSERT_TRUE(retained.has_value());

  auto interrupted = store.publish(
      {dse::GenerationId(2), {{dse::SegmentId(2), "segment-2.dseg"}}},
      {.fault_point = dse::storage::ManifestFaultPoint::after_manifest_publish});
  ASSERT_FALSE(interrupted.has_value());
  EXPECT_EQ(interrupted.error().code, dse::storage::ManifestErrorCode::injected_failure);
  auto current = store.load_current();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(current->generation, dse::GenerationId(1));
  EXPECT_NE(retained->segments()[0]->document(dse::DocumentId("a")), nullptr);

  ASSERT_TRUE(store.publish({dse::GenerationId(2),
                             {{dse::SegmentId(2), "segment-2.dseg"}}}));
  current = store.load_current();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(current->generation, dse::GenerationId(2));
  EXPECT_NE(retained->segments()[0]->document(dse::DocumentId("a")), nullptr);
}

TEST(Manifest, RejectsMissingMismatchedAndCorruptMetadata) {
  TemporaryDirectory directory;
  auto index = index_with("a", "first");
  ASSERT_TRUE(dse::storage::SegmentWriter::write(
      directory.path() / "segment.dseg", index.snapshot(),
      {.segment_id = dse::SegmentId(7)}));
  const dse::storage::ManifestStore store(directory.path());
  EXPECT_FALSE(store.publish({dse::GenerationId(1),
                              {{dse::SegmentId(8), "segment.dseg"}}}));
  EXPECT_FALSE(store.publish({dse::GenerationId(1),
                              {{dse::SegmentId(7), "missing.dseg"}}}));
  ASSERT_TRUE(store.publish({dse::GenerationId(1),
                             {{dse::SegmentId(7), "segment.dseg"}}}));

  std::fstream current(directory.path() / "CURRENT", std::ios::binary | std::ios::in |
                                                        std::ios::out);
  ASSERT_TRUE(current.good());
  current.seekp(0);
  current.put('X');
  current.close();
  auto loaded = store.load_current();
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().code, dse::storage::ManifestErrorCode::corruption);
}

}  // namespace
