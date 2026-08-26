#pragma once

#include "dse/query/error.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace dse::query {

struct QueryLimits {
  std::size_t maximum_query_bytes{16U * 1024U};
  std::size_t maximum_lexemes{2'048};
  std::size_t maximum_lexeme_bytes{4U * 1024U};
  std::size_t maximum_nesting_depth{128};
};

enum class TokenKind {
  word,
  phrase,
  and_operator,
  or_operator,
  not_operator,
  to_operator,
  left_parenthesis,
  right_parenthesis,
  colon,
  caret,
  left_bracket,
  right_bracket,
  star,
  end,
};

struct Lexeme {
  TokenKind kind;
  std::string text;
  std::size_t position;
  auto operator<=>(const Lexeme&) const = default;
};

[[nodiscard]] std::expected<std::vector<Lexeme>, ParseError> lex(
    std::string_view input, const QueryLimits& limits = {});

}  // namespace dse::query
