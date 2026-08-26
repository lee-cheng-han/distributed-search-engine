#include "dse/storage/index_writer.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>

namespace {
class TempDir { public: TempDir(){static std::atomic_uint64_t n{}; p_=std::filesystem::temp_directory_path()/("dse-writer-"+std::to_string(++n));} ~TempDir(){std::error_code e;std::filesystem::remove_all(p_,e);} const auto& path()const{return p_;} private:std::filesystem::path p_;};
dse::Document doc(std::string id,std::string text,std::uint64_t version){return {.id=dse::DocumentId(std::move(id)),.fields={{"body",""},{"tags","test"},{"title",std::move(text)}},.version=version};}

TEST(IndexWriter, ThresholdFlushPublishesSearchableGenerations) {
  TempDir dir; auto writer=dse::storage::IndexWriter::open(dir.path(),dse::index::IndexSchema::default_schema(),{.maximum_buffered_mutations=2}); ASSERT_TRUE(writer);
  ASSERT_TRUE((*writer)->put(doc("a","first",1))); EXPECT_EQ((*writer)->generation(),dse::GenerationId(0));
  ASSERT_TRUE((*writer)->put(doc("b","second",1))); EXPECT_EQ((*writer)->generation(),dse::GenerationId(1));
  auto view=(*writer)->open_search_view(); ASSERT_TRUE(view); EXPECT_EQ(view->live_document_count(),2U);
  ASSERT_TRUE((*writer)->put(doc("a","latest",2))); ASSERT_TRUE((*writer)->refresh());
  view=(*writer)->open_search_view(); ASSERT_TRUE(view); EXPECT_EQ(view->document(dse::DocumentId("a"))->document.fields.at("title"),"latest");
}

TEST(IndexWriter, RestartPreservesVersionsAndMergeCompactsGeneration) {
  TempDir dir;
  { auto writer=dse::storage::IndexWriter::open(dir.path(),dse::index::IndexSchema::default_schema(),{.maximum_buffered_mutations=1}); ASSERT_TRUE(writer); ASSERT_TRUE((*writer)->put(doc("a","old",3))); ASSERT_TRUE((*writer)->put(doc("b","gone",1))); ASSERT_TRUE((*writer)->erase(dse::DocumentId("b"),2)); }
  auto writer=dse::storage::IndexWriter::open(dir.path()); ASSERT_TRUE(writer);
  auto stale=(*writer)->put(doc("a","stale",3)); ASSERT_FALSE(stale); EXPECT_EQ(stale.error().code,dse::storage::WriterErrorCode::stale_version);
  ASSERT_TRUE((*writer)->put(doc("a","new",4))); ASSERT_TRUE((*writer)->refresh());
  const auto before=(*writer)->generation(); ASSERT_TRUE((*writer)->merge_all()); EXPECT_GT((*writer)->generation().value(),before.value());
  auto view=(*writer)->open_search_view(); ASSERT_TRUE(view); EXPECT_EQ(view->source().manifest().segments.size(),1U); EXPECT_EQ(view->live_document_count(),1U); EXPECT_EQ(view->document(dse::DocumentId("a"))->document.version,4U);
}
}  // namespace
