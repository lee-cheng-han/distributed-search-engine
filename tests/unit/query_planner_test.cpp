#include "dse/query/parser.hpp"
#include "dse/query/planner.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <variant>

namespace {

template <typename Node>
const Node& as(const dse::query::PlanNode& plan) {
  return std::get<Node>(plan.value);
}

dse::query::PlannedQuery plan(const dse::index::InMemoryIndex& index, std::string_view text) {
  auto query = dse::query::parse(text);
  EXPECT_TRUE(query.has_value());
  auto planned = dse::query::QueryPlanner(index).plan(**query);
  EXPECT_TRUE(planned.has_value()) << (planned ? "" : planned.error().message);
  return std::move(*planned);
}

TEST(QueryPlanner, ExpandsDefaultFieldsAndAnalyzesExactlyOncePerField) {
  dse::index::InMemoryIndex index;
  const auto planned = plan(index, "Search");
  const auto& disjunction = as<dse::query::PlannedOr>(planned.root());
  ASSERT_EQ(disjunction.children.size(), 3U);
  for (const auto& child : disjunction.children) {
    const auto& term = as<dse::query::PlannedTerm>(*child);
    EXPECT_EQ(term.term, term.field == "tags" ? "Search" : "search");
  }
}

TEST(QueryPlanner, DeduplicatesEquivalentClausesAndCanonicalizesDeterministically) {
  dse::index::InMemoryIndex index;
  const auto duplicate = plan(index, "title:search OR title:search");
  EXPECT_TRUE(std::holds_alternative<dse::query::PlannedTerm>(duplicate.root().value));
  const auto first = plan(index, "title:a OR title:b");
  const auto second = plan(index, "title:b OR title:a");
  EXPECT_EQ(dse::query::canonicalize(first.root()), dse::query::canonicalize(second.root()));
}

TEST(QueryPlanner, OrdersConjunctionsByEstimatedPostingCost) {
  dse::index::InMemoryIndex index;
  ASSERT_TRUE(index.put({.id = dse::DocumentId("a"), .fields = {{"title", "common rare"}}}));
  ASSERT_TRUE(index.put({.id = dse::DocumentId("b"), .fields = {{"title", "common"}}}));
  const auto planned = plan(index, "title:common AND title:rare");
  const auto& conjunction = as<dse::query::PlannedAnd>(planned.root());
  ASSERT_EQ(conjunction.children.size(), 2U);
  EXPECT_EQ(as<dse::query::PlannedTerm>(*conjunction.children[0]).term, "rare");
  EXPECT_LT(conjunction.children[0]->estimated_matches,
            conjunction.children[1]->estimated_matches);
}

TEST(QueryPlanner, ValidatesAndCanonicalizesTypedIntegerRanges) {
  const auto standard = std::make_shared<const dse::analysis::StandardAnalyzer>();
  auto schema = dse::index::IndexSchema::create(
      {{"title", dse::index::FieldType::text, true, true, 1.0, standard},
       {"count", dse::index::FieldType::int64, false, true, 1.0, nullptr}});
  ASSERT_TRUE(schema.has_value());
  dse::index::InMemoryIndex index(std::move(*schema));
  const auto planned = plan(index, "count:[001 TO 010]");
  const auto& range = as<dse::query::PlannedRange>(planned.root());
  EXPECT_EQ(range.lower_bound, "1");
  EXPECT_EQ(range.upper_bound, "10");
}

TEST(QueryPlanner, RejectsUnknownFieldsTypesAndResourceExhaustion) {
  dse::index::InMemoryIndex index;
  auto unknown = dse::query::parse("missing:value");
  ASSERT_TRUE(unknown.has_value());
  auto unknown_plan = dse::query::QueryPlanner(index).plan(**unknown);
  ASSERT_FALSE(unknown_plan.has_value());
  EXPECT_EQ(unknown_plan.error().code, dse::query::PlanningErrorCode::unknown_field);

  auto query = dse::query::parse("a OR b");
  ASSERT_TRUE(query.has_value());
  dse::query::PlannerOptions options;
  options.limits.maximum_nodes = 1;
  auto limited = dse::query::QueryPlanner(index).plan(**query, options);
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(limited.error().code, dse::query::PlanningErrorCode::resource_limit);
}

TEST(QueryPlanner, PreservesPhrasePositionGapsInImmutablePlan) {
  const auto analyzer = std::make_shared<const dse::analysis::StandardAnalyzer>(
      std::set<std::string, std::less<>>{"the"});
  auto schema = dse::index::IndexSchema::create(
      {{"title", dse::index::FieldType::text, true, true, 1.0, analyzer}});
  ASSERT_TRUE(schema.has_value());
  dse::index::InMemoryIndex index(std::move(*schema));
  const auto planned = plan(index, "title:\"distributed the search\"");
  const auto& phrase = as<dse::query::PlannedPhrase>(planned.root());
  ASSERT_EQ(phrase.tokens.size(), 2U);
  EXPECT_EQ(phrase.tokens[0].position, 0U);
  EXPECT_EQ(phrase.tokens[1].position, 2U);
}

}  // namespace
