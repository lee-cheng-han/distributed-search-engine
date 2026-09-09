#include "dse/distributed/tcp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dse::distributed {
namespace {
RpcError fail(RpcErrorCode code, std::string message) { return {code, std::move(message)}; }

class Socket {
 public:
  explicit Socket(int descriptor = -1) : descriptor_(descriptor) {}
  ~Socket() { if (descriptor_ >= 0) ::close(descriptor_); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] int release() noexcept { const int value = descriptor_; descriptor_ = -1; return value; }
 private:
  int descriptor_;
};

std::expected<int, RpcError> connect_socket(const TcpEndpoint& endpoint,
                                            std::chrono::steady_clock::time_point deadline) {
  if (endpoint.port == 0U) return std::unexpected(fail(RpcErrorCode::protocol_error, "TCP port is zero"));
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses{};
  const auto port = std::to_string(endpoint.port);
  const int resolved = ::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &addresses);
  if (resolved != 0) return std::unexpected(fail(RpcErrorCode::io_error, ::gai_strerror(resolved)));
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owned(addresses, ::freeaddrinfo);
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    Socket socket(::socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC,
                           address->ai_protocol));
    if (socket.get() < 0) continue;
    const int flags = ::fcntl(socket.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags | O_NONBLOCK) < 0) continue;
    if (::connect(socket.get(), address->ai_addr, address->ai_addrlen) < 0 && errno != EINPROGRESS) continue;
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return std::unexpected(fail(RpcErrorCode::timeout, "TCP connect deadline exceeded"));
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      pollfd descriptor{socket.get(), POLLOUT, 0};
      const int ready = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining.count() + 1, 2'147'483'647)));
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0) { if (ready == 0) continue; break; }
      int error{}; socklen_t length = sizeof(error);
      if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
        (void)::fcntl(socket.get(), F_SETFL, flags);
        return socket.release();
      }
      break;
    }
  }
  return std::unexpected(fail(RpcErrorCode::io_error, "cannot connect to shard endpoint"));
}
}  // namespace

std::expected<query::SearchResult, RpcError> TcpShardClient::search(
    std::string_view query, std::size_t top_k,
    std::chrono::steady_clock::time_point deadline) const {
  auto connected = connect_socket(endpoint_, deadline);
  if (!connected) return std::unexpected(connected.error());
  Socket socket(*connected);
  return ShardRpcClient(socket.get(), limits_).search(query, top_k, deadline);
}

TcpShardServer::TcpShardServer(int listener, std::uint16_t port,
                               std::shared_ptr<const index::SearchIndexView> index,
                               TcpServerOptions options)
    : listener_(listener), port_(port), index_(std::move(index)), options_(options) {}

std::expected<std::unique_ptr<TcpShardServer>, RpcError> TcpShardServer::start(
    TcpEndpoint endpoint, std::shared_ptr<const index::SearchIndexView> index,
    TcpServerOptions options) {
  if (!index || options.worker_count == 0U || options.worker_count > 256U ||
      options.maximum_queued_connections == 0U || options.request_timeout.count() <= 0)
    return std::unexpected(fail(RpcErrorCode::resource_limit, "invalid TCP server options"));
  Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (listener.get() < 0) return std::unexpected(fail(RpcErrorCode::io_error, std::strerror(errno)));
  int reuse = 1; (void)::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(endpoint.port);
  if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1)
    return std::unexpected(fail(RpcErrorCode::protocol_error, "server bind address must be an IPv4 literal"));
  if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0 ||
      ::listen(listener.get(), static_cast<int>(std::min<std::size_t>(options.maximum_queued_connections, 4096U))) < 0)
    return std::unexpected(fail(RpcErrorCode::io_error, std::strerror(errno)));
  socklen_t length = sizeof(address);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0)
    return std::unexpected(fail(RpcErrorCode::io_error, std::strerror(errno)));
  auto server = std::unique_ptr<TcpShardServer>(new TcpShardServer(
      listener.release(), ntohs(address.sin_port), std::move(index), options));
  server->workers_.reserve(options.worker_count);
  for (std::size_t worker = 0; worker < options.worker_count; ++worker)
    server->workers_.emplace_back([pointer = server.get()] { pointer->worker_loop(); });
  server->acceptor_ = std::thread([pointer = server.get()] { pointer->accept_loop(); });
  return server;
}

TcpShardServer::~TcpShardServer() { stop(); }

void TcpShardServer::stop() noexcept {
  { std::lock_guard lock(mutex_); if (stopping_) return; stopping_ = true; }
  if (listener_ >= 0) { (void)::shutdown(listener_, SHUT_RDWR); ::close(listener_); }
  condition_.notify_all();
  if (acceptor_.joinable()) acceptor_.join();
  listener_ = -1;
  for (auto& worker : workers_) if (worker.joinable()) worker.join();
  std::lock_guard lock(mutex_);
  for (const int connection : connections_) ::close(connection);
  connections_.clear();
}

void TcpShardServer::accept_loop() {
  for (;;) {
    const int connection = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
    if (connection < 0) {
      if (errno == EINTR) continue;
      std::lock_guard lock(mutex_); if (stopping_) return; else continue;
    }
    bool accepted{};
    { std::lock_guard lock(mutex_); if (!stopping_ && connections_.size() < options_.maximum_queued_connections) { connections_.push_back(connection); accepted = true; } }
    if (accepted) { if (options_.metrics) { options_.metrics->increment("dse_tcp_connections_accepted_total"); options_.metrics->add_gauge("dse_tcp_connections_queued", 1); } condition_.notify_one(); }
    else { if (options_.metrics) options_.metrics->increment("dse_tcp_connections_rejected_total"); ::close(connection); }
  }
}

void TcpShardServer::worker_loop() {
  for (;;) {
    int connection{-1};
    { std::unique_lock lock(mutex_); condition_.wait(lock, [&] { return stopping_ || !connections_.empty(); }); if (connections_.empty()) { if (stopping_) return; continue; } connection = connections_.front(); connections_.pop_front(); }
    if (options_.metrics) { options_.metrics->add_gauge("dse_tcp_connections_queued", -1); options_.metrics->add_gauge("dse_tcp_requests_active", 1); }
    Socket socket(connection);
    const auto started = std::chrono::steady_clock::now();
    const auto served = ShardRpcServer::serve_one(socket.get(), *index_,
        std::chrono::steady_clock::now() + options_.request_timeout, options_.rpc_limits);
    if (options_.metrics) { options_.metrics->add_gauge("dse_tcp_requests_active", -1); options_.metrics->increment(served ? "dse_tcp_requests_completed_total" : "dse_tcp_requests_failed_total"); options_.metrics->observe("dse_tcp_request_duration_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()); }
  }
}

}  // namespace dse::distributed
