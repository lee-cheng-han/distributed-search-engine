#include "dse/distributed/tcp_transport.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  std::string host{"127.0.0.1"}; std::uint16_t port{9'090}; std::string query{"*"};
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if ((option == "--host" || option == "--port" || option == "--query") && index + 1 >= argc) return 2;
    if (option == "--host") host = argv[++index];
    else if (option == "--query") query = argv[++index];
    else if (option == "--port") {
      const std::string_view value(argv[++index]); unsigned parsed{};
      const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || parsed > 65'535U) return 2;
      port = static_cast<std::uint16_t>(parsed);
    } else return 2;
  }
  dse::distributed::TcpShardClient client({std::move(host), port});
  const auto result = client.search(query, 10,
      std::chrono::steady_clock::now() + std::chrono::seconds(5));
  if (!result) { std::cerr << result.error().message << '\n'; return 3; }
  std::cout << "total_hits=" << result->total_hits << '\n';
  for (const auto& hit : result->hits)
    std::cout << hit.document_id.value() << '\t' << hit.score << '\n';
  return 0;
}
