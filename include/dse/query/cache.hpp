#pragma once
#include "dse/query/executor.hpp"
#include "dse/types.hpp"
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
namespace dse::query {
struct QueryCacheKey { GenerationId generation;std::string schema_fingerprint;std::string plan;std::size_t top_k{};std::string scoring_fingerprint;bool operator==(const QueryCacheKey&)const=default; };
struct QueryCacheStatistics { std::size_t entries{};std::size_t bytes{};std::uint64_t hits{};std::uint64_t misses{};std::uint64_t evictions{}; };
class QueryResultCache {
 public:
  explicit QueryResultCache(std::size_t maximum_bytes):maximum_bytes_(maximum_bytes){}
  [[nodiscard]] std::optional<SearchResult> get(const QueryCacheKey& key);
  void put(QueryCacheKey key,SearchResult result);
  void invalidate_before(GenerationId generation);
  [[nodiscard]] QueryCacheStatistics statistics()const;
 private:
  struct Entry {QueryCacheKey key;SearchResult result;std::size_t bytes{};};
  struct Hash {std::size_t operator()(const QueryCacheKey& key)const noexcept;};
  static std::size_t measure(const QueryCacheKey& key,const SearchResult& result)noexcept;
  using List=std::list<Entry>;std::size_t maximum_bytes_;std::size_t bytes_{};List entries_;std::unordered_map<QueryCacheKey,List::iterator,Hash> lookup_;std::uint64_t hits_{};std::uint64_t misses_{};std::uint64_t evictions_{};mutable std::mutex mutex_;
};
}  // namespace dse::query
