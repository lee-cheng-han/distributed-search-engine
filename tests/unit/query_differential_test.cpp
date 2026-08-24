#include "../support/reference_evaluator.hpp"

#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"
#include "dse/query/planner.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr double kScoreTolerance = 1e-12;

void expect_equivalent(const dse::index::InMemoryIndex& index, std::string_view text,
                       std::size_t top_k) {
  auto ast = dse::query::parse(text);
  ASSERT_TRUE(ast.has_value()) << text << ": " << (ast ? "" : ast.error().message);
  auto plan = dse::query::QueryPlanner(index).plan(**ast);
  ASSERT_TRUE(plan.has_value()) << text << ": " << (plan ? "" : plan.error().message);

  auto optimized = dse::query::QueryExecutor(index).search(*plan, top_k);
  ASSERT_TRUE(optimized.has_value()) << text << ": "
                                    << (optimized ? "" : optimized.error().message);
  const auto reference = dse::test::ReferenceEvaluator(index).search(*plan, top_k);

  ASSERT_EQ(optimized->total_hits, reference.total_hits) << text;
  ASSERT_EQ(optimized->hits.size(), reference.hits.size()) << text;
  for (std::size_t i = 0; i < reference.hits.size(); ++i) {
    EXPECT_EQ(optimized->hits[i].document_id, reference.hits[i].document_id) << text << " @" << i;
    EXPECT_NEAR(optimized->hits[i].score, reference.hits[i].score, kScoreTolerance)
        << text << " @" << i;
  }
}

std::string date(std::uint32_t value) {
  std::ostringstream output;
  output << "202" << value % 7U << '-' << std::setfill('0') << std::setw(2)
         << (value % 12U) + 1U << '-' << std::setw(2) << (value % 28U) + 1U;
  return output.str();
}

dse::Document generated_document(std::mt19937& random, std::uint32_t number,
                                 std::uint64_t version = 1) {
  constexpr std::array<std::string_view, 8> words{
      "alpha", "beta", "gamma", "delta", "search", "engine", "fast", "slow"};
  const auto sentence = [&](std::size_t length) {
    std::string result;
    for (std::size_t i = 0; i < length; ++i) {
      if (!result.empty()) result += ' ';
      result += words[random() % words.size()];
    }
    return result;
  };
  return {.id = dse::DocumentId("doc-" + std::to_string(number)),
          .fields = {{"body", sentence(3U + random() % 8U)},
                     {"tags", std::string(words[random() % words.size()])},
                     {"title", sentence(2U + random() % 5U)}},
          .stored_metadata = {{"timestamp", date(number)}},
          .version = version};
}

TEST(QueryDifferential, CoversHandSelectedSemanticsAndTopKBoundaries) {
  dse::index::InMemoryIndex index;
  ASSERT_TRUE(index.put({.id = dse::DocumentId("a"),
                         .fields = {{"body", "fast distributed engine"},
                                    {"tags", "Systems"},
                                    {"title", "distributed search"}},
                         .stored_metadata = {{"timestamp", "2024-01-01"}}}));
  ASSERT_TRUE(index.put({.id = dse::DocumentId("b"),
                         .fields = {{"body", "slow search"},
                                    {"tags", "search"},
                                    {"title", "distributed fast search"}},
                         .stored_metadata = {{"timestamp", "2025-06-15"}}}));
  ASSERT_TRUE(index.put({.id = dse::DocumentId("c"),
                         .fields = {{"body", "unrelated"},
                                    {"tags", "Systems"},
                                    {"title", "search search"}},
                         .stored_metadata = {{"timestamp", "2026-12-31"}}}));
  ASSERT_TRUE(index.put({.id = dse::DocumentId("d"),
                         .fields = {{"body", ""}, {"tags", ""}, {"title", ""}},
                         .stored_metadata = {{"timestamp", "2022-01-01"}}}));

  constexpr std::array queries{
      "search", "title:search", "tags:Systems", "tags:systems",
      "\"distributed search\"", "title:\"distributed fast search\"",
      "distributed AND search", "fast OR slow", "search AND NOT slow", "NOT search",
      "search^2.5", "title:search OR title:search", "---",
      "timestamp:[2024-01-01 TO 2025-12-31]", "search AND timestamp:[2025-01-01 TO *]",
      "*"};
  for (const auto query : queries) {
    for (const auto top_k : {0U, 1U, 2U, 10U}) expect_equivalent(index, query, top_k);
  }

  ASSERT_TRUE(index.put({.id = dse::DocumentId("b"),
                         .fields = {{"body", "updated alpha"},
                                    {"tags", "updated"},
                                    {"title", "no longer matching"}},
                         .stored_metadata = {{"timestamp", "2023-02-02"}},
                         .version = 2}));
  ASSERT_TRUE(index.erase(dse::DocumentId("a"), 2));
  for (const auto query : queries) expect_equivalent(index, query, 10);
}

TEST(QueryDifferential, SeededGeneratedCorporaAndQueriesMatchReference) {
  constexpr std::array<std::string_view, 8> words{
      "alpha", "beta", "gamma", "delta", "search", "engine", "fast", "slow"};
  for (const std::uint32_t seed : {7U, 29U, 101U}) {
    std::mt19937 random(seed);
    dse::index::InMemoryIndex index;
    for (std::uint32_t i = 0; i < 36U; ++i) ASSERT_TRUE(index.put(generated_document(random, i)));
    for (std::uint32_t i = 0; i < 6U; ++i)
      ASSERT_TRUE(index.put(generated_document(random, i, 2)));
    for (std::uint32_t i = 30U; i < 34U; ++i)
      ASSERT_TRUE(index.erase(dse::DocumentId("doc-" + std::to_string(i)), 2));
    ASSERT_TRUE(index.validate_invariants());

    for (std::size_t iteration = 0; iteration < 120U; ++iteration) {
      const auto left = words[random() % words.size()];
      const auto right = words[random() % words.size()];
      std::string query;
      switch (random() % 9U) {
        case 0: query = std::string(left); break;
        case 1: query = "title:" + std::string(left); break;
        case 2: query = std::string(left) + " AND " + std::string(right); break;
        case 3: query = std::string(left) + " OR " + std::string(right); break;
        case 4: query = std::string(left) + " AND NOT " + std::string(right); break;
        case 5: query = "title:\"" + std::string(left) + ' ' + std::string(right) + "\""; break;
        case 6: query = std::string(left) + "^2 OR body:" + std::string(right); break;
        case 7: query = "timestamp:[2022-01-01 TO 2025-12-31]"; break;
        default: query = "(" + std::string(left) + " OR " + std::string(right) +
                         ") AND NOT tags:missing"; break;
      }
      expect_equivalent(index, query, random() % 12U);
    }
  }
}

}  // namespace
