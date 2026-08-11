#include "dse/ranking/top_k.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dse::ranking {

bool BetterScore::operator()(const ScoredDocument& lhs,
                             const ScoredDocument& rhs) const noexcept {
  if (lhs.score != rhs.score) return lhs.score > rhs.score;
  return lhs.document_id < rhs.document_id;
}

bool TopKCollector::collect(ScoredDocument candidate) {
  if (!std::isfinite(candidate.score)) return false;
  if (capacity_ == 0U) return true;
  if (heap_.size() < capacity_) {
    heap_.push(std::move(candidate));
    return true;
  }
  if (BetterScore{}(candidate, heap_.top())) {
    heap_.pop();
    heap_.push(std::move(candidate));
  }
  return true;
}

std::vector<ScoredDocument> TopKCollector::results() const {
  auto heap_copy = heap_;
  std::vector<ScoredDocument> ordered;
  ordered.reserve(heap_copy.size());
  while (!heap_copy.empty()) {
    ordered.push_back(heap_copy.top());
    heap_copy.pop();
  }
  std::ranges::sort(ordered, BetterScore{});
  return ordered;
}

}  // namespace dse::ranking
