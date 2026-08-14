#include "dse/query/executor.hpp"

#include "dse/ranking/top_k.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace dse::query {
namespace {

constexpr std::size_t kMaximumExecutionDepth = 256;

struct Candidate {
  InternalDocumentId document_id;
  double score{};
};

using Candidates = std::vector<Candidate>;

ExecutionError error(ExecutionErrorCode code, std::string message) {
  return {code, std::move(message)};
}

Candidates intersect(const Candidates& left, const Candidates& right) {
  Candidates result;
  result.reserve(std::min(left.size(), right.size()));
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() && right_index < right.size()) {
    if (left[left_index].document_id < right[right_index].document_id) {
      ++left_index;
    } else if (right[right_index].document_id < left[left_index].document_id) {
      ++right_index;
    } else {
      result.push_back(
          {left[left_index].document_id, left[left_index].score + right[right_index].score});
      ++left_index;
      ++right_index;
    }
  }
  return result;
}

Candidates unite(const Candidates& left, const Candidates& right) {
  Candidates result;
  result.reserve(left.size() + right.size());
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() || right_index < right.size()) {
    if (right_index == right.size() ||
        (left_index < left.size() &&
         left[left_index].document_id < right[right_index].document_id)) {
      result.push_back(left[left_index++]);
    } else if (left_index == left.size() ||
               right[right_index].document_id < left[left_index].document_id) {
      result.push_back(right[right_index++]);
    } else {
      result.push_back(
          {left[left_index].document_id, left[left_index].score + right[right_index].score});
      ++left_index;
      ++right_index;
    }
  }
  return result;
}

Candidates exclude(const Candidates& universe, const Candidates& excluded) {
  Candidates result;
  result.reserve(universe.size());
  std::size_t excluded_index = 0;
  for (const auto& candidate : universe) {
    while (excluded_index < excluded.size() &&
           excluded[excluded_index].document_id < candidate.document_id) {
      ++excluded_index;
    }
    if (excluded_index == excluded.size() ||
        candidate.document_id < excluded[excluded_index].document_id) {
      result.push_back(candidate);
    }
  }
  return result;
}

const index::Posting* posting_for(const index::TermEntry& entry, const InternalDocumentId& id) {
  const auto posting = std::ranges::lower_bound(entry.postings, id, {}, &index::Posting::document_id);
  return posting == entry.postings.end() || posting->document_id != id ? nullptr : &*posting;
}

bool positions_match(const std::vector<const index::Posting*>& postings,
                     const std::vector<analysis::Token>& tokens) {
  const auto base_query_position = tokens.front().position;
  for (const auto base_document_position : postings.front()->positions) {
    bool matches = true;
    for (std::size_t index = 1; index < postings.size(); ++index) {
      const auto offset = tokens[index].position - base_query_position;
      if (base_document_position > std::numeric_limits<std::uint32_t>::max() - offset ||
          !std::ranges::binary_search(postings[index]->positions,
                                      base_document_position + offset)) {
        matches = false;
        break;
      }
    }
    if (matches) return true;
  }
  return false;
}

class Evaluator {
 public:
  Evaluator(const index::InMemoryIndex& index, const ranking::BM25Scorer& scorer,
            const SearchOptions& options)
      : index_(index), scorer_(scorer), options_(options) {}

  std::expected<Candidates, ExecutionError> evaluate(const QueryNode& query,
                                                      std::optional<std::string_view> field = {},
                                                      std::size_t depth = 0) const {
    if (depth > kMaximumExecutionDepth) {
      return std::unexpected(error(ExecutionErrorCode::nesting_too_deep,
                                   "query execution nesting limit exceeded"));
    }
    return std::visit(
        [&](const auto& node) { return evaluate_node(node, field, depth); }, query.value);
  }

