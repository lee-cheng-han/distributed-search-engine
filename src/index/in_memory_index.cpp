#include "dse/index/in_memory_index.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace dse::index {

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

  const bool is_new_document = existing == documents_.end();
  InternalDocumentId internal_document_id;
  if (is_new_document) {
    if (next_internal_id_ == 0U || next_internal_id_ > maximum_internal_id_) {
      return std::unexpected(IndexError{IndexErrorCode::internal_id_exhausted,
                                        "segment-local document ID space is exhausted", {}});
    }
    internal_document_id = InternalDocumentId(next_internal_id_);
  } else {
    internal_document_id = existing->second.internal_id;
  }

  using PreparedPostings =
      std::map<std::string,
               std::map<std::string, std::vector<std::uint32_t>, std::less<>>, std::less<>>;
  PreparedPostings prepared_postings;
  DocumentRecord record{internal_document_id, std::move(document_value), {}, {}};
  if (!record.document.deleted) {
    try {
      for (const auto& [field, text] : record.document.fields) {
        const auto* definition = schema_.find(field);
        if (definition == nullptr || !definition->indexed) continue;
        const auto tokens = definition->analyzer->analyze(text);
        if (tokens.size() > std::numeric_limits<std::uint32_t>::max()) {
          return std::unexpected(IndexError{IndexErrorCode::field_length_overflow,
                                            "analyzed field length exceeds uint32 range", {}});
        }
        record.field_lengths[field] = static_cast<std::uint32_t>(tokens.size());
        auto& positions = prepared_postings[field];
        for (const auto& token : tokens) positions[token.term].push_back(token.position);
        auto& terms = record.indexed_terms[field];
        terms.reserve(positions.size());
        for (const auto& [term, term_positions] : positions) {
          (void)term_positions;
          terms.push_back(term);
        }
      }
    } catch (const std::exception& exception) {
      return std::unexpected(IndexError{IndexErrorCode::analysis_failed,
                                        "document analysis failed: " +
                                            std::string(exception.what()),
                                        {}});
    }
  }

  // Stage a complete replacement and publish with noexcept swaps. This is intentionally
  // correctness-first; later immutable builders make publication cheap without weakening semantics.
  auto staged_fields = fields_;
  auto staged_documents = documents_;
  auto staged_external_ids = external_ids_;
  auto staged_statistics = field_statistics_;

  if (existing != documents_.end() && !existing->second.document.deleted) {
    for (const auto& [field, length] : existing->second.field_lengths) {
      auto statistic = staged_statistics.find(field);
      if (statistic != staged_statistics.end()) {
        --statistic->second.document_count;
        statistic->second.total_length -= length;
        if (statistic->second.document_count == 0U) staged_statistics.erase(statistic);
      }
    }
    for (const auto& [field, terms] : existing->second.indexed_terms) {
      auto field_iterator = staged_fields.find(field);
      if (field_iterator == staged_fields.end()) continue;
      for (const auto& term : terms) {
        auto term_iterator = field_iterator->second.find(term);
        if (term_iterator == field_iterator->second.end()) continue;
        auto& postings = term_iterator->second.postings;
        std::erase_if(postings, [&](const Posting& posting) {
          return posting.document_id == internal_document_id;
        });
        term_iterator->second.document_frequency =
            static_cast<std::uint32_t>(postings.size());
        if (postings.empty()) field_iterator->second.erase(term_iterator);
      }
      if (field_iterator->second.empty()) staged_fields.erase(field_iterator);
    }
  }

  if (!record.document.deleted) {
    for (auto& [field, positions] : prepared_postings) {
      for (auto& [term, term_positions] : positions) {
        auto& entry = staged_fields[field][term];
        entry.postings.push_back({record.internal_id,
                                  static_cast<std::uint32_t>(term_positions.size()),
                                  std::move(term_positions)});
        std::ranges::sort(entry.postings, {}, &Posting::document_id);
        entry.document_frequency = static_cast<std::uint32_t>(entry.postings.size());
      }
    }
    for (const auto& [field, length] : record.field_lengths) {
      auto& statistic = staged_statistics[field];
      ++statistic.document_count;
      statistic.total_length += length;
    }
  }
  const auto external_document_id = record.document.id;
  staged_external_ids.insert_or_assign(internal_document_id, external_document_id);
  staged_documents.insert_or_assign(external_document_id, std::move(record));

  fields_.swap(staged_fields);
  documents_.swap(staged_documents);
  external_ids_.swap(staged_external_ids);
  field_statistics_.swap(staged_statistics);
  if (is_new_document) ++next_internal_id_;
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

const DocumentRecord* InMemoryIndex::document(InternalDocumentId id) const {
  const auto* external = external_id(id);
  return external == nullptr ? nullptr : document(*external);
}

