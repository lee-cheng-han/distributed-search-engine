#include "dse/query/lexer.hpp"
#include "dse/query/parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>

namespace {

template <typename Node>
const Node& as(const dse::query::Query& query) {
  return std::get<Node>(query->value);
}

TEST(QueryLexer, RecognizesOperatorsPunctuationAndBytePositions) {
  const auto tokens = dse::query::lex("title:\"distributed systems\"^2 AND year:[2024 TO 2026]");
  ASSERT_TRUE(tokens.has_value());
  ASSERT_EQ(tokens->size(), 14U);
  EXPECT_EQ((*tokens)[0], (dse::query::Lexeme{dse::query::TokenKind::word, "title", 0}));
  EXPECT_EQ((*tokens)[2], (dse::query::Lexeme{dse::query::TokenKind::phrase,
                                               "distributed systems", 6}));
  EXPECT_EQ((*tokens)[5].kind, dse::query::TokenKind::and_operator);
  EXPECT_EQ((*tokens)[10].kind, dse::query::TokenKind::to_operator);
  EXPECT_EQ(tokens->back().kind, dse::query::TokenKind::end);
}

TEST(QueryLexer, UnescapesQuotesAndBackslashesInsidePhrases) {
  const auto tokens = dse::query::lex(R"("quoted \"word\" and \\ path")");
  ASSERT_TRUE(tokens.has_value());
  ASSERT_EQ(tokens->size(), 2U);
  EXPECT_EQ((*tokens)[0].text, "quoted \"word\" and \\ path");
}

TEST(QueryParser, AppliesNotAndOrPrecedence) {
  const auto query = dse::query::parse("gpu OR cpu AND NOT slow");
  ASSERT_TRUE(query.has_value());
  const auto& root = as<dse::query::OrQuery>(*query);
  EXPECT_EQ(as<dse::query::TermQuery>(root.left).term, "gpu");
  const auto& conjunction = as<dse::query::AndQuery>(root.right);
  EXPECT_EQ(as<dse::query::TermQuery>(conjunction.left).term, "cpu");
  const auto& negation = as<dse::query::NotQuery>(conjunction.right);
  EXPECT_EQ(as<dse::query::TermQuery>(negation.operand).term, "slow");
}

TEST(QueryParser, TreatsAdjacentExpressionsAsOr) {
  const auto query = dse::query::parse("distributed systems");
  ASSERT_TRUE(query.has_value());
  const auto& disjunction = as<dse::query::OrQuery>(*query);
  EXPECT_EQ(as<dse::query::TermQuery>(disjunction.left).term, "distributed");
  EXPECT_EQ(as<dse::query::TermQuery>(disjunction.right).term, "systems");
}

TEST(QueryParser, ParenthesesOverridePrecedence) {
  const auto query = dse::query::parse("(gpu OR cpu) AND fast");
  ASSERT_TRUE(query.has_value());
  const auto& conjunction = as<dse::query::AndQuery>(*query);
  EXPECT_TRUE(std::holds_alternative<dse::query::OrQuery>(conjunction.left->value));
  EXPECT_EQ(as<dse::query::TermQuery>(conjunction.right).term, "fast");
}

TEST(QueryParser, ParsesFieldedPhraseAndBoost) {
  const auto query = dse::query::parse("title:\"distributed systems\"^2.5");
  ASSERT_TRUE(query.has_value());
  const auto& boost = as<dse::query::BoostQuery>(*query);
  EXPECT_DOUBLE_EQ(boost.boost, 2.5);
  const auto& field = as<dse::query::FieldQuery>(boost.query);
  EXPECT_EQ(field.field, "title");
  EXPECT_EQ(as<dse::query::PhraseQuery>(field.query).text, "distributed systems");
}

TEST(QueryParser, ParsesInclusiveRangesAndWildcardBounds) {
  const auto query = dse::query::parse("year:[2024 TO *]");
  ASSERT_TRUE(query.has_value());
  const auto& range = as<dse::query::RangeFilter>(*query);
  EXPECT_EQ(range.field, "year");
  EXPECT_EQ(range.lower_bound, "2024");
  EXPECT_EQ(range.upper_bound, "*");
  EXPECT_TRUE(range.include_lower);
  EXPECT_TRUE(range.include_upper);
}

TEST(QueryParser, ParsesMatchAllAndNestedFieldQuery) {
  const auto all = dse::query::parse("*");
  ASSERT_TRUE(all.has_value());
  EXPECT_TRUE(std::holds_alternative<dse::query::MatchAllQuery>((*all)->value));

  const auto fielded = dse::query::parse("title:(search OR engine)");
  ASSERT_TRUE(fielded.has_value());
  const auto& field = as<dse::query::FieldQuery>(*fielded);
  EXPECT_EQ(field.field, "title");
  EXPECT_TRUE(std::holds_alternative<dse::query::OrQuery>(field.query->value));
}

TEST(QueryParser, ReturnsStructuredErrorsWithExactPositions) {
  const auto unterminated = dse::query::parse("title:\"broken");
  ASSERT_FALSE(unterminated.has_value());
  EXPECT_EQ(unterminated.error().code, dse::query::ParseErrorCode::unterminated_phrase);
  EXPECT_EQ(unterminated.error().position, 6U);
  EXPECT_FALSE(unterminated.error().message.empty());

  const auto boost = dse::query::parse("search^zero");
  ASSERT_FALSE(boost.has_value());
  EXPECT_EQ(boost.error().code, dse::query::ParseErrorCode::invalid_boost);
  EXPECT_EQ(boost.error().position, 7U);

  const auto range = dse::query::parse("year:[2024 2026]");
  ASSERT_FALSE(range.has_value());
  EXPECT_EQ(range.error().code, dse::query::ParseErrorCode::expected_range_bound);
  EXPECT_EQ(range.error().position, 11U);
}

TEST(QueryParser, RejectsEmptyExpressionsAndExcessiveNesting) {
  const auto empty = dse::query::parse("  \t ");
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().code, dse::query::ParseErrorCode::empty_query);
  EXPECT_FALSE(dse::query::parse("()"));
  EXPECT_FALSE(dse::query::parse("\"   \""));
  EXPECT_FALSE(dse::query::parse("search^0"));
  EXPECT_FALSE(dse::query::parse("year:[2024 TO]"));

  std::string deeply_nested(129, '(');
  deeply_nested += "term";
  deeply_nested.append(129, ')');
  const auto nested = dse::query::parse(deeply_nested);
  ASSERT_FALSE(nested.has_value());
  EXPECT_EQ(nested.error().code, dse::query::ParseErrorCode::nesting_too_deep);
}

TEST(QueryParser, WhitespaceDoesNotChangeBooleanShape) {
  const auto compact = dse::query::parse("a AND(b OR c)");
  const auto spaced = dse::query::parse(" a  AND ( b OR c ) ");
  ASSERT_TRUE(compact.has_value());
  ASSERT_TRUE(spaced.has_value());
  const auto& compact_and = as<dse::query::AndQuery>(*compact);
  const auto& spaced_and = as<dse::query::AndQuery>(*spaced);
  EXPECT_EQ(as<dse::query::TermQuery>(compact_and.left).term,
            as<dse::query::TermQuery>(spaced_and.left).term);
  EXPECT_TRUE(std::holds_alternative<dse::query::OrQuery>(compact_and.right->value));
  EXPECT_TRUE(std::holds_alternative<dse::query::OrQuery>(spaced_and.right->value));
}

}  // namespace
