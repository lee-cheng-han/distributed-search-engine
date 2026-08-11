#include "dse/analysis/analyzer.hpp"

#include <cctype>
#include <limits>
#include <stdexcept>

namespace dse::analysis {
namespace {

std::uint32_t checked_offset(std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("analyzer input exceeds uint32 offset range");
  }
  return static_cast<std::uint32_t>(value);
}

std::string ascii_lower(std::string_view value) {
  std::string result(value);
  for (char& ch : result) {
    const auto byte = static_cast<unsigned char>(ch);
    if (byte < 0x80U) ch = static_cast<char>(std::tolower(byte));
  }
  return result;
}

bool is_token_byte(unsigned char byte) {
  return byte >= 0x80U || std::isalnum(byte) != 0;
}

}  // namespace

StandardAnalyzer::StandardAnalyzer(std::set<std::string, std::less<>> stop_words) {
  for (const auto& word : stop_words) stop_words_.insert(ascii_lower(word));
}

std::vector<Token> StandardAnalyzer::analyze(std::string_view text) const {
  std::vector<Token> tokens;
  std::uint32_t position = 0;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    while (cursor < text.size() && !is_token_byte(static_cast<unsigned char>(text[cursor]))) ++cursor;
    const auto start = cursor;
    while (cursor < text.size() && is_token_byte(static_cast<unsigned char>(text[cursor]))) ++cursor;
    if (start == cursor) break;
    auto term = ascii_lower(text.substr(start, cursor - start));
    const auto current_position = position++;
    if (!stop_words_.contains(term)) {
      tokens.push_back({std::move(term), current_position, checked_offset(start), checked_offset(cursor)});
    }
  }
  return tokens;
}

std::vector<Token> KeywordAnalyzer::analyze(std::string_view text) const {
  if (text.empty()) return {};
  return {{std::string(text), 0, 0, checked_offset(text.size())}};
}

}  // namespace dse::analysis
