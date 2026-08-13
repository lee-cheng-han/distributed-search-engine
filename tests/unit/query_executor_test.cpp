#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>

namespace {

dse::Document document(std::string id, std::string title, std::string body, std::string timestamp,
                       std::uint64_t version = 1) {
  return {.id = dse::DocumentId(std::move(id)),
          .fields = {{"body", std::move(body)}, {"title", std::move(title)}},
          .stored_metadata = {{"timestamp", std::move(timestamp)}},
          .version = version};
}

std::set<std::string> ids(const dse::query::SearchResult& result) {
  std::set<std::string> values;
  for (const auto& hit : result.hits) values.insert(hit.document_id.value());
  return values;
}

dse::query::SearchResult search(const dse::query::QueryExecutor& executor, std::string_view text,
                                std::size_t top_k = 10) {
  auto query = dse::query::parse(text);
  EXPECT_TRUE(query.has_value()) << (query ? "" : query.error().message);
  if (!query) return {};
  auto result = executor.search(**query, {.top_k = top_k});
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
  return result ? std::move(*result) : dse::query::SearchResult{};
}

class QueryExecutorTest : public ::testing::Test {
 protected:
  QueryExecutorTest() : executor(index) {
    EXPECT_TRUE(
        index.put(document("a", "Distributed search engine", "reliable systems", "2024-01-01")));
    EXPECT_TRUE(
        index.put(document("b", "Distributed fast search", "slow systems", "2025-01-01")));
    EXPECT_TRUE(index.put(document("c", "GPU compute", "fast parallel search", "2026-01-01")));
    EXPECT_TRUE(index.put(document("d", "CPU compute", "batch processing", "2023-01-01")));
  }

  dse::index::InMemoryIndex index;
  dse::query::QueryExecutor executor;
};

TEST_F(QueryExecutorTest, ExecutesTermsAcrossDefaultFieldsWithBM25Scores) {
  const auto result = search(executor, "search");
  EXPECT_EQ(result.total_hits, 3U);
  EXPECT_EQ(ids(result), (std::set<std::string>{"a", "b", "c"}));
  for (const auto& hit : result.hits) EXPECT_GT(hit.score, 0.0);
}

TEST_F(QueryExecutorTest, ExecutesBooleanIntersectionUnionAndExclusion) {
  EXPECT_EQ(ids(search(executor, "distributed AND systems")),
            (std::set<std::string>{"a", "b"}));
  EXPECT_EQ(ids(search(executor, "gpu OR cpu")), (std::set<std::string>{"c", "d"}));
  EXPECT_EQ(ids(search(executor, "distributed AND NOT slow")),
            (std::set<std::string>{"a"}));
}

TEST_F(QueryExecutorTest, RequiresPhrasePositionsWithinOneField) {
  const auto result = search(executor, "\"distributed search\"");
  EXPECT_EQ(result.total_hits, 1U);
  ASSERT_EQ(result.hits.size(), 1U);
  EXPECT_EQ(result.hits[0].document_id, dse::DocumentId("a"));
}

TEST(QueryExecutor, PreservesAnalyzerPositionGapsInPhrases) {
  const auto analyzer =
      std::make_shared<const dse::analysis::StandardAnalyzer>(
          std::set<std::string, std::less<>>{"the"});
  auto schema = dse::index::IndexSchema::create(
      {{"title", dse::index::FieldType::text, true, true, 1.0, analyzer},
       {"body", dse::index::FieldType::text, true, true, 1.0, analyzer},
       {"timestamp", dse::index::FieldType::timestamp, false, true, 1.0, nullptr}});
  ASSERT_TRUE(schema.has_value());
  dse::index::InMemoryIndex index(std::move(*schema));
  ASSERT_TRUE(index.put(document("gap", "distributed the search", "", "2024-01-01")));
  ASSERT_TRUE(index.put(document("adjacent", "distributed search", "", "2024-01-01")));
  const dse::query::QueryExecutor executor(index);
  const auto result = search(executor, "title:\"distributed the search\"");
  ASSERT_EQ(result.hits.size(), 1U);
  EXPECT_EQ(result.hits[0].document_id, dse::DocumentId("gap"));
}

TEST_F(QueryExecutorTest, RestrictsFieldedQueriesAndAppliesBoosts) {
  EXPECT_EQ(ids(search(executor, "title:systems")), std::set<std::string>{});
  const auto base = search(executor, "title:distributed");
  const auto boosted = search(executor, "title:distributed^2.5");
  ASSERT_EQ(base.hits.size(), boosted.hits.size());
  for (std::size_t index = 0; index < base.hits.size(); ++index) {
    EXPECT_EQ(base.hits[index].document_id, boosted.hits[index].document_id);
    EXPECT_DOUBLE_EQ(boosted.hits[index].score, base.hits[index].score * 2.5);
  }
}

TEST_F(QueryExecutorTest, AppliesInclusiveMetadataRangeFilters) {
  const auto result = search(executor, "timestamp:[2024-01-01 TO 2025-12-31]");
  EXPECT_EQ(result.total_hits, 2U);
  EXPECT_EQ(ids(result), (std::set<std::string>{"a", "b"}));
  EXPECT_EQ(ids(search(executor, "timestamp:[2026-01-01 TO *]")),
            (std::set<std::string>{"c"}));
}

TEST_F(QueryExecutorTest, CombinesFiltersWithScoredQueries) {
  const auto result =
      search(executor, "search AND timestamp:[2025-01-01 TO 2026-12-31]");
  EXPECT_EQ(ids(result), (std::set<std::string>{"b", "c"}));
  for (const auto& hit : result.hits) EXPECT_GT(hit.score, 0.0);
}

TEST_F(QueryExecutorTest, CollectsTopKAndBreaksZeroScoreTiesById) {
  const auto result = search(executor, "*", 2);
  EXPECT_EQ(result.total_hits, 4U);
  ASSERT_EQ(result.hits.size(), 2U);
  EXPECT_EQ(result.hits[0].document_id, dse::DocumentId("a"));
  EXPECT_EQ(result.hits[1].document_id, dse::DocumentId("b"));
}

TEST_F(QueryExecutorTest, DeletedDocumentsNeverAppear) {
  ASSERT_TRUE(index.erase(dse::DocumentId("a"), 2));
  EXPECT_EQ(ids(search(executor, "distributed")), (std::set<std::string>{"b"}));
  EXPECT_EQ(search(executor, "*").total_hits, 3U);
}

TEST_F(QueryExecutorTest, RejectsInvalidOptionsAndMalformedAstNodes) {
  const auto parsed = dse::query::parse("search");
  ASSERT_TRUE(parsed.has_value());
  const auto invalid_options = executor.search(
      **parsed, {.top_k = 10, .default_fields = {{"title", -1.0}}});
  ASSERT_FALSE(invalid_options.has_value());
  EXPECT_EQ(invalid_options.error().code, dse::query::ExecutionErrorCode::invalid_options);

  const dse::query::QueryNode invalid{
      dse::query::AndQuery{dse::query::Query{}, dse::query::Query{}}, 0};
  const auto invalid_tree = executor.search(invalid);
  ASSERT_FALSE(invalid_tree.has_value());
  EXPECT_EQ(invalid_tree.error().code, dse::query::ExecutionErrorCode::invalid_query_tree);
}

}  // namespace
