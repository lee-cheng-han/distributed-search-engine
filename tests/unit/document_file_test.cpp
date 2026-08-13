#include "dse/index/document_file.hpp"

#include <gtest/gtest.h>

#include <sstream>

namespace {

TEST(DocumentFile, ReadsEscapedDeterministicTsvDocuments) {
  std::istringstream input(
      "document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp\n"
      "doc-1\t2\t0\tTitle\\tValue\tBody\\nLine\tsystems\\\\search\t2026-01-01\n"
      "doc-2\t3\t1\t\t\t\t2026-02-01\n");
  const auto documents = dse::index::read_documents(input);
  ASSERT_TRUE(documents.has_value());
  ASSERT_EQ(documents->size(), 2U);
  EXPECT_EQ((*documents)[0].id, dse::DocumentId("doc-1"));
  EXPECT_EQ((*documents)[0].version, 2U);
  EXPECT_EQ((*documents)[0].fields.at("title"), "Title\tValue");
  EXPECT_EQ((*documents)[0].fields.at("body"), "Body\nLine");
  EXPECT_EQ((*documents)[0].fields.at("tags"), "systems\\search");
  EXPECT_EQ((*documents)[0].stored_metadata.at("timestamp"), "2026-01-01");
  EXPECT_TRUE((*documents)[1].deleted);
}

TEST(DocumentFile, RejectsBadHeaderColumnsEscapesAndScalars) {
  struct Case {
    std::string input;
    dse::index::DocumentFileErrorCode code;
    std::size_t line;
  };
  const std::vector<Case> cases{
      {"wrong\n", dse::index::DocumentFileErrorCode::invalid_header, 1},
      {"document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp\n"
       "id\t1\t0\ttoo\tfew\tcolumns\n",
       dse::index::DocumentFileErrorCode::invalid_column_count, 2},
      {"document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp\n"
       "id\t1\t0\tbad\\q\tbody\ttags\tdate\n",
       dse::index::DocumentFileErrorCode::invalid_escape, 2},
      {"document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp\n"
       "id\tzero\t0\ttitle\tbody\ttags\tdate\n",
       dse::index::DocumentFileErrorCode::invalid_version, 2},
      {"document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp\n"
       "id\t1\tyes\ttitle\tbody\ttags\tdate\n",
       dse::index::DocumentFileErrorCode::invalid_deleted, 2},
      {"document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp\n"
       "\t1\t0\ttitle\tbody\ttags\tdate\n",
       dse::index::DocumentFileErrorCode::empty_document_id, 2},
  };
  for (const auto& test_case : cases) {
    std::istringstream input(test_case.input);
    const auto result = dse::index::read_documents(input);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, test_case.code);
    EXPECT_EQ(result.error().line, test_case.line);
    EXPECT_FALSE(result.error().message.empty());
  }
}

TEST(DocumentFile, ReportsMissingPath) {
  const auto result = dse::index::read_documents("definitely-not-a-real-document-file.tsv");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, dse::index::DocumentFileErrorCode::open_failed);
}

}  // namespace
