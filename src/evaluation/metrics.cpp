#include "dse/evaluation/metrics.hpp"

#include <algorithm>
#include <cmath>

namespace dse::evaluation {
RankingMetrics evaluate(std::span<const DocumentId> ranked,
                        const RelevanceJudgments& judgments, std::size_t k) {
  const auto relevant = static_cast<std::size_t>(std::ranges::count_if(
      judgments, [](const auto& judgment) { return judgment.second != 0U; }));
  const auto limit = std::min(k, ranked.size());
  std::size_t found{}; double precision_sum{}; double reciprocal{}; double dcg{};
  for (std::size_t index = 0; index < ranked.size(); ++index) {
    const auto judgment = judgments.find(ranked[index]);
    const unsigned grade = judgment == judgments.end() ? 0U : judgment->second;
    if (grade != 0U) {
      ++found; precision_sum += static_cast<double>(found) / static_cast<double>(index + 1U);
      if (reciprocal == 0.0) reciprocal = 1.0 / static_cast<double>(index + 1U);
    }
    if (index < limit) dcg += (std::exp2(static_cast<double>(grade)) - 1.0) /
                              std::log2(static_cast<double>(index) + 2.0);
  }
  std::vector<unsigned> ideal; ideal.reserve(judgments.size());
  for (const auto& [id, grade] : judgments) { (void)id; ideal.push_back(grade); }
  std::ranges::sort(ideal, std::greater<>{}); double ideal_dcg{};
  for (std::size_t index = 0; index < std::min(limit, ideal.size()); ++index)
    ideal_dcg += (std::exp2(static_cast<double>(ideal[index])) - 1.0) /
                 std::log2(static_cast<double>(index) + 2.0);
  const auto found_at_k = static_cast<std::size_t>(std::ranges::count_if(
      ranked.first(limit), [&](const DocumentId& id) { const auto item = judgments.find(id); return item != judgments.end() && item->second != 0U; }));
  return {.precision_at_k = k == 0U ? 0.0 : static_cast<double>(found_at_k) / static_cast<double>(k),
          .recall_at_k = relevant == 0U ? 0.0 : static_cast<double>(found_at_k) / static_cast<double>(relevant),
          .average_precision = relevant == 0U ? 0.0 : precision_sum / static_cast<double>(relevant),
          .reciprocal_rank = reciprocal,
          .ndcg_at_k = ideal_dcg == 0.0 ? 0.0 : dcg / ideal_dcg};
}
}  // namespace dse::evaluation
