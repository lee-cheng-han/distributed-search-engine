#include "dse/storage/generation.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>

namespace {
class TempDir {
 public:
  TempDir() { static std::atomic_uint64_t n{}; path_ = std::filesystem::temp_directory_path() / ("dse-generation-" + std::to_string(++n)); std::filesystem::create_directories(path_); }
  ~TempDir() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
  const std::filesystem::path& path() const { return path_; }
 private: std::filesystem::path path_;
};

dse::Document doc(std::string id, std::string title, std::uint64_t version) {
  return {.id=dse::DocumentId(std::move(id)), .fields={{"body", ""}, {"tags", "test"}, {"title", std::move(title)}}, .version=version};
}

TEST(Generation, ResolvesNewestVersionsAndTombstonesAcrossSegments) {
  TempDir dir;
  dse::index::InMemoryIndex first;
  ASSERT_TRUE(first.put(doc("a", "old", 1)));
  ASSERT_TRUE(first.put(doc("b", "removed", 1)));
  dse::index::InMemoryIndex second;
  ASSERT_TRUE(second.put(doc("a", "new", 2)));
  ASSERT_TRUE(second.erase(dse::DocumentId("b"), 2));
  ASSERT_TRUE(dse::storage::SegmentWriter::write(dir.path()/"s1.dseg", first.snapshot(), {.segment_id=dse::SegmentId(1)}));
  ASSERT_TRUE(dse::storage::SegmentWriter::write(dir.path()/"s2.dseg", second.snapshot(), {.segment_id=dse::SegmentId(2)}));
  dse::storage::ManifestStore store(dir.path());
  ASSERT_TRUE(store.publish({dse::GenerationId(1), {{dse::SegmentId(1),"s1.dseg"},{dse::SegmentId(2),"s2.dseg"}}}));
  auto opened = store.open_current(); ASSERT_TRUE(opened);
  auto view = dse::storage::GenerationView::open(std::move(*opened)); ASSERT_TRUE(view);
  ASSERT_NE(view->document(dse::DocumentId("a")), nullptr);
  EXPECT_EQ(view->document(dse::DocumentId("a"))->document.fields.at("title"), "new");
  ASSERT_NE(view->document(dse::DocumentId("b")), nullptr);
  EXPECT_TRUE(view->document(dse::DocumentId("b"))->document.deleted);
  EXPECT_EQ(view->live_document_count(), 1U);
  EXPECT_EQ(view->lookup("title", "old"), nullptr);
}

TEST(Generation, MergeProducesEquivalentSingleSegment) {
  TempDir dir;
  dse::index::InMemoryIndex one; ASSERT_TRUE(one.put(doc("a", "first", 1)));
  dse::index::InMemoryIndex two; ASSERT_TRUE(two.put(doc("a", "latest", 2))); ASSERT_TRUE(two.put(doc("c", "third", 1)));
  ASSERT_TRUE(dse::storage::SegmentWriter::write(dir.path()/"s1.dseg", one.snapshot(), {.segment_id=dse::SegmentId(1)}));
  ASSERT_TRUE(dse::storage::SegmentWriter::write(dir.path()/"s2.dseg", two.snapshot(), {.segment_id=dse::SegmentId(2)}));
  dse::storage::ManifestStore store(dir.path()); ASSERT_TRUE(store.publish({dse::GenerationId(1),{{dse::SegmentId(1),"s1.dseg"},{dse::SegmentId(2),"s2.dseg"}}}));
  auto opened=store.open_current(); ASSERT_TRUE(opened); auto view=dse::storage::GenerationView::open(std::move(*opened)); ASSERT_TRUE(view);
  auto merged=dse::storage::SegmentMerger::merge(*view,dir.path(),dse::SegmentId(3)); ASSERT_TRUE(merged);
  ASSERT_TRUE(store.publish({dse::GenerationId(2),{*merged}}));
  auto reopened=store.open_current(); ASSERT_TRUE(reopened); auto compact=dse::storage::GenerationView::open(std::move(*reopened)); ASSERT_TRUE(compact);
  EXPECT_EQ(compact->live_document_count(),view->live_document_count());
  EXPECT_EQ(compact->document(dse::DocumentId("a"))->document.version,2U);
  EXPECT_NE(compact->document(dse::DocumentId("c")),nullptr);
}
}  // namespace
