#include "dse/storage/generation.hpp"

#include "dse/storage/segment.hpp"

#include <map>
#include <utility>

namespace dse::storage {
namespace {
GenerationError error(GenerationErrorCode code, std::string message) { return {code, std::move(message)}; }
bool same_schema(const index::IndexSchema& left, const index::IndexSchema& right) {
  if (left.fields().size() != right.fields().size()) return false;
  auto a = left.fields().begin();
  auto b = right.fields().begin();
  for (; a != left.fields().end(); ++a, ++b) {
    const auto& x = a->second;
    const auto& y = b->second;
    if (x.name != y.name || x.type != y.type || x.indexed != y.indexed || x.stored != y.stored || x.boost != y.boost) return false;
    if (static_cast<bool>(x.analyzer) != static_cast<bool>(y.analyzer)) return false;
    if (x.analyzer && x.analyzer->descriptor() != y.analyzer->descriptor()) return false;
  }
  return true;
}
}

std::expected<GenerationView, GenerationError> GenerationView::open(OpenGeneration generation) {
  if (generation.segments().empty()) return std::unexpected(error(GenerationErrorCode::empty_generation, "generation has no segments"));
  const auto& schema = generation.segments().front()->schema();
  auto resolved = std::make_shared<index::InMemoryIndex>(schema);
  std::map<DocumentId, Document> winners;
  for (const auto& segment : generation.segments()) {
    if (!same_schema(segment->schema(), schema)) return std::unexpected(error(GenerationErrorCode::schema_mismatch, "segment schemas differ"));
    for (const auto& [id, record] : segment->records()) {
      const auto found = winners.find(id);
      if (found == winners.end() || record.document.version > found->second.version) winners[id] = record.document;
      else if (record.document.version == found->second.version && record.document.deleted != found->second.deleted)
        return std::unexpected(error(GenerationErrorCode::invalid_document, "conflicting equal document versions"));
    }
  }
  for (auto& [id, document] : winners) {
    (void)id;
    const auto result = resolved->put(std::move(document));
    if (!result) return std::unexpected(error(GenerationErrorCode::invalid_document, result.error().message));
  }
  return GenerationView(std::move(generation), std::move(resolved));
}

std::expected<ManifestSegment, GenerationError> SegmentMerger::merge(
    const GenerationView& generation, const std::filesystem::path& directory, SegmentId output_id) {
  const std::string filename = "segment-" + std::to_string(output_id.value()) + ".dseg";
  const auto result = SegmentWriter::write(directory / filename, generation.snapshot(), {.segment_id = output_id});
  if (!result) return std::unexpected(error(GenerationErrorCode::segment_error, result.error().message));
  return ManifestSegment{output_id, filename};
}
}  // namespace dse::storage
