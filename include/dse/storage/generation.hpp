#pragma once

#include "dse/index/in_memory_index.hpp"
#include "dse/storage/manifest.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace dse::storage {

enum class GenerationErrorCode { empty_generation, schema_mismatch, invalid_document, segment_error };
struct GenerationError { GenerationErrorCode code; std::string message; };

// A correctness-first generation view. It resolves external IDs by highest document version,
// including tombstones, and rebuilds globally consistent postings/statistics in memory.
class GenerationView final : public index::SearchIndexView {
 public:
  [[nodiscard]] static std::expected<GenerationView, GenerationError> open(OpenGeneration generation);

  [[nodiscard]] const index::TermEntry* lookup(std::string_view f, std::string_view t) const override { return resolved_->lookup(f, t); }
  [[nodiscard]] const index::DocumentRecord* document(const DocumentId& id) const override { return resolved_->document(id); }
  [[nodiscard]] const index::DocumentRecord* document(InternalDocumentId id) const override { return resolved_->document(id); }
  [[nodiscard]] std::optional<InternalDocumentId> internal_id(const DocumentId& id) const noexcept override { return resolved_->internal_id(id); }
  [[nodiscard]] const DocumentId* external_id(InternalDocumentId id) const noexcept override { return resolved_->external_id(id); }
  [[nodiscard]] std::vector<InternalDocumentId> live_document_ids() const override { return resolved_->live_document_ids(); }
  [[nodiscard]] std::size_t live_document_count() const noexcept override { return resolved_->live_document_count(); }
  [[nodiscard]] index::FieldStatistics field_statistics(std::string_view f) const noexcept override { return resolved_->field_statistics(f); }
  [[nodiscard]] const index::IndexSchema& schema() const noexcept override { return resolved_->schema(); }
  [[nodiscard]] bool validate_invariants(std::string* why = nullptr) const override { return resolved_->validate_invariants(why); }
  [[nodiscard]] const OpenGeneration& source() const noexcept { return generation_; }
  [[nodiscard]] index::IndexSnapshot snapshot() const { return resolved_->snapshot(); }

 private:
  GenerationView(OpenGeneration generation, std::shared_ptr<index::InMemoryIndex> resolved)
      : generation_(std::move(generation)), resolved_(std::move(resolved)) {}
  OpenGeneration generation_;
  std::shared_ptr<index::InMemoryIndex> resolved_;
};

class SegmentMerger {
 public:
  [[nodiscard]] static std::expected<ManifestSegment, GenerationError> merge(
      const GenerationView& generation, const std::filesystem::path& directory, SegmentId output_id);
};

}  // namespace dse::storage
