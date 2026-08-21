#pragma once

#include "dse/analysis/analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dse::query {

struct PlanNode;
using Plan = std::unique_ptr<PlanNode>;

struct PlannedTerm {
  std::string field;
  std::string term;
  double boost{1.0};
};

struct PlannedPhrase {
  std::string field;
  std::vector<analysis::Token> tokens;
  double boost{1.0};
};

struct PlannedAnd { std::vector<Plan> children; };
struct PlannedOr { std::vector<Plan> children; };
struct PlannedNot { Plan operand; };

struct PlannedRange {
  std::string field;
  std::string lower_bound;
  std::string upper_bound;
  bool include_lower{true};
  bool include_upper{true};
};

struct PlannedMatchAll {};

using PlanVariant = std::variant<PlannedTerm, PlannedPhrase, PlannedAnd, PlannedOr, PlannedNot,
                                 PlannedRange, PlannedMatchAll>;

struct PlanNode {
  PlanVariant value;
  std::size_t estimated_matches{};
};

class PlannedQuery {
 public:
  explicit PlannedQuery(Plan root) : root_(std::move(root)) {}
  [[nodiscard]] const PlanNode& root() const noexcept { return *root_; }

 private:
  Plan root_;
};

[[nodiscard]] std::string canonicalize(const PlanNode& plan);

}  // namespace dse::query
