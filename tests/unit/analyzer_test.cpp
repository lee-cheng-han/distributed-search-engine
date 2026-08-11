#include "dse/analysis/analyzer.hpp"

#include <gtest/gtest.h>

namespace {

TEST(StandardAnalyzer, LowercasesSplitsAndTracksOffsets) {
  const dse::analysis::StandardAnalyzer analyzer;
  const auto tokens = analyzer.analyze("Hello, WORLD 42!");
  ASSERT_EQ(tokens.size(), 3U);
  EXPECT_EQ(tokens[0], (dse::analysis::Token{"hello", 0, 0, 5}));
  EXPECT_EQ(tokens[1], (dse::analysis::Token{"world", 1, 7, 12}));
  EXPECT_EQ(tokens[2], (dse::analysis::Token{"42", 2, 13, 15}));
}

TEST(StandardAnalyzer, StopWordsLeavePositionGapsForPhraseCorrectness) {
  const dse::analysis::StandardAnalyzer analyzer({"the"});
  const auto tokens = analyzer.analyze("search the systems");
  ASSERT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].position, 0U);
  EXPECT_EQ(tokens[1].position, 2U);
}

TEST(StandardAnalyzer, KeepsUtf8SequencesIntactWithDocumentedByteOffsets) {
  const dse::analysis::StandardAnalyzer analyzer;
  const auto tokens = analyzer.analyze("Caf\xC3\xA9 search");
  ASSERT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].term, "caf\xC3\xA9");
  EXPECT_EQ(tokens[0].end_offset, 5U);
}

TEST(KeywordAnalyzer, EmitsEntireNonEmptyValueVerbatim) {
  const dse::analysis::KeywordAnalyzer analyzer;
  EXPECT_EQ(analyzer.analyze("Distributed Systems"),
            (std::vector<dse::analysis::Token>{{"Distributed Systems", 0, 0, 19}}));
  EXPECT_TRUE(analyzer.analyze("").empty());
}

}  // namespace
