#include "dse/query/planner.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dse::query {
namespace {

PlanningError error(PlanningErrorCode code, std::string message) {
  return {code, std::move(message)};
}

std::string atom(std::string_view value) {
  return std::to_string(value.size()) + ":" + std::string(value);
}

std::string canonical(const PlanNode& node) {
  return std::visit(
      [&](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PlannedTerm>) {
          std::ostringstream output;
          output.precision(17);
          output << "T(" << atom(value.field) << ',' << atom(value.term) << ',' << value.boost
                 << ')';
          return output.str();
        } else if constexpr (std::is_same_v<T, PlannedPhrase>) {
          std::ostringstream output;
          output.precision(17);
          output << "P(" << atom(value.field) << ',' << value.boost;
          for (const auto& token : value.tokens) {
            output << ',' << atom(token.term) << '@' << token.position;
          }
          return output.str() + ')';
        } else if constexpr (std::is_same_v<T, PlannedAnd> ||
                             std::is_same_v<T, PlannedOr>) {
          std::string output = std::is_same_v<T, PlannedAnd> ? "A(" : "O(";
          for (const auto& child : value.children) output += canonical(*child) + ',';
          return output + ')';
        } else if constexpr (std::is_same_v<T, PlannedNot>) {
          return "N(" + canonical(*value.operand) + ')';
        } else if constexpr (std::is_same_v<T, PlannedRange>) {
          return "R(" + atom(value.field) + ',' + atom(value.lower_bound) + ',' +
                 atom(value.upper_bound) + ',' + (value.include_lower ? "1" : "0") + ',' +
                 (value.include_upper ? "1" : "0") + ')';
        } else {
          return "M()";
        }
      },
      node.value);
}

class Builder {
 public:
  Builder(const index::SearchIndexView& index, const PlannerOptions& options)
      : index_(index), options_(options) {}

  std::expected<Plan, PlanningError> build(const QueryNode& query,
                                           std::optional<std::string_view> field = {},
                                           double boost = 1.0, std::size_t depth = 0) {
    if (depth > options_.limits.maximum_depth) {
      return std::unexpected(
          error(PlanningErrorCode::resource_limit, "query planning depth limit exceeded"));
    }
    if (++source_nodes_ > options_.limits.maximum_nodes) {
      return std::unexpected(
          error(PlanningErrorCode::resource_limit, "query AST node limit exceeded"));
    }
    return std::visit(
        [&](const auto& node) { return build_node(node, field, boost, depth); }, query.value);
  }

 private:
  std::expected<Plan, PlanningError> make(PlanVariant value, std::size_t estimate) {
    if (++planned_nodes_ > options_.limits.maximum_nodes) {
      return std::unexpected(
          error(PlanningErrorCode::resource_limit, "planned query node limit exceeded"));
    }
    return std::make_unique<PlanNode>(PlanNode{std::move(value), estimate});
  }

  std::expected<const index::FieldDefinition*, PlanningError> searchable_field(
      std::string_view field) const {
    const auto* definition = index_.schema().find(field);
    if (definition == nullptr) {
      return std::unexpected(error(PlanningErrorCode::unknown_field,
                                   "query references unknown field: " + std::string(field)));
    }
    if (!definition->indexed || !definition->analyzer) {
      return std::unexpected(error(PlanningErrorCode::incompatible_field_type,
                                   "field is not searchable as text: " + std::string(field)));
    }
    return definition;
  }

  std::expected<std::vector<analysis::Token>, PlanningError> analyze(
      const index::FieldDefinition& field, std::string_view text, bool phrase) {
    try {
      auto tokens = field.analyzer->analyze(text);
      analyzed_tokens_ += tokens.size();
      if (analyzed_tokens_ > options_.limits.maximum_analyzed_tokens ||
          (phrase && tokens.size() > options_.limits.maximum_phrase_tokens)) {
        return std::unexpected(
            error(PlanningErrorCode::resource_limit, "analyzed query token limit exceeded"));
      }
      return tokens;
    } catch (const std::exception& exception) {
      return std::unexpected(error(PlanningErrorCode::analysis_failed,
                                   "query analysis failed: " + std::string(exception.what())));
    }
  }

