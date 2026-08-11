#pragma once

#include "dse/types.hpp"

#include <cstddef>
#include <queue>
#include <vector>

namespace dse::ranking {

struct ScoredDocument {
  DocumentId document_id;
  double score{};
};

// Defines the public result order: higher scores first, then smaller IDs.
struct BetterScore {
  [[nodiscard]] bool operator()(const ScoredDocument& lhs,
                                const ScoredDocument& rhs) const noexcept;
};

class TopKCollector {
 public:
  explicit TopKCollector(std::size_t capacity) : capacity_(capacity) {}

  // Returns false for non-finite scores. Zero-capacity collectors accept no values.
  bool collect(ScoredDocument candidate);
  [[nodiscard]] std::vector<ScoredDocument> results() const;
  [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  std::size_t capacity_;
  std::priority_queue<ScoredDocument, std::vector<ScoredDocument>, BetterScore> heap_;
};

}  // namespace dse::ranking
