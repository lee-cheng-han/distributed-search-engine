#pragma once

#include "dse/document.hpp"
#include "dse/index/search_index_view.hpp"
#include "dse/index/schema.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dse::index {

enum class IndexErrorCode {
  empty_document_id,
  stale_version,
  schema_error,
  internal_id_exhausted,
  analysis_failed,
  field_length_overflow,
};

struct IndexError {
  IndexErrorCode code;
  std::string message;
  std::optional<SchemaError> schema_error;
};

class InMemoryIndex final : public SearchIndexView {
 public:
  explicit InMemoryIndex(
      IndexSchema schema = IndexSchema::default_schema(),
      std::uint32_t maximum_internal_id = std::numeric_limits<std::uint32_t>::max())
      : schema_(std::move(schema)), maximum_internal_id_(maximum_internal_id) {}

  // Higher versions replace older versions. Equal/older versions are rejected.
  [[nodiscard]] std::expected<void, IndexError> put(Document document);
  [[nodiscard]] std::expected<void, IndexError> erase(const DocumentId& id,
                                                       std::uint64_t version);
  [[nodiscard]] const TermEntry* lookup(std::string_view field, std::string_view term) const override;
  [[nodiscard]] const DocumentRecord* document(const DocumentId& id) const override;
  [[nodiscard]] const DocumentRecord* document(InternalDocumentId id) const override;
  [[nodiscard]] std::optional<InternalDocumentId> internal_id(
      const DocumentId& id) const noexcept override;
  [[nodiscard]] const DocumentId* external_id(InternalDocumentId id) const noexcept override;
  [[nodiscard]] std::vector<InternalDocumentId> live_document_ids() const override;
  [[nodiscard]] std::size_t live_document_count() const noexcept override;
  [[nodiscard]] FieldStatistics field_statistics(std::string_view field) const noexcept override;
  [[nodiscard]] const IndexSchema& schema() const noexcept override { return schema_; }
  [[nodiscard]] bool validate_invariants(std::string* reason = nullptr) const override;
  [[nodiscard]] IndexSnapshot snapshot() const;

 private:
  using Dictionary = TermDictionary;
  struct FieldAccumulator {
    std::size_t document_count{};
    std::uint64_t total_length{};
  };

  IndexSchema schema_;
  std::map<std::string, Dictionary, std::less<>> fields_;
  std::map<DocumentId, DocumentRecord> documents_;
  std::map<InternalDocumentId, DocumentId> external_ids_;
  std::map<std::string, FieldAccumulator, std::less<>> field_statistics_;
  std::uint32_t next_internal_id_{1};
  std::uint32_t maximum_internal_id_;
};

}  // namespace dse::index
