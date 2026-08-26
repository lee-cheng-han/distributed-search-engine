#include "dse/storage/index_writer.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace dse::storage {
namespace {
WriterError failure(WriterErrorCode code, std::string message) { return {code, std::move(message)}; }
}

IndexWriter::IndexWriter(std::filesystem::path directory, index::IndexSchema schema, IndexWriterOptions options)
    : directory_(std::move(directory)), schema_(std::move(schema)), options_(options), store_(directory_),
      active_(std::make_unique<index::InMemoryIndex>(schema_)) {}

std::expected<std::unique_ptr<IndexWriter>, WriterError> IndexWriter::open(
    std::filesystem::path directory, index::IndexSchema schema, IndexWriterOptions options) {
  if (options.maximum_buffered_mutations == 0U) return std::unexpected(failure(WriterErrorCode::storage_error, "flush threshold must be positive"));
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec) return std::unexpected(failure(WriterErrorCode::storage_error, ec.message()));
  auto writer = std::unique_ptr<IndexWriter>(new IndexWriter(std::move(directory), std::move(schema), options));
  if (std::filesystem::exists(writer->directory_ / "CURRENT")) {
    auto opened = writer->store_.open_current();
    if (!opened) return std::unexpected(failure(WriterErrorCode::storage_error, opened.error().message));
    writer->manifest_ = opened->manifest();
    for (const auto& segment : opened->segments()) {
      writer->next_segment_id_ = std::max(writer->next_segment_id_, segment->segment_id().value() + 1U);
      for (const auto& [id, record] : segment->records()) writer->versions_[id] = std::max(writer->versions_[id], record.document.version);
    }
  }
  return writer;
}

std::expected<void, WriterError> IndexWriter::put(Document document) {
  std::scoped_lock lock(mutex_);
  if (const auto it = versions_.find(document.id); it != versions_.end() && document.version <= it->second)
    return std::unexpected(failure(WriterErrorCode::stale_version, "document version is not newer"));
  const auto id = document.id; const auto version = document.version;
  auto result = active_->put(std::move(document));
  if (!result) return std::unexpected(failure(WriterErrorCode::index_error, result.error().message));
  versions_[id] = version;
  ++buffered_mutations_;
  return buffered_mutations_ >= options_.maximum_buffered_mutations ? flush_locked() : std::expected<void, WriterError>{};
}

std::expected<void, WriterError> IndexWriter::erase(const DocumentId& id, std::uint64_t version) {
  Document tombstone{.id=id, .fields={}, .stored_metadata={}, .version=version, .deleted=true};
  return put(std::move(tombstone));
}

std::expected<void, WriterError> IndexWriter::flush_locked() {
  if (buffered_mutations_ == 0U) return {};
  const SegmentId id(next_segment_id_);
  const std::string filename = "segment-" + std::to_string(next_segment_id_) + ".dseg";
  auto write = SegmentWriter::write(directory_ / filename, active_->snapshot(), {.segment_id=id});
  if (!write) return std::unexpected(failure(WriterErrorCode::storage_error, write.error().message));
  IndexManifest next{GenerationId(manifest_.generation.value() + 1U), manifest_.segments};
  next.segments.push_back({id, filename});
  auto publish = store_.publish(next);
  if (!publish) return std::unexpected(failure(WriterErrorCode::storage_error, publish.error().message));
  manifest_ = std::move(next); ++next_segment_id_; buffered_mutations_ = 0U;
  active_ = std::make_unique<index::InMemoryIndex>(schema_);
  return {};
}

std::expected<void, WriterError> IndexWriter::refresh() { std::scoped_lock lock(mutex_); return flush_locked(); }

std::expected<GenerationView, WriterError> IndexWriter::open_search_view() const {
  std::scoped_lock lock(mutex_);
  auto opened = store_.open_current();
  if (!opened) return std::unexpected(failure(WriterErrorCode::storage_error, opened.error().message));
  auto view = GenerationView::open(std::move(*opened));
  if (!view) return std::unexpected(failure(WriterErrorCode::storage_error, view.error().message));
  return std::move(*view);
}

std::expected<void, WriterError> IndexWriter::merge_all() {
  std::scoped_lock lock(mutex_);
  if (auto flushed = flush_locked(); !flushed) return flushed;
  if (manifest_.segments.size() <= 1U) return {};
  auto opened=store_.open_current(); if (!opened) return std::unexpected(failure(WriterErrorCode::storage_error,opened.error().message));
  auto view=GenerationView::open(std::move(*opened)); if (!view) return std::unexpected(failure(WriterErrorCode::storage_error,view.error().message));
  const SegmentId id(next_segment_id_); auto merged=SegmentMerger::merge(*view,directory_,id);
  if (!merged) return std::unexpected(failure(WriterErrorCode::storage_error,merged.error().message));
  IndexManifest next{GenerationId(manifest_.generation.value()+1U),{*merged}};
  auto publish=store_.publish(next); if (!publish) return std::unexpected(failure(WriterErrorCode::storage_error,publish.error().message));
  manifest_=std::move(next); ++next_segment_id_; return {};
}

GenerationId IndexWriter::generation() const noexcept { std::scoped_lock lock(mutex_); return manifest_.generation; }
}  // namespace dse::storage
