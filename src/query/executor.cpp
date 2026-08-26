#include "dse/query/executor.hpp"

#include "dse/ranking/top_k.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace dse::query {
namespace {

struct Candidate { InternalDocumentId document_id; double score{}; };
using Candidates = std::vector<Candidate>;

ExecutionError error(ExecutionErrorCode code, std::string message) {
  return {code, std::move(message)};
}

ExecutionError planning_error(const PlanningError& planning) {
  switch (planning.code) {
    case PlanningErrorCode::invalid_options:
      return error(ExecutionErrorCode::invalid_options, planning.message);
    case PlanningErrorCode::invalid_query_tree:
      return error(ExecutionErrorCode::invalid_query_tree, planning.message);
    case PlanningErrorCode::unknown_field:
      return error(ExecutionErrorCode::unknown_field, planning.message);
    case PlanningErrorCode::incompatible_field_type:
    case PlanningErrorCode::invalid_range:
      return error(ExecutionErrorCode::incompatible_field_type, planning.message);
    case PlanningErrorCode::resource_limit:
    case PlanningErrorCode::analysis_failed:
      return error(ExecutionErrorCode::planning_error, planning.message);
  }
  return error(ExecutionErrorCode::planning_error, planning.message);
}

Candidates intersect(const Candidates& left, const Candidates& right) {
  Candidates result;
  std::size_t i = 0, j = 0;
  while (i < left.size() && j < right.size()) {
    if (left[i].document_id < right[j].document_id) ++i;
    else if (right[j].document_id < left[i].document_id) ++j;
    else { result.push_back({left[i].document_id, left[i].score + right[j].score}); ++i; ++j; }
  }
  return result;
}

Candidates unite(const Candidates& left, const Candidates& right) {
  Candidates result;
  std::size_t i = 0, j = 0;
  while (i < left.size() || j < right.size()) {
    if (j == right.size() || (i < left.size() && left[i].document_id < right[j].document_id))
      result.push_back(left[i++]);
    else if (i == left.size() || right[j].document_id < left[i].document_id)
      result.push_back(right[j++]);
    else { result.push_back({left[i].document_id, left[i].score + right[j].score}); ++i; ++j; }
  }
  return result;
}

Candidates exclude(const Candidates& universe, const Candidates& excluded) {
  Candidates result;
  std::size_t j = 0;
  for (const auto& candidate : universe) {
    while (j < excluded.size() && excluded[j].document_id < candidate.document_id) ++j;
    if (j == excluded.size() || candidate.document_id < excluded[j].document_id)
      result.push_back(candidate);
  }
  return result;
}

const index::Posting* posting_for(const index::TermEntry& entry, InternalDocumentId id) {
  const auto posting = std::ranges::lower_bound(entry.postings, id, {}, &index::Posting::document_id);
  return posting == entry.postings.end() || posting->document_id != id ? nullptr : &*posting;
}

bool positions_match(const std::vector<const index::Posting*>& postings,
                     const std::vector<analysis::Token>& tokens) {
  if (tokens.empty()) return false;
  for (const auto base : postings.front()->positions) {
    bool match = true;
    for (std::size_t i = 1; i < postings.size(); ++i) {
      const auto offset = tokens[i].position - tokens.front().position;
      if (base > std::numeric_limits<std::uint32_t>::max() - offset ||
          !std::ranges::binary_search(postings[i]->positions, base + offset)) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

class Evaluator {
 public:
  Evaluator(const index::SearchIndexView& index, const ranking::BM25Scorer& scorer)
      : index_(index), scorer_(scorer) {}

  std::expected<Candidates, ExecutionError> evaluate(const PlanNode& plan) const {
    return std::visit([&](const auto& node) { return evaluate_node(node); }, plan.value);
  }

 private:
  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedTerm& term) const {
    return score_term(term.field, term.term, term.boost);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedPhrase& phrase) const {
    if (phrase.tokens.empty()) return Candidates{};
    std::vector<const index::TermEntry*> entries;
    for (const auto& token : phrase.tokens) {
      const auto* entry = index_.lookup(phrase.field, token.term);
      if (entry == nullptr) return Candidates{};
      entries.push_back(entry);
    }
    std::size_t anchor = 0;
    for (std::size_t i = 1; i < entries.size(); ++i)
      if (entries[i]->postings.size() < entries[anchor]->postings.size()) anchor = i;
    Candidates result;
    for (const auto& anchor_posting : entries[anchor]->postings) {
      std::vector<const index::Posting*> postings(entries.size());
      postings[anchor] = &anchor_posting;
      bool all = true;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i == anchor) continue;
        postings[i] = posting_for(*entries[i], anchor_posting.document_id);
        if (postings[i] == nullptr) { all = false; break; }
      }
      if (!all || !positions_match(postings, phrase.tokens)) continue;
      double score = 0.0;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        auto term_score = score_posting(phrase.field, *entries[i], *postings[i], phrase.boost);
        if (!term_score) return std::unexpected(term_score.error());
        score += *term_score;
      }
      result.push_back({anchor_posting.document_id, score});
    }
    std::ranges::sort(result, {}, &Candidate::document_id);
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedAnd& node) const {
    if (node.children.empty()) return Candidates{};
    auto result = evaluate(*node.children.front());
    if (!result) return result;
    for (std::size_t i = 1; i < node.children.size(); ++i) {
      auto child = evaluate(*node.children[i]);
      if (!child) return child;
      *result = intersect(*result, *child);
      if (result->empty()) break;
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedOr& node) const {
    Candidates result;
    for (const auto& child_plan : node.children) {
      auto child = evaluate(*child_plan);
      if (!child) return child;
      result = unite(result, *child);
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedNot& node) const {
    if (!node.operand)
      return std::unexpected(error(ExecutionErrorCode::invalid_query_tree, "planned NOT is empty"));
    auto operand = evaluate(*node.operand);
    if (!operand) return operand;
    return exclude(universe(), *operand);
  }

  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedRange& range) const {
    const auto* definition = index_.schema().find(range.field);
    if (definition == nullptr)
      return std::unexpected(error(ExecutionErrorCode::invalid_query_tree, "unknown planned field"));
    Candidates result;
    for (const auto id : index_.live_document_ids()) {
      const auto* record = index_.document(id);
      const auto value = record == nullptr ? std::nullopt : field_value(*record, range.field);
      if (value && range_matches(*definition, *value, range)) result.push_back({id, 0.0});
    }
    return result;
  }

  std::expected<Candidates, ExecutionError> evaluate_node(const PlannedMatchAll&) const {
    return universe();
  }

  Candidates universe() const {
    Candidates result;
    for (const auto id : index_.live_document_ids()) result.push_back({id, 0.0});
    return result;
  }

  static std::optional<std::string_view> field_value(const index::DocumentRecord& record,
                                                      std::string_view field) {
    const auto indexed = record.document.fields.find(field);
    if (indexed != record.document.fields.end()) return indexed->second;
    const auto stored = record.document.stored_metadata.find(field);
    return stored == record.document.stored_metadata.end()
               ? std::nullopt : std::optional<std::string_view>(stored->second);
  }

  static bool range_matches(const index::FieldDefinition& definition, std::string_view value,
                            const PlannedRange& range) {
    if (definition.type == index::FieldType::int64) {
      std::int64_t actual = 0;
      if (std::from_chars(value.data(), value.data() + value.size(), actual).ec != std::errc{})
        return false;
      const auto compare = [&](const std::string& bound, bool lower) {
        if (bound == "*") return true;
        std::int64_t expected = 0;
        std::from_chars(bound.data(), bound.data() + bound.size(), expected);
        return lower ? (range.include_lower ? actual >= expected : actual > expected)
                     : (range.include_upper ? actual <= expected : actual < expected);
      };
      return compare(range.lower_bound, true) && compare(range.upper_bound, false);
    }
    const bool above = range.lower_bound == "*" ||
        (range.include_lower ? value >= range.lower_bound : value > range.lower_bound);
    const bool below = range.upper_bound == "*" ||
        (range.include_upper ? value <= range.upper_bound : value < range.upper_bound);
    return above && below;
  }

  std::expected<double, ExecutionError> score_posting(std::string_view field,
      const index::TermEntry& entry, const index::Posting& posting, double boost) const {
    const auto* document = index_.document(posting.document_id);
    if (document == nullptr)
      return std::unexpected(error(ExecutionErrorCode::invalid_query_tree, "unknown posting document"));
    const auto length = document->field_lengths.find(field);
    if (length == document->field_lengths.end())
      return std::unexpected(error(ExecutionErrorCode::invalid_query_tree, "missing field length"));
    const auto stats = index_.field_statistics(field);
    auto score = scorer_.score({stats.document_count, entry.document_frequency,
                                posting.term_frequency, length->second, stats.average_length}, boost);
    if (!score) return std::unexpected(error(ExecutionErrorCode::ranking_error,
                                             std::string(ranking::describe(score.error()))));
    return *score;
  }

  std::expected<Candidates, ExecutionError> score_term(std::string_view field,
      std::string_view term, double boost) const {
    const auto* entry = index_.lookup(field, term);
    if (entry == nullptr) return Candidates{};
    Candidates result;
    for (const auto& posting : entry->postings) {
      auto score = score_posting(field, *entry, posting, boost);
      if (!score) return std::unexpected(score.error());
      result.push_back({posting.document_id, *score});
    }
    return result;
  }

  const index::SearchIndexView& index_;
  const ranking::BM25Scorer& scorer_;
};

}  // namespace