  std::expected<Plan, PlanningError> term_for_field(std::string_view field, std::string_view text,
                                                    double boost) {
    auto definition = searchable_field(field);
    if (!definition) return std::unexpected(definition.error());
    auto tokens = analyze(**definition, text, false);
    if (!tokens) return std::unexpected(tokens.error());
    std::vector<Plan> children;
    std::set<std::string, std::less<>> seen;
    for (const auto& token : *tokens) {
      if (!seen.insert(token.term).second) continue;
      const auto* entry = index_.lookup(field, token.term);
      const auto estimate = entry == nullptr ? 0U : entry->postings.size();
      auto child = make(PlannedTerm{std::string(field), token.term,
                                    boost * (*definition)->boost},
                        estimate);
      if (!child) return child;
      children.push_back(std::move(*child));
    }
    return normalize_or(std::move(children));
  }

  std::expected<Plan, PlanningError> phrase_for_field(std::string_view field,
                                                      std::string_view text, double boost) {
    auto definition = searchable_field(field);
    if (!definition) return std::unexpected(definition.error());
    auto tokens = analyze(**definition, text, true);
    if (!tokens) return std::unexpected(tokens.error());
    std::size_t estimate = index_.live_document_count();
    for (const auto& token : *tokens) {
      const auto* entry = index_.lookup(field, token.term);
      estimate = std::min(estimate, entry == nullptr ? 0U : entry->postings.size());
    }
    return make(PlannedPhrase{std::string(field), std::move(*tokens),
                              boost * (*definition)->boost},
                estimate);
  }

  std::expected<Plan, PlanningError> build_node(const TermQuery& query,
                                                std::optional<std::string_view> field,
                                                double boost, std::size_t) {
    if (field) return term_for_field(*field, query.term, boost);
    std::vector<Plan> children;
    for (const auto& search_field : options_.default_fields) {
      auto child = term_for_field(search_field.name, query.term, boost * search_field.boost);
      if (!child) return child;
      children.push_back(std::move(*child));
    }
    return normalize_or(std::move(children));
  }

  std::expected<Plan, PlanningError> build_node(const PhraseQuery& query,
                                                std::optional<std::string_view> field,
                                                double boost, std::size_t) {
    if (field) return phrase_for_field(*field, query.text, boost);
    std::vector<Plan> children;
    for (const auto& search_field : options_.default_fields) {
      auto child = phrase_for_field(search_field.name, query.text, boost * search_field.boost);
      if (!child) return child;
      children.push_back(std::move(*child));
    }
    return normalize_or(std::move(children));
  }

  std::expected<Plan, PlanningError> build_node(const AndQuery& query,
                                                std::optional<std::string_view> field,
                                                double boost, std::size_t depth) {
    if (!query.left || !query.right) return invalid_tree();
    std::vector<Plan> children;
    auto left = build(*query.left, field, boost, depth + 1);
    if (!left) return left;
    auto right = build(*query.right, field, boost, depth + 1);
    if (!right) return right;
    children.push_back(std::move(*left));
    children.push_back(std::move(*right));
    return normalize_boolean<PlannedAnd>(std::move(children), true);
  }

  std::expected<Plan, PlanningError> build_node(const OrQuery& query,
                                                std::optional<std::string_view> field,
                                                double boost, std::size_t depth) {
    if (!query.left || !query.right) return invalid_tree();
    std::vector<Plan> children;
    auto left = build(*query.left, field, boost, depth + 1);
    if (!left) return left;
    auto right = build(*query.right, field, boost, depth + 1);
    if (!right) return right;
    children.push_back(std::move(*left));
    children.push_back(std::move(*right));
    return normalize_or(std::move(children));
  }

  std::expected<Plan, PlanningError> build_node(const NotQuery& query,
                                                std::optional<std::string_view> field,
                                                double boost, std::size_t depth) {
    if (!query.operand) return invalid_tree();
    auto operand = build(*query.operand, field, boost, depth + 1);
    if (!operand) return operand;
    return make(PlannedNot{std::move(*operand)}, index_.live_document_count());
  }

  std::expected<Plan, PlanningError> build_node(const FieldQuery& query,
                                                std::optional<std::string_view>, double boost,
                                                std::size_t depth) {
    if (!query.query || query.field.empty()) return invalid_tree();
    if (index_.schema().find(query.field) == nullptr) {
      return std::unexpected(error(PlanningErrorCode::unknown_field,
                                   "query references unknown field: " + query.field));
    }
    return build(*query.query, query.field, boost, depth + 1);
  }

