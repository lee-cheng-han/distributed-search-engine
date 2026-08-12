#include "dse/query/lexer.hpp"

#include <cctype>

namespace dse::query {
namespace {

bool ascii_equal_ignore_case(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto left = static_cast<unsigned char>(lhs[index]);
    const auto right = static_cast<unsigned char>(rhs[index]);
    if (std::tolower(left) != std::tolower(right)) return false;
  }
  return true;
}

bool is_delimiter(unsigned char byte) {
  return std::isspace(byte) != 0 || byte == '(' || byte == ')' || byte == ':' || byte == '^' ||
         byte == '[' || byte == ']' || byte == '"' || byte == '*';
}

TokenKind classify_word(std::string_view word) {
  if (ascii_equal_ignore_case(word, "AND")) return TokenKind::and_operator;
  if (ascii_equal_ignore_case(word, "OR")) return TokenKind::or_operator;
  if (ascii_equal_ignore_case(word, "NOT")) return TokenKind::not_operator;
  if (ascii_equal_ignore_case(word, "TO")) return TokenKind::to_operator;
  return TokenKind::word;
}

}  // namespace

std::expected<std::vector<Lexeme>, ParseError> lex(std::string_view input) {
  std::vector<Lexeme> tokens;
  std::size_t cursor = 0;
  while (cursor < input.size()) {
    const auto byte = static_cast<unsigned char>(input[cursor]);
    if (std::isspace(byte) != 0) {
      ++cursor;
      continue;
    }
    const auto position = cursor;
    switch (input[cursor]) {
      case '(':
        tokens.push_back({TokenKind::left_parenthesis, "(", cursor++});
        continue;
      case ')':
        tokens.push_back({TokenKind::right_parenthesis, ")", cursor++});
        continue;
      case ':':
        tokens.push_back({TokenKind::colon, ":", cursor++});
        continue;
      case '^':
        tokens.push_back({TokenKind::caret, "^", cursor++});
        continue;
      case '[':
        tokens.push_back({TokenKind::left_bracket, "[", cursor++});
        continue;
      case ']':
        tokens.push_back({TokenKind::right_bracket, "]", cursor++});
        continue;
      case '*':
        tokens.push_back({TokenKind::star, "*", cursor++});
        continue;
      case '"': {
        ++cursor;
        std::string phrase;
        bool closed = false;
        while (cursor < input.size()) {
          if (input[cursor] == '"') {
            ++cursor;
            closed = true;
            break;
          }
          if (input[cursor] == '\\') {
            if (cursor + 1 >= input.size() ||
                (input[cursor + 1] != '"' && input[cursor + 1] != '\\')) {
              return std::unexpected(ParseError{ParseErrorCode::unexpected_character, cursor,
                                                "only quote and backslash may be escaped"});
            }
            phrase.push_back(input[cursor + 1]);
            cursor += 2;
            continue;
          }
          phrase.push_back(input[cursor++]);
        }
        if (!closed) {
          return std::unexpected(ParseError{ParseErrorCode::unterminated_phrase, position,
                                            "quoted phrase is missing its closing quote"});
        }
        tokens.push_back({TokenKind::phrase, std::move(phrase), position});
        continue;
      }
      default:
        break;
    }

    const auto start = cursor;
    while (cursor < input.size() &&
           !is_delimiter(static_cast<unsigned char>(input[cursor]))) {
      ++cursor;
    }
    if (start == cursor) {
      return std::unexpected(ParseError{ParseErrorCode::unexpected_character, position,
                                        "unexpected character in query"});
    }
    std::string word(input.substr(start, cursor - start));
    tokens.push_back({classify_word(word), std::move(word), start});
  }
  tokens.push_back({TokenKind::end, "", input.size()});
  return tokens;
}

}  // namespace dse::query
