#include "dse/ranking/top_k.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace {

dse::ranking::ScoredDocument scored(std::string id, double score) {
  return {dse::DocumentId(std::move(id)), score};
}

TEST(TopKCollector, MatchesFullSortReference) {
  std::vector<dse::ranking::ScoredDocument> candidates{
      scored("d", 4.0), scored("a", 2.0), scored("c", 8.0),
      scored("b", 8.0), scored("e", -1.0)};
  dse::ranking::TopKCollector collector(3);
  for (const auto& candidate : candidates) ASSERT_TRUE(collector.collect(candidate));

  std::ranges::sort(candidates, dse::ranking::BetterScore{});
  candidates.resize(3);
  const auto results = collector.results();
  ASSERT_EQ(results.size(), candidates.size());
  for (std::size_t index = 0; index < results.size(); ++index) {
    EXPECT_EQ(results[index].document_id, candidates[index].document_id);
    EXPECT_DOUBLE_EQ(results[index].score, candidates[index].score);
  }
}

TEST(TopKCollector, BreaksEqualScoreTiesByAscendingDocumentId) {
  dse::ranking::TopKCollector collector(2);
  ASSERT_TRUE(collector.collect(scored("z", 1.0)));
  ASSERT_TRUE(collector.collect(scored("b", 1.0)));
  ASSERT_TRUE(collector.collect(scored("a", 1.0)));
  const auto results = collector.results();
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(results[0].document_id, dse::DocumentId("a"));
  EXPECT_EQ(results[1].document_id, dse::DocumentId("b"));
}

TEST(TopKCollector, HandlesZeroCapacityAndRejectsNonFiniteScores) {
  dse::ranking::TopKCollector collector(0);
  EXPECT_TRUE(collector.collect(scored("a", 1.0)));
  EXPECT_TRUE(collector.results().empty());

  dse::ranking::TopKCollector finite_collector(1);
  EXPECT_FALSE(finite_collector.collect(
      scored("nan", std::numeric_limits<double>::quiet_NaN())));
  EXPECT_FALSE(finite_collector.collect(
      scored("inf", std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(finite_collector.results().empty());
}

}  // namespace
