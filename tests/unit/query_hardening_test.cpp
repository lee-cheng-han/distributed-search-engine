#include "dse/query/executor.hpp"
#include "dse/query/lexer.hpp"
#include "dse/query/parser.hpp"
#include "dse/query/planner.hpp"

#include <gtest/gtest.h>

#include <array>
#include <random>
#include <string>
#include <string_view>

namespace {

TEST(QueryHardening, EnforcesEverySyntacticResourceBoundary) {
  dse::query::QueryLimits limits{.maximum_query_bytes = 8,
                                 .maximum_lexemes = 3,
                                 .maximum_lexeme_bytes = 4,
                                 .maximum_nesting_depth = 2};
  const auto bytes = dse::query::parse("123456789", limits);
  ASSERT_FALSE(bytes.has_value());
  EXPECT_EQ(bytes.error().code, dse::query::ParseErrorCode::resource_limit);

  const auto lexeme_size = dse::query::parse("abcde", limits);
  ASSERT_FALSE(lexeme_size.has_value());
  EXPECT_EQ(lexeme_size.error().code, dse::query::ParseErrorCode::resource_limit);

  const auto lexemes = dse::query::parse("a b c d", limits);
  ASSERT_FALSE(lexemes.has_value());
  EXPECT_EQ(lexemes.error().code, dse::query::ParseErrorCode::resource_limit);

  auto nesting_limits = limits;
  nesting_limits.maximum_query_bytes = 16;
  nesting_limits.maximum_lexemes = 16;
  const auto nesting = dse::query::parse("(((a)))", nesting_limits);
  ASSERT_FALSE(nesting.has_value());
  EXPECT_EQ(nesting.error().code, dse::query::ParseErrorCode::nesting_too_deep);

  for (auto* member : {&limits.maximum_query_bytes, &limits.maximum_lexemes,
                       &limits.maximum_lexeme_bytes, &limits.maximum_nesting_depth}) {
    const auto saved = *member;
    *member = 0;
    const auto invalid = dse::query::parse("a", limits);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, dse::query::ParseErrorCode::resource_limit);
    *member = saved;
  }
}

TEST(QueryHardening, BoundaryValuesAreAcceptedWithoutOffByOneErrors) {
  const dse::query::QueryLimits limits{.maximum_query_bytes = 5,
                                       .maximum_lexemes = 1,
                                       .maximum_lexeme_bytes = 5,
                                       .maximum_nesting_depth = 1};
  const auto parsed = dse::query::parse("abcde", limits);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(std::get<dse::query::TermQuery>((*parsed)->value).term, "abcde");
}

TEST(QueryHardening, SeededArbitraryBytesNeverEscapeTheBoundedPipeline) {
  std::mt19937 random(0xD15EA5EU);
  dse::index::InMemoryIndex index;
  for (std::size_t iteration = 0; iteration < 10'000U; ++iteration) {
    std::string input(random() % 256U, '\0');
    for (auto& byte : input) byte = static_cast<char>(random() & 0xFFU);
    auto parsed = dse::query::parse(input);
    if (!parsed) continue;
    auto first = dse::query::QueryPlanner(index).plan(**parsed);
    if (!first) continue;
    auto second_ast = dse::query::parse(input);
    ASSERT_TRUE(second_ast.has_value());
    auto second = dse::query::QueryPlanner(index).plan(**second_ast);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(dse::query::canonicalize(first->root()),
              dse::query::canonicalize(second->root()));
    EXPECT_TRUE(dse::query::QueryExecutor(index).search(*first, iteration % 17U).has_value());
  }
}

TEST(QueryHardening, AdversarialFlatAndNestedQueriesFailDeterministically) {
  std::string flat = "a";
  for (std::size_t i = 0; i < 3'000U; ++i) flat += " OR a";
  const auto flat_result = dse::query::parse(flat);
  ASSERT_FALSE(flat_result.has_value());
  EXPECT_EQ(flat_result.error().code, dse::query::ParseErrorCode::resource_limit);

  std::string nested(129, '(');
  nested += 'a';
  nested.append(129, ')');
  const auto nested_result = dse::query::parse(nested);
  ASSERT_FALSE(nested_result.has_value());
  EXPECT_EQ(nested_result.error().code, dse::query::ParseErrorCode::nesting_too_deep);

  constexpr std::array malformed{"\"unterminated", "a^1e9999", "field:[a TO ]",
                                  "((((", "a AND AND b"};
  for (const std::string_view input : malformed) {
    const auto first = dse::query::parse(input);
    const auto second = dse::query::parse(input);
    ASSERT_FALSE(first.has_value()) << input;
    ASSERT_FALSE(second.has_value()) << input;
    EXPECT_EQ(first.error(), second.error()) << input;
  }
}

}  // namespace
