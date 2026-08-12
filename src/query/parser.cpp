#include "dse/query/parser.hpp"

#include "dse/query/lexer.hpp"

#include <charconv>
#include <cmath>
#include <utility>
#include <vector>

namespace dse::query {
namespace {

constexpr std::size_t kMaximumNestingDepth = 128;

class Parser {
 public:
  explicit Parser(std::vector<Lexeme> tokens) : tokens_(std::move(tokens)) {}

  std::expected<Query, ParseError> run() {
    if (peek().kind == TokenKind::end) {
      return fail(ParseErrorCode::empty_query, peek(), "query is empty");
    }
    auto query = parse_or();
    if (!query) return query;
    if (peek().kind != TokenKind::end) {
      return fail(ParseErrorCode::unexpected_token, peek(), "unexpected token after query");
    }
    return query;
  }

 private:
  const Lexeme& peek(std::size_t offset = 0) const { return tokens_[cursor_ + offset]; }

  const Lexeme& consume() { return tokens_[cursor_++]; }

  std::unexpected<ParseError> fail(ParseErrorCode code, const Lexeme& token,
                                   std::string message) const {
    return std::unexpected(ParseError{code, token.position, std::move(message)});
  }

  static bool begins_primary(TokenKind kind) {
    return kind == TokenKind::word || kind == TokenKind::phrase ||
           kind == TokenKind::left_parenthesis || kind == TokenKind::star;
  }

  std::expected<Query, ParseError> parse_or() {
    auto left = parse_and();
    if (!left) return left;
    while (peek().kind == TokenKind::or_operator || begins_primary(peek().kind)) {
      if (peek().kind == TokenKind::or_operator) consume();
      const auto position = (*left)->position;
      auto right = parse_and();
      if (!right) return right;
      left = make_query<OrQuery>(position, std::move(*left), std::move(*right));
    }
    return left;
  }

  std::expected<Query, ParseError> parse_and() {
    auto left = parse_unary();
    if (!left) return left;
    while (peek().kind == TokenKind::and_operator) {
      consume();
      const auto position = (*left)->position;
      auto right = parse_unary();
      if (!right) return right;
      left = make_query<AndQuery>(position, std::move(*left), std::move(*right));
    }
    return left;
  }

  std::expected<Query, ParseError> parse_unary() {
    if (peek().kind == TokenKind::not_operator) {
      const auto position = consume().position;
      auto operand = parse_unary();
      if (!operand) return operand;
      return make_query<NotQuery>(position, std::move(*operand));
    }
    return parse_postfix();
  }

  std::expected<Query, ParseError> parse_postfix() {
    auto query = parse_primary();
    if (!query) return query;
    while (peek().kind == TokenKind::caret) {
      consume();
      if (peek().kind != TokenKind::word) {
        return fail(ParseErrorCode::invalid_boost, peek(), "boost requires a positive number");
      }
      const auto boost_token = consume();
      double boost = 0.0;
      const auto* begin = boost_token.text.data();
      const auto* end = begin + boost_token.text.size();
      const auto parsed = std::from_chars(begin, end, boost);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(boost) || boost <= 0.0) {
        return fail(ParseErrorCode::invalid_boost, boost_token,
                    "boost must be a finite number greater than zero");
      }
      const auto position = (*query)->position;
      query = make_query<BoostQuery>(position, std::move(*query), boost);
    }
    return query;
  }

  std::expected<Query, ParseError> parse_field_operand() {
    if (peek().kind == TokenKind::not_operator) {
      const auto position = consume().position;
      auto operand = parse_field_operand();
      if (!operand) return operand;
      return make_query<NotQuery>(position, std::move(*operand));
    }
    return parse_primary();
  }

  std::expected<Query, ParseError> parse_primary() {
    const auto token = peek();
    if (token.kind == TokenKind::word) {
      consume();
      if (peek().kind != TokenKind::colon) {
        return make_query<TermQuery>(token.position, token.text);
      }
      consume();
      if (peek().kind == TokenKind::left_bracket) return parse_range(token);
      if (!begins_primary(peek().kind) && peek().kind != TokenKind::not_operator) {
        return fail(ParseErrorCode::expected_expression, peek(),
                    "field name must be followed by a query");
      }
      // Parse without postfix boosts so `field:value^2` boosts the completed
      // field query rather than only its child value.
      auto child = parse_field_operand();
      if (!child) return child;
      return make_query<FieldQuery>(token.position, token.text, std::move(*child));
    }
    if (token.kind == TokenKind::phrase) {
      consume();
      if (token.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return fail(ParseErrorCode::expected_expression, token, "phrase must contain text");
      }
      return make_query<PhraseQuery>(token.position, token.text);
    }
    if (token.kind == TokenKind::star) {
      consume();
      return make_query<MatchAllQuery>(token.position);
    }
    if (token.kind == TokenKind::left_parenthesis) {
      if (depth_ >= kMaximumNestingDepth) {
        return fail(ParseErrorCode::nesting_too_deep, token, "query nesting limit exceeded");
      }
      consume();
      ++depth_;
      auto inner = parse_or();
      --depth_;
      if (!inner) return inner;
      if (peek().kind != TokenKind::right_parenthesis) {
        return fail(ParseErrorCode::unexpected_token, peek(), "expected closing parenthesis");
      }
      consume();
      return inner;
    }
    return fail(ParseErrorCode::expected_expression, token, "expected query expression");
  }

  std::expected<Query, ParseError> parse_range(const Lexeme& field) {
    consume();  // '['
    const auto lower = consume_range_bound();
    if (!lower) return std::unexpected(lower.error());
    if (peek().kind != TokenKind::to_operator) {
      return fail(ParseErrorCode::expected_range_bound, peek(), "range requires TO between bounds");
    }
    consume();
    const auto upper = consume_range_bound();
    if (!upper) return std::unexpected(upper.error());
    if (peek().kind != TokenKind::right_bracket) {
      return fail(ParseErrorCode::unexpected_token, peek(), "range is missing closing bracket");
    }
    consume();
    return make_query<RangeFilter>(field.position, field.text, *lower, *upper, true, true);
  }

  std::expected<std::string, ParseError> consume_range_bound() {
    if (peek().kind != TokenKind::word && peek().kind != TokenKind::star) {
      return fail(ParseErrorCode::expected_range_bound, peek(), "expected range bound");
    }
    return consume().text;
  }

  std::vector<Lexeme> tokens_;
  std::size_t cursor_{};
  std::size_t depth_{};
};

}  // namespace

std::expected<Query, ParseError> parse(std::string_view input) {
  auto tokens = lex(input);
  if (!tokens) return std::unexpected(tokens.error());
  return Parser(std::move(*tokens)).run();
}

}  // namespace dse::query
