#include "dse/index/in_memory_index.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace dse::index {

void InMemoryIndex::remove_postings(const DocumentId& id) {
  for (auto field_it = fields_.begin(); field_it != fields_.end();) {
    for (auto term_it = field_it->second.begin(); term_it != field_it->second.end();) {
      auto& postings = term_it->second.postings;
      std::erase_if(postings, [&](const Posting& posting) { return posting.document_id == id; });
      term_it->second.document_frequency = static_cast<std::uint32_t>(postings.size());
      if (postings.empty()) term_it = field_it->second.erase(term_it); else ++term_it;
    }
    if (field_it->second.empty()) field_it = fields_.erase(field_it); else ++field_it;
  }
}

std::expected<void, IndexError> InMemoryIndex::put(Document document_value) {
  if (document_value.id.value().empty()) {
    return std::unexpected(
        IndexError{IndexErrorCode::empty_document_id, "document ID must not be empty", {}});
  }
  const auto existing = documents_.find(document_value.id);
  if (existing != documents_.end() && document_value.version <= existing->second.document.version) {
    return std::unexpected(IndexError{IndexErrorCode::stale_version,
                                      "document version must be newer than visible version", {}});
  }

  for (const auto& [field, value] : document_value.fields) {
    auto validation = schema_.validate_value(field, value);
    if (!validation) {
      return std::unexpected(IndexError{IndexErrorCode::schema_error,
                                        validation.error().message, validation.error()});
    }
    const auto* definition = schema_.find(field);
    if (!definition->indexed) {
      SchemaError schema_error{SchemaErrorCode::field_not_indexed, field,
                               "field is not configured for indexing"};
      return std::unexpected(IndexError{IndexErrorCode::schema_error, schema_error.message,
                                        std::move(schema_error)});
    }
  }
  for (const auto& [field, value] : document_value.stored_metadata) {
    auto validation = schema_.validate_value(field, value);
    if (!validation) {
      return std::unexpected(IndexError{IndexErrorCode::schema_error,
                                        validation.error().message, validation.error()});
    }
    const auto* definition = schema_.find(field);
    if (!definition->stored) {
      SchemaError schema_error{SchemaErrorCode::field_not_stored, field,
                               "field is not configured for stored metadata"};
      return std::unexpected(IndexError{IndexErrorCode::schema_error, schema_error.message,
                                        std::move(schema_error)});
    }
  }

  remove_postings(document_value.id);
  DocumentRecord record{std::move(document_value), {}};
  if (!record.document.deleted) {
    for (const auto& [field, text] : record.document.fields) {
      const auto* definition = schema_.find(field);
      if (definition == nullptr || !definition->indexed) continue;
      const auto tokens = definition->analyzer->analyze(text);
      record.field_lengths[field] = static_cast<std::uint32_t>(tokens.size());
      std::map<std::string, std::vector<std::uint32_t>, std::less<>> positions;
      for (const auto& token : tokens) positions[token.term].push_back(token.position);
      for (auto& [term, term_positions] : positions) {
        auto& entry = fields_[field][term];
        entry.postings.push_back({record.document.id,
                                  static_cast<std::uint32_t>(term_positions.size()),
                                  std::move(term_positions)});
        std::ranges::sort(entry.postings, {}, &Posting::document_id);
        entry.document_frequency = static_cast<std::uint32_t>(entry.postings.size());
      }
    }
  }
  documents_.insert_or_assign(record.document.id, std::move(record));
  return {};
}

std::expected<void, IndexError> InMemoryIndex::erase(const DocumentId& id, std::uint64_t version) {
  const auto existing = documents_.find(id);
  if (existing != documents_.end() && version <= existing->second.document.version) {
    return std::unexpected(IndexError{IndexErrorCode::stale_version,
                                      "delete version must be newer than visible version", {}});
  }
  Document tombstone{.id = id,
                     .fields = {},
                     .stored_metadata = {},
                     .version = version,
                     .deleted = true};
  return put(std::move(tombstone));
}

const TermEntry* InMemoryIndex::lookup(std::string_view field, std::string_view term) const {
  const auto field_it = fields_.find(field);
  if (field_it == fields_.end()) return nullptr;
  const auto term_it = field_it->second.find(term);
  return term_it == field_it->second.end() ? nullptr : &term_it->second;
}

const DocumentRecord* InMemoryIndex::document(const DocumentId& id) const {
  const auto it = documents_.find(id);
  return it == documents_.end() ? nullptr : &it->second;
}

std::vector<DocumentId> InMemoryIndex::live_document_ids() const {
  std::vector<DocumentId> ids;
  ids.reserve(live_document_count());
  for (const auto& [id, record] : documents_) {
    if (!record.document.deleted) ids.push_back(id);
  }
  return ids;
}

std::size_t InMemoryIndex::live_document_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      documents_, [](const auto& item) { return !item.second.document.deleted; }));
}

FieldStatistics InMemoryIndex::field_statistics(std::string_view field) const noexcept {
  FieldStatistics statistics;
  for (const auto& [id, record] : documents_) {
    (void)id;
    if (record.document.deleted) continue;
    const auto length = record.field_lengths.find(field);
    if (length == record.field_lengths.end()) continue;
    ++statistics.document_count;
    statistics.total_length += length->second;
  }
  if (statistics.document_count != 0U) {
    statistics.average_length = static_cast<double>(statistics.total_length) /
                                static_cast<double>(statistics.document_count);
  }
  return statistics;
}

bool InMemoryIndex::validate_invariants(std::string* reason) const {
  const auto fail = [&](std::string value) { if (reason) *reason = std::move(value); return false; };
  for (const auto& [field, dictionary] : fields_) {
    for (const auto& [term, entry] : dictionary) {
      if (entry.document_frequency != entry.postings.size()) return fail("document frequency mismatch");
      if (!std::ranges::is_sorted(entry.postings, {}, &Posting::document_id)) return fail("unsorted postings");
      for (const auto& posting : entry.postings) {
        const auto doc_it = documents_.find(posting.document_id);
        if (doc_it == documents_.end() || doc_it->second.document.deleted) return fail("posting references missing/deleted document");
        if (posting.term_frequency != posting.positions.size()) return fail("term frequency mismatch");
        if (!std::ranges::is_sorted(posting.positions) ||
            std::ranges::adjacent_find(posting.positions) != posting.positions.end()) return fail("positions not strictly increasing");
        if (!doc_it->second.field_lengths.contains(field)) return fail("field length missing");
        if (term.empty()) return fail("empty term");
      }
    }
  }
  for (const auto& [id, record] : documents_) {
    if (id != record.document.id) return fail("document key mismatch");
    if (record.document.deleted && !record.field_lengths.empty()) return fail("tombstone has field lengths");
    if (!record.document.deleted) {
      for (const auto& [field, text] : record.document.fields) {
        const auto* definition = schema_.find(field);
        if (definition == nullptr) return fail("document contains unknown field");
        if (!definition->indexed) continue;
        const auto expected = definition->analyzer->analyze(text).size();
        const auto it = record.field_lengths.find(field);
        if (it == record.field_lengths.end() || it->second != expected) return fail("field length mismatch");
      }
    }
  }
  return true;
}

}  // namespace dse::index
