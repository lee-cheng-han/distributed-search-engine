#pragma once

#include <cstddef>
#include <string>

namespace dse::query {

enum class ParseErrorCode {
  empty_query,
  unexpected_character,
  unterminated_phrase,
  expected_expression,
  unexpected_token,
  expected_range_bound,
  invalid_boost,
  nesting_too_deep,
  resource_limit,
};

struct ParseError {
  ParseErrorCode code;
  std::size_t position;
  std::string message;
  auto operator<=>(const ParseError&) const = default;
};

}  // namespace dse::query
