#pragma once

#include "dse/query/error.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace dse::query {

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

[[nodiscard]] std::expected<std::vector<Lexeme>, ParseError> lex(std::string_view input);

}  // namespace dse::query
