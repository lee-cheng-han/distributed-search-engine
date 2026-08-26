#include "dse/storage/segment.hpp"
#include "../support/reference_evaluator.hpp"

#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"
#include "dse/query/planner.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace {

class TemporarySegment {
 public:
  TemporarySegment() {
    static std::atomic_uint64_t next{};
    path_ = std::filesystem::temp_directory_path() /
            ("dse-segment-test-" + std::to_string(++next) + ".dseg");
  }
  ~TemporarySegment() { std::error_code ignored; std::filesystem::remove(path_, ignored); std::filesystem::remove(path_.string() + ".tmp", ignored); }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
 private:
  std::filesystem::path path_;
};

class UnsupportedAnalyzer final : public dse::analysis::Analyzer {
 public:
  std::vector<dse::analysis::Token> analyze(std::string_view text) const override {
    return text.empty() ? std::vector<dse::analysis::Token>{}
                        : std::vector<dse::analysis::Token>{{std::string(text), 0, 0,
                                                             static_cast<std::uint32_t>(text.size())}};
  }
};

dse::index::InMemoryIndex populated_index() {
  const auto standard = std::make_shared<const dse::analysis::StandardAnalyzer>(
      std::set<std::string, std::less<>>{"the"});
  const auto keyword = std::make_shared<const dse::analysis::KeywordAnalyzer>();
  auto schema = dse::index::IndexSchema::create(
      {{"body", dse::index::FieldType::text, true, true, 1.0, standard},
       {"count", dse::index::FieldType::int64, false, true, 1.0, nullptr},
       {"tags", dse::index::FieldType::keyword, true, true, 1.5, keyword},
       {"timestamp", dse::index::FieldType::timestamp, false, true, 1.0, nullptr},
       {"title", dse::index::FieldType::text, true, true, 2.0, standard}});
  EXPECT_TRUE(schema.has_value());
  dse::index::InMemoryIndex index(std::move(*schema));
  EXPECT_TRUE(index.put({.id = dse::DocumentId("a"),
                         .fields = {{"body", "distributed the search search"},
                                    {"tags", "Systems"}, {"title", "Distributed Search"}},
                         .stored_metadata = {{"count", "10"}, {"timestamp", "2025-01-02"}},
                         .version = 1}));
  EXPECT_TRUE(index.put({.id = dse::DocumentId("b"),
                         .fields = {{"body", ""}, {"tags", "empty"}, {"title", ""}},
                         .stored_metadata = {{"count", "-2"}, {"timestamp", "2024-02-29"}},
                         .version = 1}));
  EXPECT_TRUE(index.put({.id = dse::DocumentId("c"),
                         .fields = {{"body", "obsolete"}, {"tags", "old"}, {"title", "old"}},
                         .stored_metadata = {{"count", "1"}, {"timestamp", "2023-01-01"}},
                         .version = 1}));
  EXPECT_TRUE(index.erase(dse::DocumentId("c"), 2));
  return index;
}

dse::query::SearchResult search(const dse::index::SearchIndexView& index,
                                std::string_view query, std::size_t top_k) {
  auto parsed = dse::query::parse(query);
  EXPECT_TRUE(parsed.has_value());
  auto result = dse::query::QueryExecutor(index).search(
      **parsed, {.top_k = top_k,
                 .default_fields = {{"title", 1.0}, {"body", 1.0}, {"tags", 1.0}},
                 .planner_limits = {}});
  EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
  return result ? std::move(*result) : dse::query::SearchResult{};
}

void expect_same(const dse::query::SearchResult& left, const dse::query::SearchResult& right) {
  ASSERT_EQ(left.total_hits, right.total_hits);
  ASSERT_EQ(left.hits.size(), right.hits.size());
  for (std::size_t i = 0; i < left.hits.size(); ++i) {
    EXPECT_EQ(left.hits[i].document_id, right.hits[i].document_id);
    EXPECT_NEAR(left.hits[i].score, right.hits[i].score, 1e-12);
  }
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto size = input.tellg(); input.seekg(0);
  std::vector<std::byte> result(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(result.data()), size); return result;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t crc32c(std::span<const std::byte> bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const auto item : bytes) {
    crc ^= static_cast<std::uint8_t>(item);
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return ~crc;
}

void set_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index)
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

void repair_checksum(std::vector<std::byte>& bytes) {
  set_u32(bytes, 40, crc32c(std::span<const std::byte>(bytes).subspan(64)));
}

