#include "dse/evaluation/metrics.hpp"

#include <gtest/gtest.h>

#include <array>

TEST(EvaluationMetrics, ComputesPrecisionRecallMapMrrAndNdcg) {
  const std::array ranked{dse::DocumentId("a"), dse::DocumentId("b"),
                          dse::DocumentId("c"), dse::DocumentId("d")};
  const dse::evaluation::RelevanceJudgments judgments{
      {dse::DocumentId("a"), 3}, {dse::DocumentId("c"), 1}, {dse::DocumentId("x"), 2}};
  const auto result = dse::evaluation::evaluate(ranked, judgments, 3);
  EXPECT_DOUBLE_EQ(result.precision_at_k, 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(result.recall_at_k, 2.0 / 3.0);
  EXPECT_NEAR(result.average_precision, (1.0 + 2.0 / 3.0) / 3.0, 1e-12);
  EXPECT_DOUBLE_EQ(result.reciprocal_rank, 1.0);
  EXPECT_GT(result.ndcg_at_k, 0.0); EXPECT_LE(result.ndcg_at_k, 1.0);
}

TEST(EvaluationMetrics, HandlesEmptyRankingsAndJudgments) {
  EXPECT_EQ(dse::evaluation::evaluate({}, {}, 10).recall_at_k, 0.0);
}
