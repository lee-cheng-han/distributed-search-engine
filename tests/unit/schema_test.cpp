#include "dse/index/in_memory_index.hpp"
#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace {

TEST(IndexSchema, OwnsPerFieldAnalyzersAndValidatesDefinitions) {
  const auto standard = std::make_shared<const dse::analysis::StandardAnalyzer>();
  const auto keyword = std::make_shared<const dse::analysis::KeywordAnalyzer>();
  auto schema = dse::index::IndexSchema::create(
      {{"text", dse::index::FieldType::text, true, true, 2.0, standard},
       {"tag", dse::index::FieldType::keyword, true, true, 1.0, keyword},
       {"date", dse::index::FieldType::timestamp, false, true, 1.0, nullptr}});
  ASSERT_TRUE(schema.has_value());
  EXPECT_EQ(schema->find("text")->analyzer, standard);
  EXPECT_EQ(schema->find("tag")->analyzer, keyword);
  EXPECT_EQ(schema->find("date")->type, dse::index::FieldType::timestamp);

  EXPECT_FALSE(dse::index::IndexSchema::create(
      {{"text", dse::index::FieldType::text, true, true, 1.0, nullptr}}));
  EXPECT_FALSE(dse::index::IndexSchema::create(
      {{"date", dse::index::FieldType::timestamp, false, true, 1.0, standard}}));
  EXPECT_FALSE(dse::index::IndexSchema::create(
      {{"same", dse::index::FieldType::text, true, true, 1.0, standard},
       {"same", dse::index::FieldType::text, true, true, 1.0, standard}}));
}

TEST(IndexSchema, ValidatesIntegerAndCalendarDateValues) {
  const auto standard = std::make_shared<const dse::analysis::StandardAnalyzer>();
  auto schema = dse::index::IndexSchema::create(
      {{"number", dse::index::FieldType::int64, false, true, 1.0, nullptr},
       {"date", dse::index::FieldType::timestamp, false, true, 1.0, nullptr},
       {"text", dse::index::FieldType::text, true, true, 1.0, standard}});
  ASSERT_TRUE(schema.has_value());
  EXPECT_TRUE(schema->validate_value("number", "-9223372036854775808"));
  EXPECT_FALSE(schema->validate_value("number", "12.5"));
  EXPECT_TRUE(schema->validate_value("date", "2024-02-29"));
  EXPECT_FALSE(schema->validate_value("date", "2023-02-29"));
  EXPECT_FALSE(schema->validate_value("missing", "value"));
}

TEST(IndexSchema, IndexesTextAndKeywordFieldsWithDifferentAnalysis) {
  dse::index::InMemoryIndex index;
  dse::Document document{.id = dse::DocumentId("doc"),
                         .fields = {{"body", "Systems Search"},
                                    {"tags", "Systems Search"},
                                    {"title", "Schema Test"}},
                         .stored_metadata = {{"timestamp", "2026-08-13"}}};
  ASSERT_TRUE(index.put(std::move(document)));
  EXPECT_NE(index.lookup("body", "systems"), nullptr);
  EXPECT_EQ(index.lookup("tags", "systems"), nullptr);
  EXPECT_NE(index.lookup("tags", "Systems Search"), nullptr);
}

TEST(IndexSchema, RejectsUnknownAndInvalidTypedDocumentFields) {
  dse::index::InMemoryIndex index;
  auto unknown = index.put({.id = dse::DocumentId("unknown"),
                            .fields = {{"mystery", "value"}}});
  ASSERT_FALSE(unknown.has_value());
  EXPECT_EQ(unknown.error().code, dse::index::IndexErrorCode::schema_error);
  ASSERT_TRUE(unknown.error().schema_error.has_value());
  EXPECT_EQ(unknown.error().schema_error->code, dse::index::SchemaErrorCode::unknown_field);

  auto invalid_date = index.put({.id = dse::DocumentId("date"),
                                 .stored_metadata = {{"timestamp", "2026-02-30"}}});
  ASSERT_FALSE(invalid_date.has_value());
  EXPECT_EQ(invalid_date.error().schema_error->code,
            dse::index::SchemaErrorCode::invalid_typed_value);
}

TEST(IndexSchema, RejectsUnknownQueryFieldsAndTextRanges) {
  dse::index::InMemoryIndex index;
  ASSERT_TRUE(index.put({.id = dse::DocumentId("doc"),
                         .fields = {{"body", "search"}, {"title", "engine"}},
                         .stored_metadata = {{"timestamp", "2026-08-13"}}}));
  const dse::query::QueryExecutor executor(index);

  auto unknown_query = dse::query::parse("missing:search");
  ASSERT_TRUE(unknown_query.has_value());
  auto unknown_result = executor.search(**unknown_query);
  ASSERT_FALSE(unknown_result.has_value());
  EXPECT_EQ(unknown_result.error().code, dse::query::ExecutionErrorCode::unknown_field);

  auto text_range = dse::query::parse("title:[a TO z]");
  ASSERT_TRUE(text_range.has_value());
  auto range_result = executor.search(**text_range);
  ASSERT_FALSE(range_result.has_value());
  EXPECT_EQ(range_result.error().code,
            dse::query::ExecutionErrorCode::incompatible_field_type);
}

}  // namespace
