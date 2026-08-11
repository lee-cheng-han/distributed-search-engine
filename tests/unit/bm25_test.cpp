#include "dse/ranking/bm25.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace {

TEST(BM25Scorer, MatchesHandCalculatedExample) {
  const auto scorer = dse::ranking::BM25Scorer::create();
  ASSERT_TRUE(scorer.has_value());
  const auto score = scorer->score({.document_count = 100,
                                    .document_frequency = 10,
                                    .term_frequency = 3,
                                    .document_length = 120,
                                    .average_document_length = 100.0});
  ASSERT_TRUE(score.has_value());
  EXPECT_NEAR(*score, 3.411122994035014, 1e-12);
}

TEST(BM25Scorer, AppliesFieldAndQueryBoostsAfterTermScore) {
  const auto scorer = dse::ranking::BM25Scorer::create();
  ASSERT_TRUE(scorer.has_value());
  const dse::ranking::BM25TermStatistics statistics{.document_count = 12,
                                                    .document_frequency = 4,
                                                    .term_frequency = 2,
                                                    .document_length = 8,
                                                    .average_document_length = 10.0};
  const auto base = scorer->score(statistics);
  const auto boosted = scorer->score(statistics, 2.0, 3.0);
  ASSERT_TRUE(base.has_value());
  ASSERT_TRUE(boosted.has_value());
  EXPECT_DOUBLE_EQ(*boosted, *base * 6.0);
}

TEST(BM25Scorer, ReturnsZeroForAbsentTermsAndEmptyCorpora) {
  const auto scorer = dse::ranking::BM25Scorer::create();
  ASSERT_TRUE(scorer.has_value());
  const auto absent = scorer->score({.document_count = 5,
                                     .document_frequency = 2,
                                     .term_frequency = 0,
                                     .document_length = 0,
                                     .average_document_length = 0.0});
  const auto empty = scorer->score({});
  ASSERT_TRUE(absent.has_value());
  ASSERT_TRUE(empty.has_value());
  EXPECT_DOUBLE_EQ(*absent, 0.0);
  EXPECT_DOUBLE_EQ(*empty, 0.0);
}

TEST(BM25Scorer, RejectsInvalidParametersStatisticsAndBoosts) {
  EXPECT_EQ(dse::ranking::BM25Scorer::create({.k1 = 0.0, .b = 0.75}).error(),
            dse::ranking::BM25Error::invalid_parameters);
  EXPECT_EQ(dse::ranking::BM25Scorer::create({.k1 = 1.2, .b = 1.1}).error(),
            dse::ranking::BM25Error::invalid_parameters);
  const auto scorer = dse::ranking::BM25Scorer::create();
  ASSERT_TRUE(scorer.has_value());
  const auto inconsistent = scorer->score({.document_count = 2,
                                           .document_frequency = 3,
                                           .term_frequency = 1,
                                           .document_length = 1,
                                           .average_document_length = 1.0});
  ASSERT_FALSE(inconsistent.has_value());
  EXPECT_EQ(inconsistent.error(), dse::ranking::BM25Error::inconsistent_statistics);
  const auto invalid_boost = scorer->score({}, std::numeric_limits<double>::infinity());
  ASSERT_FALSE(invalid_boost.has_value());
  EXPECT_EQ(invalid_boost.error(), dse::ranking::BM25Error::invalid_boost);
}

TEST(BM25Scorer, ConfigurableParametersChangeLengthNormalization) {
  const auto no_length_normalization =
      dse::ranking::BM25Scorer::create({.k1 = 1.2, .b = 0.0});
  const auto full_length_normalization =
      dse::ranking::BM25Scorer::create({.k1 = 1.2, .b = 1.0});
  ASSERT_TRUE(no_length_normalization.has_value());
  ASSERT_TRUE(full_length_normalization.has_value());
  const dse::ranking::BM25TermStatistics statistics{.document_count = 20,
                                                    .document_frequency = 2,
                                                    .term_frequency = 1,
                                                    .document_length = 20,
                                                    .average_document_length = 10.0};
  EXPECT_GT(*no_length_normalization->score(statistics),
            *full_length_normalization->score(statistics));
}

}  // namespace
