#pragma once

#include "dse/index/in_memory_index.hpp"
#include "dse/storage/generation.hpp"

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

namespace dse::storage {

enum class WriterErrorCode { index_error, storage_error, stale_version };
struct WriterError { WriterErrorCode code; std::string message; };
struct IndexWriterOptions { std::size_t maximum_buffered_mutations{10'000}; };

// Thread-safe near-real-time writer. Mutations become searchable after refresh() or an automatic
// threshold flush. Publication is an atomic manifest generation change.
class IndexWriter {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<IndexWriter>, WriterError> open(
      std::filesystem::path directory, index::IndexSchema schema = index::IndexSchema::default_schema(),
      IndexWriterOptions options = {});
  [[nodiscard]] std::expected<void, WriterError> put(Document document);
  [[nodiscard]] std::expected<void, WriterError> erase(const DocumentId& id, std::uint64_t version);
  [[nodiscard]] std::expected<void, WriterError> refresh();
  [[nodiscard]] std::expected<void, WriterError> merge_all();
  [[nodiscard]] std::expected<GenerationView, WriterError> open_search_view() const;
  [[nodiscard]] GenerationId generation() const noexcept;

 private:
  IndexWriter(std::filesystem::path directory, index::IndexSchema schema, IndexWriterOptions options);
  [[nodiscard]] std::expected<void, WriterError> flush_locked();
  std::filesystem::path directory_;
  index::IndexSchema schema_;
  IndexWriterOptions options_;
  ManifestStore store_;
  std::unique_ptr<index::InMemoryIndex> active_;
  IndexManifest manifest_{GenerationId(0), {}};
  std::map<DocumentId, std::uint64_t> versions_;
  std::size_t buffered_mutations_{};
  std::uint64_t next_segment_id_{1};
  mutable std::mutex mutex_;
};
}  // namespace dse::storage
