#pragma once

#include "dse/index/in_memory_index.hpp"
#include "dse/query/executor.hpp"
#include "dse/query/plan.hpp"
#include "dse/ranking/bm25.hpp"

#include <cstddef>

namespace dse::test {

// Deliberately slow document-at-a-time oracle. It scans and reanalyzes source documents and never
// consults posting lists, making it independent of the optimized executor's merge algorithms.
class ReferenceEvaluator {
 public:
  explicit ReferenceEvaluator(const index::SearchIndexView& index,
                              ranking::BM25Parameters parameters = {});

  [[nodiscard]] query::SearchResult search(const query::PlannedQuery& query,
                                            std::size_t top_k) const;

 private:
  const index::SearchIndexView& index_;
  ranking::BM25Parameters parameters_;
};

}  // namespace dse::test
