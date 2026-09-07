#include "dse/distributed/shard_rpc.hpp"
#include "dse/index/in_memory_index.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
dse::index::InMemoryIndex make_index(){dse::index::InMemoryIndex index;EXPECT_TRUE(index.put({.id=dse::DocumentId("rpc-doc"),.fields={{"body","network transport"},{"tags","rpc"},{"title","distributed search"}},.stored_metadata={{"timestamp","2026-09-06"}}}));return index;}
TEST(ShardRpc, SearchesAcrossProcessBoundary){int sockets[2];ASSERT_EQ(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets),0);auto index=make_index();const auto child=::fork();ASSERT_GE(child,0);if(child==0){::close(sockets[0]);const auto served=dse::distributed::ShardRpcServer::serve_one(sockets[1],index,std::chrono::steady_clock::now()+std::chrono::seconds(2));::close(sockets[1]);::_exit(served?0:2);}::close(sockets[1]);dse::distributed::ShardRpcClient client(sockets[0]);auto result=client.search("title:distributed",10,std::chrono::steady_clock::now()+std::chrono::seconds(2));::close(sockets[0]);ASSERT_TRUE(result)<<(result?"":result.error().message);ASSERT_EQ(result->total_hits,1U);ASSERT_EQ(result->hits.size(),1U);EXPECT_EQ(result->hits[0].document_id,dse::DocumentId("rpc-doc"));int status{};ASSERT_EQ(::waitpid(child,&status,0),child);EXPECT_TRUE(WIFEXITED(status));EXPECT_EQ(WEXITSTATUS(status),0);}
TEST(ShardRpc, EnforcesDeadlineAndRequestLimits){int sockets[2];ASSERT_EQ(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets),0);dse::distributed::ShardRpcClient client(sockets[0],{.maximum_query_bytes=4});auto too_large=client.search("12345",1,std::chrono::steady_clock::now()+std::chrono::seconds(1));ASSERT_FALSE(too_large);EXPECT_EQ(too_large.error().code,dse::distributed::RpcErrorCode::resource_limit);dse::distributed::ShardRpcClient waiting(sockets[0]);auto timeout=waiting.search("*",1,std::chrono::steady_clock::now()+std::chrono::milliseconds(20));ASSERT_FALSE(timeout);EXPECT_EQ(timeout.error().code,dse::distributed::RpcErrorCode::timeout);::close(sockets[0]);::close(sockets[1]);}
}  // namespace
