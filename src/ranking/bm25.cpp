#include "dse/ranking/bm25.hpp"

#include <cmath>

namespace dse::ranking {

std::string_view describe(BM25Error error) noexcept {
  switch (error) {
    case BM25Error::invalid_parameters:
      return "BM25 requires finite k1 > 0 and b in [0, 1]";
    case BM25Error::inconsistent_statistics:
      return "BM25 corpus statistics are inconsistent";
    case BM25Error::invalid_boost:
      return "BM25 boosts must be finite and non-negative";
  }
  return "unknown BM25 error";
}

std::expected<BM25Scorer, BM25Error> BM25Scorer::create(BM25Parameters parameters) {
  if (!std::isfinite(parameters.k1) || parameters.k1 <= 0.0 ||
      !std::isfinite(parameters.b) || parameters.b < 0.0 || parameters.b > 1.0) {
    return std::unexpected(BM25Error::invalid_parameters);
  }
  return BM25Scorer(parameters);
}

std::expected<double, BM25Error> BM25Scorer::score(const BM25TermStatistics& statistics,
                                                   double field_boost,
                                                   double query_boost) const noexcept {
  if (!std::isfinite(field_boost) || field_boost < 0.0 || !std::isfinite(query_boost) ||
      query_boost < 0.0) {
    return std::unexpected(BM25Error::invalid_boost);
  }
  if (statistics.document_frequency > statistics.document_count ||
      !std::isfinite(statistics.average_document_length) ||
      statistics.average_document_length < 0.0 ||
      (statistics.term_frequency > 0U &&
       (statistics.document_frequency == 0U || statistics.document_count == 0U ||
        statistics.average_document_length <= 0.0 ||
        statistics.term_frequency > statistics.document_length))) {
    return std::unexpected(BM25Error::inconsistent_statistics);
  }
  if (statistics.term_frequency == 0U || statistics.document_frequency == 0U ||
      statistics.document_count == 0U || field_boost == 0.0 || query_boost == 0.0) {
    return 0.0;
  }

  const auto document_count = static_cast<double>(statistics.document_count);
  const auto document_frequency = static_cast<double>(statistics.document_frequency);
  const auto term_frequency = static_cast<double>(statistics.term_frequency);
  const auto document_length = static_cast<double>(statistics.document_length);

  // log1p avoids loss of precision when the ratio is small.
  const double idf = std::log1p((document_count - document_frequency + 0.5) /
                                (document_frequency + 0.5));
  const double length_ratio = document_length / statistics.average_document_length;
  const double normalization =
      parameters_.k1 * (1.0 - parameters_.b + parameters_.b * length_ratio);
  const double frequency_weight =
      term_frequency * (parameters_.k1 + 1.0) / (term_frequency + normalization);
  return idf * frequency_weight * field_boost * query_boost;
}

}  // namespace dse::ranking
