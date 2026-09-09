#include "dse/distributed/tcp_transport.hpp"
#include "dse/storage/segment.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {
std::atomic_bool stopping{};
void stop(int) { stopping.store(true); }
}

int main(int argc, char** argv) {
  std::string segment;
  std::string bind{"0.0.0.0"};
  std::uint16_t port{9'090};
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if ((option == "--segment" || option == "--bind" || option == "--port") && index + 1 >= argc) {
      std::cerr << "missing value for " << option << '\n'; return 2;
    }
    if (option == "--segment") segment = argv[++index];
    else if (option == "--bind") bind = argv[++index];
    else if (option == "--port") {
      const std::string_view value(argv[++index]); unsigned parsed{};
      const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > 65'535U) {
        std::cerr << "invalid port\n"; return 2;
      }
      port = static_cast<std::uint16_t>(parsed);
    } else { std::cerr << "unknown option: " << option << '\n'; return 2; }
  }
  if (segment.empty()) { std::cerr << "usage: dse_shard_server --segment PATH [--bind ADDRESS] [--port PORT]\n"; return 2; }
  auto opened = dse::storage::SegmentReader::open(segment);
  if (!opened) { std::cerr << opened.error().message << '\n'; return 3; }
  auto view = std::make_shared<const dse::storage::SegmentReader>(std::move(*opened));
  auto server = dse::distributed::TcpShardServer::start({std::move(bind), port}, view);
  if (!server) { std::cerr << server.error().message << '\n'; return 4; }
  std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
  std::cout << "ready port=" << (*server)->port() << " documents=" << view->live_document_count() << '\n' << std::flush;
  while (!stopping.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return 0;
}
