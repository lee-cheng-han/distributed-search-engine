#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace dse::ranking {

struct BM25Parameters {
  double k1{1.2};
  double b{0.75};
};

struct BM25TermStatistics {
  std::uint64_t document_count{};
  std::uint64_t document_frequency{};
  std::uint32_t term_frequency{};
  std::uint32_t document_length{};
  double average_document_length{};
};

enum class BM25Error {
  invalid_parameters,
  inconsistent_statistics,
  invalid_boost,
};

[[nodiscard]] std::string_view describe(BM25Error error) noexcept;

class BM25Scorer {
 public:
  [[nodiscard]] static std::expected<BM25Scorer, BM25Error> create(
      BM25Parameters parameters = {});

  [[nodiscard]] std::expected<double, BM25Error> score(
      const BM25TermStatistics& statistics, double field_boost = 1.0,
      double query_boost = 1.0) const noexcept;
  [[nodiscard]] BM25Parameters parameters() const noexcept { return parameters_; }

 private:
  explicit BM25Scorer(BM25Parameters parameters) : parameters_(parameters) {}
  BM25Parameters parameters_;
};

}  // namespace dse::ranking