TEST(Segment, ReopensWithoutSourceIndexAndPreservesSearchSemantics) {
  TemporarySegment file;
  std::vector<std::pair<std::string, dse::query::SearchResult>> expected;
  {
    auto index = populated_index();
    for (const auto query : {"search", "title:\"distributed search\"", "tags:Systems",
                             "count:[-5 TO 10]", "search AND NOT tags:empty", "*", "---"}) {
      expected.emplace_back(query, search(index, query, 10));
    }
    ASSERT_TRUE(dse::storage::SegmentWriter::write(
        file.path(), index.snapshot(), {.segment_id = dse::SegmentId(42)}));
  }
  auto reader = dse::storage::SegmentReader::open(file.path());
  ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().message);
  EXPECT_EQ(reader->segment_id(), dse::SegmentId(42));
  EXPECT_EQ(reader->live_document_count(), 2U);
  EXPECT_TRUE(reader->document(dse::DocumentId("c"))->document.deleted);
  EXPECT_EQ(reader->schema().find("title")->analyzer->descriptor(), "standard-v1:1:3:the");
  for (const auto& [query, result] : expected) {
    const auto reopened = search(*reader, query, 10);
    expect_same(result, reopened);
    auto parsed = dse::query::parse(query);
    ASSERT_TRUE(parsed.has_value());
    auto planned = dse::query::QueryPlanner(*reader).plan(**parsed);
    ASSERT_TRUE(planned.has_value());
    expect_same(reopened, dse::test::ReferenceEvaluator(*reader).search(*planned, 10));
  }
}

TEST(Segment, RejectsTruncationChecksumDamageVersionsAndTightLimits) {
  TemporarySegment original;
  auto index = populated_index();
  ASSERT_TRUE(dse::storage::SegmentWriter::write(original.path(), index.snapshot()));
  const auto valid = read_bytes(original.path());

  for (const std::size_t size : std::array<std::size_t, 4>{0U, 8U, 63U,
                                                           valid.size() - 1U}) {
    TemporarySegment damaged;
    const std::vector<std::byte> truncated(
        valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(size));
    write_bytes(damaged.path(), truncated);
    EXPECT_FALSE(dse::storage::SegmentReader::open(damaged.path()));
  }
  {
    auto bytes = valid; bytes.back() ^= std::byte{1}; TemporarySegment damaged;
    write_bytes(damaged.path(), bytes); auto opened = dse::storage::SegmentReader::open(damaged.path());
    ASSERT_FALSE(opened.has_value()); EXPECT_EQ(opened.error().code, dse::storage::SegmentErrorCode::corruption);
  }
  {
    auto bytes = valid; bytes[8] = std::byte{2}; TemporarySegment damaged;
    write_bytes(damaged.path(), bytes); auto opened = dse::storage::SegmentReader::open(damaged.path());
    ASSERT_FALSE(opened.has_value()); EXPECT_EQ(opened.error().code, dse::storage::SegmentErrorCode::unsupported_version);
  }
  auto limited = dse::storage::SegmentReader::open(
      original.path(), {.maximum_file_bytes = valid.size() - 1U});
  ASSERT_FALSE(limited.has_value());
  EXPECT_EQ(limited.error().code, dse::storage::SegmentErrorCode::resource_limit);
}

TEST(Segment, SerializationIsDeterministic) {
  TemporarySegment first;
  TemporarySegment second;
  auto index = populated_index();
  ASSERT_TRUE(dse::storage::SegmentWriter::write(first.path(), index.snapshot(),
                                                 {.segment_id = dse::SegmentId(9)}));
  ASSERT_TRUE(dse::storage::SegmentWriter::write(second.path(), index.snapshot(),
                                                 {.segment_id = dse::SegmentId(9)}));
  EXPECT_EQ(read_bytes(first.path()), read_bytes(second.path()));
}

TEST(Segment, RejectsChecksummedStructuralDirectoryCorruption) {
  TemporarySegment original;
  auto index = populated_index();
  ASSERT_TRUE(dse::storage::SegmentWriter::write(original.path(), index.snapshot()));
  const auto valid = read_bytes(original.path());

  const auto reject_mutation = [&](std::size_t offset, std::uint32_t value) {
    auto bytes = valid;
    set_u32(bytes, offset, value);
    repair_checksum(bytes);
    TemporarySegment damaged;
    write_bytes(damaged.path(), bytes);
    auto result = dse::storage::SegmentReader::open(damaged.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, dse::storage::SegmentErrorCode::corruption);
  };
  reject_mutation(64, 2U);       // first section type is no longer ordered
  reject_mutation(68, 1U);       // reserved directory word
  reject_mutation(72, 0U);       // section offset no longer contiguous
  reject_mutation(80, 0xffffffffU);  // section length escapes the file
  reject_mutation(88, 999U);     // directory/schema record-count disagreement
}

TEST(Segment, FailedWritePreservesExistingPublishedFile) {
  TemporarySegment target;
  auto valid_index = populated_index();
  ASSERT_TRUE(dse::storage::SegmentWriter::write(target.path(), valid_index.snapshot()));
  const auto original = read_bytes(target.path());

  auto schema = dse::index::IndexSchema::create(
      {{"title", dse::index::FieldType::text, true, true, 1.0,
        std::make_shared<const UnsupportedAnalyzer>()}});
  ASSERT_TRUE(schema.has_value());
  dse::index::InMemoryIndex unsupported(std::move(*schema));
  ASSERT_TRUE(unsupported.put({.id = dse::DocumentId("x"), .fields = {{"title", "x"}}}));
  const auto result = dse::storage::SegmentWriter::write(target.path(), unsupported.snapshot());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, dse::storage::SegmentErrorCode::invalid_snapshot);
  EXPECT_EQ(read_bytes(target.path()), original);
}

}  // namespace
