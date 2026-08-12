#pragma once

#include <memory>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>

namespace dse::query {

struct QueryNode;
using Query = std::unique_ptr<QueryNode>;

struct TermQuery {
  std::string term;
};

struct PhraseQuery {
  std::string text;
};

struct AndQuery {
  Query left;
  Query right;
};

struct OrQuery {
  Query left;
  Query right;
};

struct NotQuery {
  Query operand;
};

struct FieldQuery {
  std::string field;
  Query query;
};

struct RangeFilter {
  std::string field;
  std::string lower_bound;
  std::string upper_bound;
  bool include_lower{true};
  bool include_upper{true};
};

struct BoostQuery {
  Query query;
  double boost;
};

struct MatchAllQuery {};

using QueryVariant = std::variant<TermQuery, PhraseQuery, AndQuery, OrQuery, NotQuery, FieldQuery,
                                  RangeFilter, BoostQuery, MatchAllQuery>;

struct QueryNode {
  QueryVariant value;
  std::size_t position{};
};

template <typename Node, typename... Arguments>
[[nodiscard]] Query make_query(std::size_t position, Arguments&&... arguments) {
  return std::make_unique<QueryNode>(
      QueryNode{Node{std::forward<Arguments>(arguments)...}, position});
}

}  // namespace dse::query
