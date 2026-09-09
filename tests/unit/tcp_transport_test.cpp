#include "dse/distributed/tcp_transport.hpp"
#include "dse/index/in_memory_index.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <vector>

TEST(TcpTransport, ServesConcurrentLoopbackSearchesWithBoundedWorkers) {
  auto index = std::make_shared<dse::index::InMemoryIndex>();
  ASSERT_TRUE(index->put({.id = dse::DocumentId("tcp-doc"),
                          .fields = {{"body", "network transport"},
                                     {"tags", "rpc"},
                                     {"title", "distributed search"}}}));
  auto server = dse::distributed::TcpShardServer::start(
      {.host = "127.0.0.1", .port = 0}, index,
      {.worker_count = 2, .maximum_queued_connections = 8});
  if (!server && server.error().message == "Operation not permitted")
    GTEST_SKIP() << "loopback TCP is disabled by the test sandbox";
  ASSERT_TRUE(server.has_value()) << (server ? "" : server.error().message);
  dse::distributed::TcpShardClient client({"127.0.0.1", (*server)->port()});
  std::vector<std::future<std::expected<dse::query::SearchResult, dse::distributed::RpcError>>> searches;
  for (std::size_t request = 0; request < 8U; ++request) {
    searches.push_back(std::async(std::launch::async, [&] {
      return client.search("title:distributed", 10,
                           std::chrono::steady_clock::now() + std::chrono::seconds(2));
    }));
  }
  for (auto& search : searches) {
    const auto result = search.get();
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    ASSERT_EQ(result->hits.size(), 1U);
    EXPECT_EQ(result->hits.front().document_id, dse::DocumentId("tcp-doc"));
  }
}

TEST(TcpTransport, RejectsInvalidServerConfigurationAndEndpoint) {
  auto index = std::make_shared<dse::index::InMemoryIndex>();
  EXPECT_FALSE(dse::distributed::TcpShardServer::start(
      {.host = "127.0.0.1", .port = 0}, index, {.worker_count = 0}));
  dse::distributed::TcpShardClient client({"127.0.0.1", 0});
  const auto result = client.search("*", 1,
      std::chrono::steady_clock::now() + std::chrono::seconds(1));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, dse::distributed::RpcErrorCode::protocol_error);
}
