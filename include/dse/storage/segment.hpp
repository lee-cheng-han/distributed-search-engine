#pragma once

#include "dse/index/search_index_view.hpp"
#include "dse/types.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <string>

namespace dse::storage {

enum class SegmentErrorCode {
  io_error,
  corruption,
  unsupported_version,
  resource_limit,
  invalid_snapshot,
};

struct SegmentError {
  SegmentErrorCode code;
  std::string message;
};

struct SegmentWriteOptions {
  SegmentId segment_id{SegmentId(1)};
};

struct SegmentReadLimits {
  std::uint64_t maximum_file_bytes{1ULL << 30U};
  std::uint64_t maximum_documents{10'000'000};
  std::uint64_t maximum_terms{100'000'000};
  std::uint64_t maximum_postings{1'000'000'000};
  std::uint64_t maximum_positions{4'000'000'000};
  std::uint64_t maximum_string_bytes{16ULL << 20U};
  std::uint64_t maximum_fields{4'096};
};

class SegmentWriter {
 public:
  [[nodiscard]] static std::expected<void, SegmentError> write(
      const std::filesystem::path& path, const index::IndexSnapshot& snapshot,
      const SegmentWriteOptions& options = {});
};

class SegmentReader final : public index::SearchIndexView {
 public:
  [[nodiscard]] static std::expected<SegmentReader, SegmentError> open(
      const std::filesystem::path& path, const SegmentReadLimits& limits = {});

  [[nodiscard]] SegmentId segment_id() const noexcept { return segment_id_; }
  [[nodiscard]] const index::TermEntry* lookup(std::string_view field,
                                               std::string_view term) const override;
  [[nodiscard]] const index::DocumentRecord* document(const DocumentId& id) const override;
  [[nodiscard]] const index::DocumentRecord* document(InternalDocumentId id) const override;
  [[nodiscard]] std::optional<InternalDocumentId> internal_id(
      const DocumentId& id) const noexcept override;
  [[nodiscard]] const DocumentId* external_id(InternalDocumentId id) const noexcept override;
  [[nodiscard]] std::vector<InternalDocumentId> live_document_ids() const override;
  [[nodiscard]] std::size_t live_document_count() const noexcept override;
  [[nodiscard]] index::FieldStatistics field_statistics(
      std::string_view field) const noexcept override;
  [[nodiscard]] const index::IndexSchema& schema() const noexcept override { return schema_; }
  [[nodiscard]] bool validate_invariants(std::string* reason = nullptr) const override;
  [[nodiscard]] const index::DocumentRecords& records() const noexcept { return documents_; }

 private:
  SegmentReader(SegmentId segment_id, index::IndexSchema schema,
                index::FieldDictionaries fields, index::DocumentRecords documents,
                index::ExternalIdMap external_ids, index::FieldStatisticsMap statistics)
      : segment_id_(segment_id), schema_(std::move(schema)), fields_(std::move(fields)),
        documents_(std::move(documents)), external_ids_(std::move(external_ids)),
        statistics_(std::move(statistics)) {}

  SegmentId segment_id_;
  index::IndexSchema schema_;
  index::FieldDictionaries fields_;
  index::DocumentRecords documents_;
  index::ExternalIdMap external_ids_;
  index::FieldStatisticsMap statistics_;
};

[[nodiscard]] std::string_view describe(SegmentErrorCode code) noexcept;

}  // namespace dse::storage
