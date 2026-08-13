#pragma once

#include "dse/index/in_memory_index.hpp"
#include "dse/query/ast.hpp"
#include "dse/ranking/bm25.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace dse::query {

struct SearchField {
  std::string name;
  double boost{1.0};
};

struct SearchOptions {
  std::size_t top_k{10};
  std::vector<SearchField> default_fields{{"title", 1.0}, {"body", 1.0}, {"tags", 1.0}};
};

struct SearchHit {
  DocumentId document_id;
  double score{};
};

struct SearchResult {
  std::vector<SearchHit> hits;
  std::size_t total_hits{};
};

enum class ExecutionErrorCode {
  invalid_options,
  invalid_query_tree,
  unknown_field,
  incompatible_field_type,
  ranking_error,
  nesting_too_deep,
};

struct ExecutionError {
  ExecutionErrorCode code;
  std::string message;
};

class QueryExecutor {
 public:
  explicit QueryExecutor(const index::InMemoryIndex& index,
                         ranking::BM25Parameters parameters = {});

  [[nodiscard]] std::expected<SearchResult, ExecutionError> search(
      const QueryNode& query, const SearchOptions& options = {}) const;

 private:
  const index::InMemoryIndex& index_;
  std::expected<ranking::BM25Scorer, ranking::BM25Error> scorer_;
};

}  // namespace dse::query
