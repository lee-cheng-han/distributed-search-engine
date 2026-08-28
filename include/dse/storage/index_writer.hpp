#pragma once
#include "dse/index/in_memory_index.hpp"
#include "dse/storage/generation.hpp"
#include <condition_variable>
#include <deque>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
namespace dse::storage {
enum class WriterErrorCode { index_error, storage_error, stale_version, closed };
struct WriterError { WriterErrorCode code; std::string message; };
struct IndexWriterOptions { std::size_t maximum_buffered_mutations{10'000}; std::size_t maximum_frozen_indexes{2}; std::size_t automatic_merge_segment_count{8}; bool reclaim_obsolete_files{true}; };
struct IndexWriterStatistics { std::size_t buffered_mutations{}; std::size_t frozen_indexes{}; bool flush_in_progress{}; std::size_t published_segments{}; GenerationId generation{GenerationId(0)}; };
class IndexWriter {
 public:
  ~IndexWriter();
  IndexWriter(const IndexWriter&)=delete; IndexWriter& operator=(const IndexWriter&)=delete;
  [[nodiscard]] static std::expected<std::unique_ptr<IndexWriter>,WriterError> open(std::filesystem::path directory,index::IndexSchema schema=index::IndexSchema::default_schema(),IndexWriterOptions options={});
  [[nodiscard]] std::expected<void,WriterError> put(Document document);
  [[nodiscard]] std::expected<void,WriterError> erase(const DocumentId& id,std::uint64_t version);
  [[nodiscard]] std::expected<void,WriterError> refresh();
  [[nodiscard]] std::expected<void,WriterError> merge_all();
  [[nodiscard]] std::expected<GenerationView,WriterError> open_search_view() const;
  [[nodiscard]] GenerationId generation() const noexcept;
  [[nodiscard]] IndexWriterStatistics statistics() const noexcept;
 private:
  IndexWriter(std::filesystem::path directory,index::IndexSchema schema,IndexWriterOptions options);
  void worker_loop(); void freeze_active_locked();
  [[nodiscard]] std::expected<void,WriterError> wait_for_capacity_locked(std::unique_lock<std::mutex>& lock);
  [[nodiscard]] std::expected<void,WriterError> wait_until_idle_locked(std::unique_lock<std::mutex>& lock);
  [[nodiscard]] std::expected<void,WriterError> publish_snapshot(index::IndexSnapshot snapshot);
  [[nodiscard]] std::expected<void,WriterError> compact_published();
  void reclaim(const IndexManifest& obsolete,const IndexManifest& replacement) const;
  std::filesystem::path directory_; index::IndexSchema schema_; IndexWriterOptions options_; ManifestStore store_;
  std::unique_ptr<index::InMemoryIndex> active_; IndexManifest manifest_{GenerationId(0),{}};
  std::map<DocumentId,std::uint64_t> versions_; std::size_t buffered_mutations_{}; std::uint64_t next_segment_id_{1};
  mutable std::mutex mutex_; std::condition_variable condition_; std::deque<index::IndexSnapshot> frozen_;
  std::mutex publication_mutex_;
  bool worker_busy_{}; bool stopping_{}; std::optional<WriterError> worker_error_; std::thread worker_;
};
}  // namespace dse::storage
