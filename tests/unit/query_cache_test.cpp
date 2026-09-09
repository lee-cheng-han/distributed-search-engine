#include "dse/query/cache.hpp"
#include <gtest/gtest.h>
namespace {dse::query::QueryCacheKey key(std::uint64_t generation,std::string plan){return {dse::GenerationId(generation),"schema",std::move(plan),10,"bm25"};}dse::query::SearchResult result(std::string id){return {{{dse::DocumentId(std::move(id)),1.0}},1};}
TEST(QueryResultCache, IsGenerationAwareAndTracksHitsMisses){dse::query::QueryResultCache cache(4096);cache.put(key(1,"alpha"),result("a"));EXPECT_TRUE(cache.get(key(1,"alpha")));EXPECT_FALSE(cache.get(key(2,"alpha")));auto stats=cache.statistics();EXPECT_EQ(stats.hits,1U);EXPECT_EQ(stats.misses,1U);cache.invalidate_before(dse::GenerationId(2));EXPECT_EQ(cache.statistics().entries,0U);}
TEST(QueryResultCache, EnforcesByteBoundWithLruEviction){constexpr std::size_t capacity=250U;dse::query::QueryResultCache cache(capacity);cache.put(key(1,"a"),result("a"));cache.put(key(1,"b"),result("b"));EXPECT_LE(cache.statistics().bytes,capacity);EXPECT_GE(cache.statistics().evictions,1U);EXPECT_TRUE(cache.get(key(1,"b")));EXPECT_FALSE(cache.get(key(1,"a")));}
}  // namespace