  std::expected<Plan, PlanningError> build_node(const RangeFilter& query,
                                                std::optional<std::string_view>, double,
                                                std::size_t) {
    const auto* definition = index_.schema().find(query.field);
    if (definition == nullptr) {
      return std::unexpected(error(PlanningErrorCode::unknown_field,
                                   "range references unknown field: " + query.field));
    }
    if (definition->type == index::FieldType::text) {
      return std::unexpected(error(PlanningErrorCode::incompatible_field_type,
                                   "text fields do not support range filters"));
    }
    std::string lower = query.lower_bound;
    std::string upper = query.upper_bound;
    for (auto* bound : {&lower, &upper}) {
      if (*bound == "*") continue;
      auto validation = index_.schema().validate_value(query.field, *bound);
      if (!validation) {
        return std::unexpected(
            error(PlanningErrorCode::invalid_range, validation.error().message));
      }
      if (definition->type == index::FieldType::int64) {
        std::int64_t value = 0;
        std::from_chars(bound->data(), bound->data() + bound->size(), value);
        *bound = std::to_string(value);
      }
    }
    return make(PlannedRange{query.field, std::move(lower), std::move(upper),
                             query.include_lower, query.include_upper},
                index_.live_document_count());
  }

  std::expected<Plan, PlanningError> build_node(const BoostQuery& query,
                                                std::optional<std::string_view> field,
                                                double boost, std::size_t depth) {
    if (!query.query || !std::isfinite(query.boost) || query.boost <= 0.0) {
      return invalid_tree();
    }
    return build(*query.query, field, boost * query.boost, depth + 1);
  }

  std::expected<Plan, PlanningError> build_node(const MatchAllQuery&,
                                                std::optional<std::string_view>, double,
                                                std::size_t) {
    return make(PlannedMatchAll{}, index_.live_document_count());
  }

  std::expected<Plan, PlanningError> normalize_or(std::vector<Plan> children) {
    return normalize_boolean<PlannedOr>(std::move(children), false);
  }

  template <typename BooleanNode>
  std::expected<Plan, PlanningError> normalize_boolean(std::vector<Plan> children,
                                                       bool cost_order) {
    std::vector<std::pair<std::string, Plan>> keyed;
    for (auto& child : children) {
      const auto key = canonical(*child);
      if (std::ranges::find(keyed, key, &std::pair<std::string, Plan>::first) == keyed.end()) {
        keyed.emplace_back(key, std::move(child));
      }
    }
    if (keyed.empty()) return make(BooleanNode{}, 0);
    if (keyed.size() == 1U) return std::move(keyed.front().second);
    if (cost_order) {
      std::ranges::sort(keyed, [](const auto& left, const auto& right) {
        if (left.second->estimated_matches != right.second->estimated_matches) {
          return left.second->estimated_matches < right.second->estimated_matches;
        }
        return left.first < right.first;
      });
    } else {
      std::ranges::sort(keyed, {}, &std::pair<std::string, Plan>::first);
    }
    std::vector<Plan> normalized;
    std::size_t estimate = cost_order ? index_.live_document_count() : 0U;
    for (auto& [key, child] : keyed) {
      (void)key;
      estimate = cost_order ? std::min(estimate, child->estimated_matches)
                            : std::min(index_.live_document_count(),
                                       estimate + child->estimated_matches);
      normalized.push_back(std::move(child));
    }
    return make(BooleanNode{std::move(normalized)}, estimate);
  }

  std::expected<Plan, PlanningError> invalid_tree() const {
    return std::unexpected(
        error(PlanningErrorCode::invalid_query_tree, "query AST contains an invalid node"));
  }

  const index::SearchIndexView& index_;
  const PlannerOptions& options_;
  std::size_t source_nodes_{};
  std::size_t planned_nodes_{};
  std::size_t analyzed_tokens_{};
};

}  // namespace

std::string canonicalize(const PlanNode& plan) { return canonical(plan); }

std::expected<PlannedQuery, PlanningError> QueryPlanner::plan(
    const QueryNode& query, const PlannerOptions& options) const {
  if (options.default_fields.empty() || options.limits.maximum_nodes == 0U ||
      options.limits.maximum_analyzed_tokens == 0U || options.limits.maximum_depth == 0U) {
    return std::unexpected(
        error(PlanningErrorCode::invalid_options, "planner options contain a zero required limit"));
  }
  for (const auto& field : options.default_fields) {
    if (field.name.empty() || !std::isfinite(field.boost) || field.boost < 0.0) {
      return std::unexpected(error(PlanningErrorCode::invalid_options,
                                   "default fields require names and finite non-negative boosts"));
    }
  }
  auto root = Builder(index_, options).build(query);
  if (!root) return std::unexpected(root.error());
  return PlannedQuery(std::move(*root));
}

}  // namespace dse::query
