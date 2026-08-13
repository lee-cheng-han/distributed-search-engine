#pragma once

#include "dse/document.hpp"
#include "dse/index/schema.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dse::index {

struct Posting {
  DocumentId document_id;
  std::uint32_t term_frequency{};
  std::vector<std::uint32_t> positions;
};

struct TermEntry {
  std::uint32_t document_frequency{};
  std::vector<Posting> postings;
};

struct DocumentRecord {
  Document document;
  std::map<std::string, std::uint32_t, std::less<>> field_lengths;
};

struct FieldStatistics {
  std::size_t document_count{};
  std::uint64_t total_length{};
  double average_length{};
};

enum class IndexErrorCode { empty_document_id, stale_version, schema_error };

struct IndexError {
  IndexErrorCode code;
  std::string message;
  std::optional<SchemaError> schema_error;
};

class InMemoryIndex {
 public:
  explicit InMemoryIndex(IndexSchema schema = IndexSchema::default_schema())
      : schema_(std::move(schema)) {}

  // Higher versions replace older versions. Equal/older versions are rejected.
  [[nodiscard]] std::expected<void, IndexError> put(Document document);
  [[nodiscard]] std::expected<void, IndexError> erase(const DocumentId& id,
                                                       std::uint64_t version);
  [[nodiscard]] const TermEntry* lookup(std::string_view field, std::string_view term) const;
  [[nodiscard]] const DocumentRecord* document(const DocumentId& id) const;
  [[nodiscard]] std::vector<DocumentId> live_document_ids() const;
  [[nodiscard]] std::size_t live_document_count() const noexcept;
  [[nodiscard]] FieldStatistics field_statistics(std::string_view field) const noexcept;
  [[nodiscard]] const IndexSchema& schema() const noexcept { return schema_; }
  [[nodiscard]] bool validate_invariants(std::string* reason = nullptr) const;

 private:
  using Dictionary = std::map<std::string, TermEntry, std::less<>>;
  void remove_postings(const DocumentId& id);

  IndexSchema schema_;
  std::map<std::string, Dictionary, std::less<>> fields_;
  std::map<DocumentId, DocumentRecord> documents_;
};

}  // namespace dse::index