std::optional<InternalDocumentId> InMemoryIndex::internal_id(const DocumentId& id) const noexcept {
  const auto iterator = documents_.find(id);
  return iterator == documents_.end() ? std::nullopt
                                      : std::optional(iterator->second.internal_id);
}

const DocumentId* InMemoryIndex::external_id(InternalDocumentId id) const noexcept {
  const auto iterator = external_ids_.find(id);
  return iterator == external_ids_.end() ? nullptr : &iterator->second;
}

std::vector<InternalDocumentId> InMemoryIndex::live_document_ids() const {
  std::vector<InternalDocumentId> ids;
  ids.reserve(live_document_count());
  for (const auto& [id, external] : external_ids_) {
    const auto* record = document(external);
    if (record != nullptr && !record->document.deleted) ids.push_back(id);
  }
  return ids;
}

std::size_t InMemoryIndex::live_document_count() const noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      documents_, [](const auto& item) { return !item.second.document.deleted; }));
}

FieldStatistics InMemoryIndex::field_statistics(std::string_view field) const noexcept {
  const auto statistic = field_statistics_.find(field);
  if (statistic == field_statistics_.end()) return {};
  return {.document_count = statistic->second.document_count,
          .total_length = statistic->second.total_length,
          .average_length = static_cast<double>(statistic->second.total_length) /
                            static_cast<double>(statistic->second.document_count)};
}

bool InMemoryIndex::validate_invariants(std::string* reason) const {
  const auto fail = [&](std::string value) { if (reason) *reason = std::move(value); return false; };
  std::map<std::string, FieldAccumulator, std::less<>> expected_statistics;
  for (const auto& [field, dictionary] : fields_) {
    for (const auto& [term, entry] : dictionary) {
      if (entry.document_frequency != entry.postings.size()) return fail("document frequency mismatch");
      if (!std::ranges::is_sorted(entry.postings, {}, &Posting::document_id)) return fail("unsorted postings");
      for (const auto& posting : entry.postings) {
        const auto* record = document(posting.document_id);
        if (record == nullptr || record->document.deleted) return fail("posting references missing/deleted document");
        if (posting.term_frequency != posting.positions.size()) return fail("term frequency mismatch");
        if (!std::ranges::is_sorted(posting.positions) ||
            std::ranges::adjacent_find(posting.positions) != posting.positions.end()) return fail("positions not strictly increasing");
        if (!record->field_lengths.contains(field)) return fail("field length missing");
        if (term.empty()) return fail("empty term");
      }
    }
  }
  for (const auto& [id, record] : documents_) {
    if (id != record.document.id) return fail("document key mismatch");
    const auto external = external_ids_.find(record.internal_id);
    if (external == external_ids_.end() || external->second != id) {
      return fail("document ID mapping mismatch");
    }
    if (record.document.deleted &&
        (!record.field_lengths.empty() || !record.indexed_terms.empty())) {
      return fail("tombstone has analyzed field state");
    }
    if (!record.document.deleted) {
      for (const auto& [field, text] : record.document.fields) {
        const auto* definition = schema_.find(field);
        if (definition == nullptr) return fail("document contains unknown field");
        if (!definition->indexed) continue;
        const auto expected = definition->analyzer->analyze(text).size();
        const auto it = record.field_lengths.find(field);
        if (it == record.field_lengths.end() || it->second != expected) return fail("field length mismatch");
        auto& statistic = expected_statistics[field];
        ++statistic.document_count;
        statistic.total_length += it->second;
        const auto indexed_terms = record.indexed_terms.find(field);
        if (indexed_terms == record.indexed_terms.end() ||
            !std::ranges::is_sorted(indexed_terms->second) ||
            std::ranges::adjacent_find(indexed_terms->second) != indexed_terms->second.end()) {
          return fail("document term references are missing, unsorted, or duplicated");
        }
        for (const auto& term : indexed_terms->second) {
          const auto* entry = lookup(field, term);
          if (entry == nullptr) return fail("document term reference is missing from dictionary");
          const auto posting = std::ranges::lower_bound(entry->postings, record.internal_id, {},
                                                        &Posting::document_id);
          if (posting == entry->postings.end() || posting->document_id != record.internal_id) {
            return fail("document term reference is missing its posting");
          }
        }
      }
    }
  }
  if (external_ids_.size() != documents_.size()) return fail("document mapping size mismatch");
  if (expected_statistics.size() != field_statistics_.size()) {
    return fail("field statistics size mismatch");
  }
  for (const auto& [field, expected] : expected_statistics) {
    const auto actual = field_statistics_.find(field);
    if (actual == field_statistics_.end() ||
        actual->second.document_count != expected.document_count ||
        actual->second.total_length != expected.total_length) {
      return fail("field statistics mismatch");
    }
  }
  return true;
}

}  // namespace dse::index
