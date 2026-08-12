#pragma once

#include "dse/analysis/analyzer.hpp"
#include "dse/document.hpp"

#include <cstdint>
#include <map>
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

class InMemoryIndex {
 public:
  explicit InMemoryIndex(const analysis::Analyzer& analyzer) : analyzer_(analyzer) {}

  // Higher versions replace older versions. Equal/older versions are rejected.
  bool put(Document document);
  bool erase(const DocumentId& id, std::uint64_t version);
  [[nodiscard]] const TermEntry* lookup(std::string_view field, std::string_view term) const;
  [[nodiscard]] const DocumentRecord* document(const DocumentId& id) const;
  [[nodiscard]] std::vector<DocumentId> live_document_ids() const;
  [[nodiscard]] std::size_t live_document_count() const noexcept;
  [[nodiscard]] FieldStatistics field_statistics(std::string_view field) const noexcept;
  [[nodiscard]] bool validate_invariants(std::string* reason = nullptr) const;

 private:
  using Dictionary = std::map<std::string, TermEntry, std::less<>>;
  void remove_postings(const DocumentId& id);

  const analysis::Analyzer& analyzer_;
  std::map<std::string, Dictionary, std::less<>> fields_;
  std::map<DocumentId, DocumentRecord> documents_;
};

}  // namespace dse::index
