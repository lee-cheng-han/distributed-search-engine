#pragma once

#include "dse/document.hpp"
#include "dse/index/schema.hpp"

#include <cstdint>
#include <map>
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

using TermDictionary = std::map<std::string, TermEntry, std::less<>>;
using FieldDictionaries = std::map<std::string, TermDictionary, std::less<>>;
using DocumentRecords = std::map<DocumentId, DocumentRecord>;
using ExternalIdMap = std::map<InternalDocumentId, DocumentId>;
using FieldStatisticsMap = std::map<std::string, FieldStatistics, std::less<>>;

class SearchIndexView {
 public:
  virtual ~SearchIndexView() = default;
  [[nodiscard]] virtual const TermEntry* lookup(std::string_view field,
                                                std::string_view term) const = 0;
  [[nodiscard]] virtual const DocumentRecord* document(const DocumentId& id) const = 0;
  [[nodiscard]] virtual const DocumentRecord* document(InternalDocumentId id) const = 0;
  [[nodiscard]] virtual std::optional<InternalDocumentId> internal_id(
      const DocumentId& id) const noexcept = 0;
  [[nodiscard]] virtual const DocumentId* external_id(InternalDocumentId id) const noexcept = 0;
  [[nodiscard]] virtual std::vector<InternalDocumentId> live_document_ids() const = 0;
  [[nodiscard]] virtual std::size_t live_document_count() const noexcept = 0;
  [[nodiscard]] virtual FieldStatistics field_statistics(
      std::string_view field) const noexcept = 0;
  [[nodiscard]] virtual const IndexSchema& schema() const noexcept = 0;
  [[nodiscard]] virtual bool validate_invariants(std::string* reason = nullptr) const = 0;
};

class IndexSnapshot {
 public:
  [[nodiscard]] const IndexSchema& schema() const noexcept { return schema_; }
  [[nodiscard]] const FieldDictionaries& fields() const noexcept { return fields_; }
  [[nodiscard]] const DocumentRecords& documents() const noexcept { return documents_; }
  [[nodiscard]] const ExternalIdMap& external_ids() const noexcept { return external_ids_; }
  [[nodiscard]] const FieldStatisticsMap& statistics() const noexcept { return statistics_; }

 private:
  friend class InMemoryIndex;
  IndexSnapshot(IndexSchema schema, FieldDictionaries fields, DocumentRecords documents,
                ExternalIdMap external_ids, FieldStatisticsMap statistics)
      : schema_(std::move(schema)), fields_(std::move(fields)), documents_(std::move(documents)),
        external_ids_(std::move(external_ids)), statistics_(std::move(statistics)) {}

  IndexSchema schema_;
  FieldDictionaries fields_;
  DocumentRecords documents_;
  ExternalIdMap external_ids_;
  FieldStatisticsMap statistics_;
};

}  // namespace dse::index
