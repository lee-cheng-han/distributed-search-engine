#include "reference_evaluator.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace dse::test {
namespace {

struct Match {
  bool matched{};
  double score{};
};

std::optional<std::string_view> value(const index::DocumentRecord& record,
                                      std::string_view field) {
  const auto indexed = record.document.fields.find(field);
  if (indexed != record.document.fields.end()) return indexed->second;
  const auto stored = record.document.stored_metadata.find(field);
  if (stored != record.document.stored_metadata.end()) return stored->second;
  return std::nullopt;
}

std::vector<analysis::Token> tokens(const index::InMemoryIndex& index,
                                    const index::DocumentRecord& record,
                                    std::string_view field) {
  const auto* definition = index.schema().find(field);
  const auto field_value = value(record, field);
  if (definition == nullptr || !definition->analyzer || !field_value) return {};
  return definition->analyzer->analyze(*field_value);
}

std::uint32_t frequency(const std::vector<analysis::Token>& analyzed, std::string_view term) {
  return static_cast<std::uint32_t>(std::ranges::count(analyzed, term, &analysis::Token::term));
}

class Oracle {
 public:
  Oracle(const index::InMemoryIndex& index, ranking::BM25Parameters parameters)
      : index_(index), parameters_(parameters) {}

  Match evaluate(const query::PlanNode& plan, const index::DocumentRecord& document) const {
    return std::visit([&](const auto& node) { return evaluate_node(node, document); }, plan.value);
  }

 private:
  Match evaluate_node(const query::PlannedTerm& term,
                      const index::DocumentRecord& document) const {
    const auto analyzed = tokens(index_, document, term.field);
    const auto tf = frequency(analyzed, term.term);
    return {tf != 0U, tf == 0U ? 0.0 : score(term.field, term.term, tf,
                                             analyzed.size(), term.boost)};
  }

  Match evaluate_node(const query::PlannedPhrase& phrase,
                      const index::DocumentRecord& document) const {
    if (phrase.tokens.empty()) return {};
    const auto analyzed = tokens(index_, document, phrase.field);
    bool matched = false;
    for (const auto& candidate : analyzed) {
      if (candidate.term != phrase.tokens.front().term) continue;
      matched = true;
      for (std::size_t i = 1; i < phrase.tokens.size(); ++i) {
        const auto offset = phrase.tokens[i].position - phrase.tokens.front().position;
        const auto found = std::ranges::find_if(analyzed, [&](const analysis::Token& token) {
          return token.term == phrase.tokens[i].term &&
                 token.position == candidate.position + offset;
        });
        if (found == analyzed.end()) { matched = false; break; }
      }
      if (matched) break;
    }
    if (!matched) return {};
    double total = 0.0;
    for (const auto& token : phrase.tokens) {
      total += score(phrase.field, token.term, frequency(analyzed, token.term),
                     analyzed.size(), phrase.boost);
    }
    return {true, total};
  }

  Match evaluate_node(const query::PlannedAnd& conjunction,
                      const index::DocumentRecord& document) const {
    if (conjunction.children.empty()) return {};
    double total = 0.0;
    for (const auto& child : conjunction.children) {
      const auto result = evaluate(*child, document);
      if (!result.matched) return {};
      total += result.score;
    }
    return {true, total};
  }

  Match evaluate_node(const query::PlannedOr& disjunction,
                      const index::DocumentRecord& document) const {
    bool matched = false;
    double total = 0.0;
    for (const auto& child : disjunction.children) {
      const auto result = evaluate(*child, document);
      matched = matched || result.matched;
      total += result.score;
    }
    return {matched, total};
  }

  Match evaluate_node(const query::PlannedNot& negation,
                      const index::DocumentRecord& document) const {
    return {!evaluate(*negation.operand, document).matched, 0.0};
  }

  Match evaluate_node(const query::PlannedRange& range,
                      const index::DocumentRecord& document) const {
    const auto actual = value(document, range.field);
    const auto* definition = index_.schema().find(range.field);
    if (!actual || definition == nullptr) return {};
    if (definition->type == index::FieldType::int64) {
      std::int64_t number{};
      std::from_chars(actual->data(), actual->data() + actual->size(), number);
      const auto accepts = [&](const std::string& bound, bool lower) {
        if (bound == "*") return true;
        std::int64_t limit{};
        std::from_chars(bound.data(), bound.data() + bound.size(), limit);
        return lower ? (range.include_lower ? number >= limit : number > limit)
                     : (range.include_upper ? number <= limit : number < limit);
      };
      return {accepts(range.lower_bound, true) && accepts(range.upper_bound, false), 0.0};
    }
    const bool lower = range.lower_bound == "*" ||
        (range.include_lower ? *actual >= range.lower_bound : *actual > range.lower_bound);
    const bool upper = range.upper_bound == "*" ||
        (range.include_upper ? *actual <= range.upper_bound : *actual < range.upper_bound);
    return {lower && upper, 0.0};
  }

  Match evaluate_node(const query::PlannedMatchAll&,
                      const index::DocumentRecord&) const { return {true, 0.0}; }

  double score(std::string_view field, std::string_view term, std::uint32_t tf,
               std::size_t document_length, double boost) const {
    std::size_t documents = 0;
    std::size_t document_frequency = 0;
    std::size_t total_length = 0;
    for (const auto id : index_.live_document_ids()) {
      const auto* record = index_.document(id);
      const auto analyzed = tokens(index_, *record, field);
      if (value(*record, field)) {
        ++documents;
        total_length += analyzed.size();
      }
      if (frequency(analyzed, term) != 0U) ++document_frequency;
    }
    if (tf == 0U || documents == 0U || document_frequency == 0U) return 0.0;
    const double count = static_cast<double>(documents);
    const double df = static_cast<double>(document_frequency);
    const double average = static_cast<double>(total_length) / count;
    const double length_ratio = static_cast<double>(document_length) / average;
    const double normalization =
        parameters_.k1 * (1.0 - parameters_.b + parameters_.b * length_ratio);
    const double frequency_weight = static_cast<double>(tf) * (parameters_.k1 + 1.0) /
        (static_cast<double>(tf) + normalization);
    return std::log1p((count - df + 0.5) / (df + 0.5)) * frequency_weight * boost;
  }

  const index::InMemoryIndex& index_;
  ranking::BM25Parameters parameters_;
};

}  // namespace

ReferenceEvaluator::ReferenceEvaluator(const index::InMemoryIndex& index,
                                       ranking::BM25Parameters parameters)
    : index_(index), parameters_(parameters) {}

query::SearchResult ReferenceEvaluator::search(const query::PlannedQuery& query,
                                                std::size_t top_k) const {
  std::vector<query::SearchHit> hits;
  const Oracle oracle(index_, parameters_);
  for (const auto id : index_.live_document_ids()) {
    const auto* record = index_.document(id);
    const auto result = oracle.evaluate(query.root(), *record);
    if (result.matched) hits.push_back({record->document.id, result.score});
  }
  std::ranges::sort(hits, [](const query::SearchHit& left, const query::SearchHit& right) {
    if (left.score != right.score) return left.score > right.score;
    return left.document_id < right.document_id;
  });
  const auto total_hits = hits.size();
  if (hits.size() > top_k) hits.resize(top_k);
  return {.hits = std::move(hits), .total_hits = total_hits};
}

}  // namespace dse::test
