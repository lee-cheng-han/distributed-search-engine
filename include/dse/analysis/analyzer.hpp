#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dse::analysis {

struct Token {
  std::string term;
  std::uint32_t position;
  std::uint32_t start_offset;
  std::uint32_t end_offset;
  auto operator<=>(const Token&) const = default;
};

class Analyzer {
 public:
  virtual ~Analyzer() = default;
  [[nodiscard]] virtual std::vector<Token> analyze(std::string_view text) const = 0;
};

class StandardAnalyzer final : public Analyzer {
 public:
  explicit StandardAnalyzer(std::set<std::string, std::less<>> stop_words = {});
  [[nodiscard]] std::vector<Token> analyze(std::string_view text) const override;

 private:
  std::set<std::string, std::less<>> stop_words_;
};

class KeywordAnalyzer final : public Analyzer {
 public:
  [[nodiscard]] std::vector<Token> analyze(std::string_view text) const override;
};

}  // namespace dse::analysis
