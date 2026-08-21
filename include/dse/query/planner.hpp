#pragma once

#include "dse/index/in_memory_index.hpp"
#include "dse/query/ast.hpp"
#include "dse/query/plan.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

namespace dse::query {

struct SearchField {
  std::string name;
  double boost{1.0};
};

struct PlannerLimits {
  std::size_t maximum_nodes{1'024};
  std::size_t maximum_analyzed_tokens{4'096};
  std::size_t maximum_phrase_tokens{256};
  std::size_t maximum_depth{128};
};

struct PlannerOptions {
  std::vector<SearchField> default_fields{{"title", 1.0}, {"body", 1.0}, {"tags", 1.0}};
  PlannerLimits limits;
};

enum class PlanningErrorCode {
  invalid_options,
  invalid_query_tree,
  unknown_field,
  incompatible_field_type,
  invalid_range,
  resource_limit,
  analysis_failed,
};

struct PlanningError {
  PlanningErrorCode code;
  std::string message;
};

class QueryPlanner {
 public:
  explicit QueryPlanner(const index::InMemoryIndex& index) : index_(index) {}

  [[nodiscard]] std::expected<PlannedQuery, PlanningError> plan(
      const QueryNode& query, const PlannerOptions& options = {}) const;

 private:
  const index::InMemoryIndex& index_;
};

}  // namespace dse::query
