#include "dse/index/in_memory_index.hpp"

#include <gtest/gtest.h>

namespace {

dse::Document doc(std::string id, std::uint64_t version, std::string title,
                  std::string body = {}) {
  return {.id = dse::DocumentId(std::move(id)),
          .fields = {{"body", std::move(body)}, {"title", std::move(title)}},
          .version = version};
}

TEST(InMemoryIndex, BuildsFieldSpecificSortedPostingsAndStatistics) {
  const dse::analysis::StandardAnalyzer analyzer;
  dse::index::InMemoryIndex index(analyzer);
  ASSERT_TRUE(index.put(doc("b", 1, "Search systems", "search search")));
  ASSERT_TRUE(index.put(doc("a", 1, "Distributed search")));

  const auto* title_search = index.lookup("title", "search");
  ASSERT_NE(title_search, nullptr);
  ASSERT_EQ(title_search->document_frequency, 2U);
  ASSERT_EQ(title_search->postings.size(), 2U);
  EXPECT_EQ(title_search->postings[0].document_id, dse::DocumentId("a"));
  EXPECT_EQ(title_search->postings[1].document_id, dse::DocumentId("b"));
  const auto* body_search = index.lookup("body", "search");
  ASSERT_NE(body_search, nullptr);
  EXPECT_EQ(body_search->postings[0].term_frequency, 2U);
  EXPECT_EQ(body_search->postings[0].positions, (std::vector<std::uint32_t>{0, 1}));
  EXPECT_TRUE(index.validate_invariants());
}

TEST(InMemoryIndex, UpdateReplacesOldPostingsAndRejectsStaleVersions) {
  const dse::analysis::StandardAnalyzer analyzer;
  dse::index::InMemoryIndex index(analyzer);
  ASSERT_TRUE(index.put(doc("doc", 1, "old term")));
  EXPECT_FALSE(index.put(doc("doc", 1, "ignored")));
  ASSERT_TRUE(index.put(doc("doc", 2, "new term")));
  EXPECT_EQ(index.lookup("title", "old"), nullptr);
  EXPECT_NE(index.lookup("title", "new"), nullptr);
  EXPECT_TRUE(index.validate_invariants());
}

TEST(InMemoryIndex, TombstoneRemovesDocumentFromPostings) {
  const dse::analysis::StandardAnalyzer analyzer;
  dse::index::InMemoryIndex index(analyzer);
  ASSERT_TRUE(index.put(doc("doc", 1, "visible")));
  ASSERT_TRUE(index.erase(dse::DocumentId("doc"), 2));
  EXPECT_EQ(index.live_document_count(), 0U);
  EXPECT_EQ(index.lookup("title", "visible"), nullptr);
  ASSERT_NE(index.document(dse::DocumentId("doc")), nullptr);
  EXPECT_TRUE(index.document(dse::DocumentId("doc"))->document.deleted);
  EXPECT_FALSE(index.erase(dse::DocumentId("doc"), 2));
  EXPECT_TRUE(index.validate_invariants());
}

TEST(InMemoryIndex, ComputesFieldSpecificLengthStatisticsForLiveDocuments) {
  const dse::analysis::StandardAnalyzer analyzer;
  dse::index::InMemoryIndex index(analyzer);
  ASSERT_TRUE(index.put(doc("a", 1, "one two", "one two three four")));
  ASSERT_TRUE(index.put(doc("b", 1, "one two three four", "")));
  ASSERT_TRUE(index.put(doc("c", 1, "deleted")));
  ASSERT_TRUE(index.erase(dse::DocumentId("c"), 2));

  const auto title = index.field_statistics("title");
  EXPECT_EQ(title.document_count, 2U);
  EXPECT_EQ(title.total_length, 6U);
  EXPECT_DOUBLE_EQ(title.average_length, 3.0);
  const auto body = index.field_statistics("body");
  EXPECT_EQ(body.document_count, 2U);
  EXPECT_EQ(body.total_length, 4U);
  EXPECT_DOUBLE_EQ(body.average_length, 2.0);
  EXPECT_EQ(index.field_statistics("missing").document_count, 0U);
}

}  // namespace