 private:
  std::expected<Candidates, ExecutionError> evaluate_node(
      const TermQuery& query, std::optional<std::string_view> field, std::size_t) const {
    Candidates result;
    if (field) return score_text(*field, query.term, field_boost(*field));
    for (const auto& search_field : options_.default_fields) {
      auto term_result = score_text(search_field.name, query.term, search_field.boost);
      if (!term_result) return term_result;
      result = unite(result, *term_result);
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> score_text(std::string_view field,
                                                       std::string_view text,
                                                       double boost) const {
    const auto* definition = index_.schema().find(field);
    if (definition == nullptr) {
      return std::unexpected(error(ExecutionErrorCode::unknown_field,
                                   "query references unknown field: " + std::string(field)));
    }
    if (!definition->indexed || !definition->analyzer) {
      return std::unexpected(error(ExecutionErrorCode::incompatible_field_type,
                                   "field is not searchable as text: " + std::string(field)));
    }
    Candidates result;
    for (const auto& token : definition->analyzer->analyze(text)) {
      auto term_result = score_term(field, token.term, boost * definition->boost);
      if (!term_result) return term_result;
      result = unite(result, *term_result);
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> validate_phrase_field(
      std::string_view field, std::string_view phrase, double boost) const {
    const auto* definition = index_.schema().find(field);
    if (definition == nullptr) {
      return std::unexpected(error(ExecutionErrorCode::unknown_field,
                                   "query references unknown field: " + std::string(field)));
    }
    if (!definition->indexed || !definition->analyzer) {
      return std::unexpected(error(ExecutionErrorCode::incompatible_field_type,
                                   "field does not support phrase search: " + std::string(field)));
    }
    return score_phrase(field, phrase, boost * definition->boost, *definition->analyzer);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const PhraseQuery& query, std::optional<std::string_view> field, std::size_t) const {
    if (field) return validate_phrase_field(*field, query.text, field_boost(*field));
    Candidates result;
    for (const auto& search_field : options_.default_fields) {
      auto field_result =
          validate_phrase_field(search_field.name, query.text, search_field.boost);
      if (!field_result) return field_result;
      result = unite(result, *field_result);
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const AndQuery& query, std::optional<std::string_view> field, std::size_t depth) const {
    if (!query.left || !query.right) return invalid_tree();
    auto left = evaluate(*query.left, field, depth + 1);
    if (!left) return left;
    auto right = evaluate(*query.right, field, depth + 1);
    if (!right) return right;
    return intersect(*left, *right);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const OrQuery& query, std::optional<std::string_view> field, std::size_t depth) const {
    if (!query.left || !query.right) return invalid_tree();
    auto left = evaluate(*query.left, field, depth + 1);
    if (!left) return left;
    auto right = evaluate(*query.right, field, depth + 1);
    if (!right) return right;
    return unite(*left, *right);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const NotQuery& query, std::optional<std::string_view> field, std::size_t depth) const {
    if (!query.operand) return invalid_tree();
    auto operand = evaluate(*query.operand, field, depth + 1);
    if (!operand) return operand;
    return exclude(universe(), *operand);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const FieldQuery& query, std::optional<std::string_view>, std::size_t depth) const {
    if (!query.query || query.field.empty()) return invalid_tree();
    return evaluate(*query.query, query.field, depth + 1);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const RangeFilter& query, std::optional<std::string_view>, std::size_t) const {
    const auto* definition = index_.schema().find(query.field);
    if (definition == nullptr) {
      return std::unexpected(error(ExecutionErrorCode::unknown_field,
                                   "range references unknown field: " + query.field));
    }
    if (definition->type == index::FieldType::text) {
      return std::unexpected(error(ExecutionErrorCode::incompatible_field_type,
                                   "text fields do not support range filters"));
    }
    if (query.lower_bound != "*") {
      auto validation = index_.schema().validate_value(query.field, query.lower_bound);
      if (!validation) {
        return std::unexpected(error(ExecutionErrorCode::incompatible_field_type,
                                     validation.error().message));
      }
    }
    if (query.upper_bound != "*") {
      auto validation = index_.schema().validate_value(query.field, query.upper_bound);
      if (!validation) {
        return std::unexpected(error(ExecutionErrorCode::incompatible_field_type,
                                     validation.error().message));
      }
    }
    Candidates result;
    for (const auto& id : index_.live_document_ids()) {
      const auto* record = index_.document(id);
      if (record == nullptr) continue;
      const auto value = field_value(*record, query.field);
      if (!value) continue;
      const bool above_lower = query.lower_bound == "*" ||
                               (query.include_lower ? *value >= query.lower_bound
                                                    : *value > query.lower_bound);
      const bool below_upper = query.upper_bound == "*" ||
                               (query.include_upper ? *value <= query.upper_bound
                                                    : *value < query.upper_bound);
      if (above_lower && below_upper) result.push_back({id, 0.0});
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const BoostQuery& query, std::optional<std::string_view> field, std::size_t depth) const {
    if (!query.query || !std::isfinite(query.boost) || query.boost <= 0.0) {
      return invalid_tree();
    }
    auto result = evaluate(*query.query, field, depth + 1);
    if (!result) return result;
    for (auto& candidate : *result) candidate.score *= query.boost;
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(
      const MatchAllQuery&, std::optional<std::string_view>, std::size_t) const {
    return universe();
  }

  Candidates universe() const {
    Candidates result;
    const auto ids = index_.live_document_ids();
    result.reserve(ids.size());
    for (const auto& id : ids) result.push_back({id, 0.0});
    return result;
  }

  static std::expected<Candidates, ExecutionError> invalid_tree() {
    return std::unexpected(error(ExecutionErrorCode::invalid_query_tree,
                                 "query AST contains an invalid node"));
  }

  double field_boost(std::string_view field) const {
    const auto match = std::ranges::find(options_.default_fields, field, &SearchField::name);
    return match == options_.default_fields.end() ? 1.0 : match->boost;
  }

  static std::optional<std::string_view> field_value(const index::DocumentRecord& record,
                                                      std::string_view field) {
    const auto indexed = record.document.fields.find(field);
    if (indexed != record.document.fields.end()) return indexed->second;
    const auto metadata = record.document.stored_metadata.find(field);
    if (metadata != record.document.stored_metadata.end()) return metadata->second;
    return std::nullopt;
  }

  std::expected<Candidates, ExecutionError> score_term(std::string_view field,
                                                       std::string_view term,
                                                       double boost) const {
    const auto* entry = index_.lookup(field, term);
    if (entry == nullptr) return Candidates{};
    const auto field_statistics = index_.field_statistics(field);
    Candidates result;
    result.reserve(entry->postings.size());
    for (const auto& posting : entry->postings) {
      const auto* document = index_.document(posting.document_id);
      if (document == nullptr) {
        return std::unexpected(error(ExecutionErrorCode::invalid_query_tree,
                                     "posting references an unknown document"));
      }
      const auto length = document->field_lengths.find(field);
      if (length == document->field_lengths.end()) {
        return std::unexpected(error(ExecutionErrorCode::invalid_query_tree,
                                     "posting document is missing its field length"));
      }
      auto score = scorer_.score({.document_count = field_statistics.document_count,
                                  .document_frequency = entry->document_frequency,
                                  .term_frequency = posting.term_frequency,
                                  .document_length = length->second,
                                  .average_document_length = field_statistics.average_length},
                                 boost);
      if (!score) {
        return std::unexpected(error(ExecutionErrorCode::ranking_error,
                                     std::string(ranking::describe(score.error()))));
      }
      result.push_back({posting.document_id, *score});
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> score_phrase(std::string_view field,
                                                         std::string_view phrase,
                                                         double boost,
                                                         const analysis::Analyzer& analyzer) const {
    const auto tokens = analyzer.analyze(phrase);
    if (tokens.empty()) return Candidates{};
    std::vector<const index::TermEntry*> entries;
    entries.reserve(tokens.size());
    for (const auto& token : tokens) {
      const auto* entry = index_.lookup(field, token.term);
      if (entry == nullptr) return Candidates{};
      entries.push_back(entry);
    }

    Candidates result;
    for (const auto& first_posting : entries.front()->postings) {
      std::vector<const index::Posting*> postings{&first_posting};
      bool contains_all_terms = true;
      for (std::size_t index = 1; index < entries.size(); ++index) {
        const auto* posting = posting_for(*entries[index], first_posting.document_id);
        if (posting == nullptr) {
          contains_all_terms = false;
          break;
        }
        postings.push_back(posting);
      }
      if (!contains_all_terms || !positions_match(postings, tokens)) continue;

      double phrase_score = 0.0;
      for (std::size_t index = 0; index < tokens.size(); ++index) {
        auto term_scores = score_term(field, tokens[index].term, boost);
        if (!term_scores) return term_scores;
        const auto scored = std::ranges::lower_bound(*term_scores, first_posting.document_id, {},
                                                     &Candidate::document_id);
        if (scored != term_scores->end() && scored->document_id == first_posting.document_id) {
          phrase_score += scored->score;
        }
      }
      result.push_back({first_posting.document_id, phrase_score});
    }
    return result;
  }

  const index::InMemoryIndex& index_;
  const ranking::BM25Scorer& scorer_;
  const SearchOptions& options_;
};

}  // namespace

QueryExecutor::QueryExecutor(const index::InMemoryIndex& index,
                             ranking::BM25Parameters parameters)
    : index_(index), scorer_(ranking::BM25Scorer::create(parameters)) {}

std::expected<SearchResult, ExecutionError> QueryExecutor::search(const QueryNode& query,
                                                                  const SearchOptions& options) const {
  if (!scorer_) {
    return std::unexpected(error(ExecutionErrorCode::invalid_options,
                                 std::string(ranking::describe(scorer_.error()))));
  }
  for (const auto& field : options.default_fields) {
    if (field.name.empty() || !std::isfinite(field.boost) || field.boost < 0.0) {
      return std::unexpected(error(ExecutionErrorCode::invalid_options,
                                   "search fields require a name and finite non-negative boost"));
    }
  }

  auto candidates = Evaluator(index_, *scorer_, options).evaluate(query);
  if (!candidates) return std::unexpected(candidates.error());
  ranking::TopKCollector collector(options.top_k);
    for (const auto& candidate : *candidates) {
    const auto* external_id = index_.external_id(candidate.document_id);
    if (external_id == nullptr) {
      return std::unexpected(
          error(ExecutionErrorCode::invalid_query_tree, "candidate has no external document ID"));
    }
    if (!collector.collect({*external_id, candidate.score})) {
      return std::unexpected(
          error(ExecutionErrorCode::ranking_error, "query produced a non-finite score"));
    }
  }
  const auto scored = collector.results();
  SearchResult result{.hits = {}, .total_hits = candidates->size()};
  result.hits.reserve(scored.size());
  for (const auto& candidate : scored) {
    result.hits.push_back({candidate.document_id, candidate.score});
  }
  return result;
}

}  // namespace dse::query