QueryExecutor::QueryExecutor(const index::SearchIndexView& index, ranking::BM25Parameters parameters)
    : index_(index), scorer_(ranking::BM25Scorer::create(parameters)) {}

std::expected<SearchResult, ExecutionError> QueryExecutor::search(
    const QueryNode& query, const SearchOptions& options) const {
  auto plan = QueryPlanner(index_).plan(
      query, {.default_fields = options.default_fields, .limits = options.planner_limits});
  if (!plan) return std::unexpected(planning_error(plan.error()));
  return search(*plan, options.top_k);
}

std::expected<SearchResult, ExecutionError> QueryExecutor::search(
    const PlannedQuery& query, std::size_t top_k) const {
  if (!scorer_) return std::unexpected(error(ExecutionErrorCode::invalid_options,
                                              std::string(ranking::describe(scorer_.error()))));
  auto candidates = Evaluator(index_, *scorer_).evaluate(query.root());
  if (!candidates) return std::unexpected(candidates.error());
  ranking::TopKCollector collector(top_k);
  for (const auto& candidate : *candidates) {
    const auto* external = index_.external_id(candidate.document_id);
    if (external == nullptr)
      return std::unexpected(error(ExecutionErrorCode::invalid_query_tree, "missing external ID"));
    if (!collector.collect({*external, candidate.score}))
      return std::unexpected(error(ExecutionErrorCode::ranking_error, "non-finite score"));
  }
  SearchResult result{.hits = {}, .total_hits = candidates->size()};
  for (const auto& scored : collector.results()) result.hits.push_back({scored.document_id, scored.score});
  return result;
}

}  // namespace dse::query
