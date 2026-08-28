#include "dse/storage/index_writer.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {
class TempDir { public: TempDir(){static std::atomic_uint64_t n{}; p_=std::filesystem::temp_directory_path()/("dse-writer-"+std::to_string(++n));} ~TempDir(){std::error_code e;std::filesystem::remove_all(p_,e);} const auto& path()const{return p_;} private:std::filesystem::path p_;};
dse::Document doc(std::string id,std::string text,std::uint64_t version){return {.id=dse::DocumentId(std::move(id)),.fields={{"body",""},{"tags","test"},{"title",std::move(text)}},.version=version};}

TEST(IndexWriter, ThresholdFlushPublishesSearchableGenerations) {
  TempDir dir; auto writer=dse::storage::IndexWriter::open(dir.path(),dse::index::IndexSchema::default_schema(),{.maximum_buffered_mutations=2}); ASSERT_TRUE(writer);
  ASSERT_TRUE((*writer)->put(doc("a","first",1))); EXPECT_EQ((*writer)->generation(),dse::GenerationId(0));
  ASSERT_TRUE((*writer)->put(doc("b","second",1))); ASSERT_TRUE((*writer)->refresh()); EXPECT_EQ((*writer)->generation(),dse::GenerationId(1));
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

TEST(IndexWriter, AutomaticCompactionReclaimsFilesWithoutBreakingRetainedReaders) {
  TempDir dir;
  auto writer=dse::storage::IndexWriter::open(dir.path(),dse::index::IndexSchema::default_schema(),{.maximum_buffered_mutations=1,.maximum_frozen_indexes=2,.automatic_merge_segment_count=2,.reclaim_obsolete_files=true}); ASSERT_TRUE(writer);
  ASSERT_TRUE((*writer)->put(doc("a","first",1))); ASSERT_TRUE((*writer)->refresh());
  auto retained=(*writer)->open_search_view(); ASSERT_TRUE(retained);
  ASSERT_TRUE((*writer)->put(doc("b","second",1))); ASSERT_TRUE((*writer)->refresh());
  auto current=(*writer)->open_search_view(); ASSERT_TRUE(current);
  EXPECT_EQ(current->source().manifest().segments.size(),1U);
  EXPECT_EQ(current->live_document_count(),2U);
  EXPECT_NE(retained->document(dse::DocumentId("a")),nullptr);
  EXPECT_FALSE(std::filesystem::exists(dir.path()/"segment-1.dseg"));
}

TEST(IndexWriter, ConcurrentProducersRemainBoundedAndSearchable) {
  TempDir dir;
  auto writer=dse::storage::IndexWriter::open(dir.path(),dse::index::IndexSchema::default_schema(),{.maximum_buffered_mutations=3,.maximum_frozen_indexes=1,.automatic_merge_segment_count=4}); ASSERT_TRUE(writer);
  std::atomic_bool failed{}; std::vector<std::thread> producers;
  for(std::uint64_t thread=0;thread<4;++thread) producers.emplace_back([&,thread]{for(std::uint64_t i=0;i<10;++i){auto result=(*writer)->put(doc("doc-"+std::to_string(thread)+"-"+std::to_string(i),"parallel",1));if(!result)failed=true;}});
  for(auto& producer:producers)producer.join();
  EXPECT_FALSE(failed.load()); ASSERT_TRUE((*writer)->refresh());
  const auto stats=(*writer)->statistics(); EXPECT_EQ(stats.buffered_mutations,0U); EXPECT_EQ(stats.frozen_indexes,0U);
  auto view=(*writer)->open_search_view(); ASSERT_TRUE(view); EXPECT_EQ(view->live_document_count(),40U);
}

TEST(IndexWriter, StartupReclaimsOrphanSegmentsManifestsAndTemporaryFiles) {
  TempDir dir;
  { auto writer=dse::storage::IndexWriter::open(dir.path(),dse::index::IndexSchema::default_schema(),{.maximum_buffered_mutations=1}); ASSERT_TRUE(writer); ASSERT_TRUE((*writer)->put(doc("a","kept",1))); ASSERT_TRUE((*writer)->refresh()); }
  std::filesystem::copy_file(dir.path()/"segment-1.dseg",dir.path()/"segment-999.dseg");
  { std::ofstream orphan(dir.path()/"MANIFEST-999",std::ios::binary); orphan<<"orphan"; }
  { std::ofstream temporary(dir.path()/"abandoned.tmp",std::ios::binary); temporary<<"partial"; }
  auto writer=dse::storage::IndexWriter::open(dir.path()); ASSERT_TRUE(writer);
  EXPECT_FALSE(std::filesystem::exists(dir.path()/"segment-999.dseg"));
  EXPECT_FALSE(std::filesystem::exists(dir.path()/"MANIFEST-999"));
  EXPECT_FALSE(std::filesystem::exists(dir.path()/"abandoned.tmp"));
  EXPECT_TRUE(std::filesystem::exists(dir.path()/"segment-1.dseg"));
  auto view=(*writer)->open_search_view(); ASSERT_TRUE(view); EXPECT_EQ(view->live_document_count(),1U);
}
}  // namespace
