#pragma once

#include "dse/document.hpp"
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

struct Posting {
  InternalDocumentId document_id;
  std::uint32_t term_frequency{};
  std::vector<std::uint32_t> positions;
};

struct TermEntry {
  std::uint32_t document_frequency{};
  std::vector<Posting> postings;
};

struct DocumentRecord {
  InternalDocumentId internal_id;
  Document document;
  std::map<std::string, std::uint32_t, std::less<>> field_lengths;
  std::map<std::string, std::vector<std::string>, std::less<>> indexed_terms;
};

struct FieldStatistics {
  std::size_t document_count{};
  std::uint64_t total_length{};
  double average_length{};
};

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

class InMemoryIndex {
 public:
  explicit InMemoryIndex(
      IndexSchema schema = IndexSchema::default_schema(),
      std::uint32_t maximum_internal_id = std::numeric_limits<std::uint32_t>::max())
      : schema_(std::move(schema)), maximum_internal_id_(maximum_internal_id) {}

  // Higher versions replace older versions. Equal/older versions are rejected.
  [[nodiscard]] std::expected<void, IndexError> put(Document document);
  [[nodiscard]] std::expected<void, IndexError> erase(const DocumentId& id,
                                                       std::uint64_t version);
  [[nodiscard]] const TermEntry* lookup(std::string_view field, std::string_view term) const;
  [[nodiscard]] const DocumentRecord* document(const DocumentId& id) const;
  [[nodiscard]] const DocumentRecord* document(InternalDocumentId id) const;
  [[nodiscard]] std::optional<InternalDocumentId> internal_id(const DocumentId& id) const noexcept;
  [[nodiscard]] const DocumentId* external_id(InternalDocumentId id) const noexcept;
  [[nodiscard]] std::vector<InternalDocumentId> live_document_ids() const;
  [[nodiscard]] std::size_t live_document_count() const noexcept;
  [[nodiscard]] FieldStatistics field_statistics(std::string_view field) const noexcept;
  [[nodiscard]] const IndexSchema& schema() const noexcept { return schema_; }
  [[nodiscard]] bool validate_invariants(std::string* reason = nullptr) const;

 private:
  using Dictionary = std::map<std::string, TermEntry, std::less<>>;
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
