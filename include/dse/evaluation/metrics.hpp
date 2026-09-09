#pragma once

#include "dse/types.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <vector>

namespace dse::evaluation {

using RelevanceJudgments = std::map<DocumentId, unsigned>;
struct RankingMetrics {
  double precision_at_k{};
  double recall_at_k{};
  double average_precision{};
  double reciprocal_rank{};
  double ndcg_at_k{};
};

[[nodiscard]] RankingMetrics evaluate(std::span<const DocumentId> ranked,
                                      const RelevanceJudgments& judgments,
                                      std::size_t k);

}  // namespace dse::evaluation
