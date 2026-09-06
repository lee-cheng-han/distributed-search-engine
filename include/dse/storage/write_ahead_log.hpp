#pragma once
#include "dse/document.hpp"
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>
namespace dse::storage {
enum class WalErrorCode { io_error, corruption, resource_limit };
struct WalError { WalErrorCode code; std::string message; };
struct WalLimits { std::uint64_t maximum_file_bytes{1ULL<<30U}; std::uint64_t maximum_record_bytes{64ULL<<20U}; std::uint64_t maximum_records{10'000'000}; std::uint64_t maximum_string_bytes{16ULL<<20U}; };
class WriteAheadLog {
 public:
  explicit WriteAheadLog(std::filesystem::path path):path_(std::move(path)){}
  [[nodiscard]] std::expected<void,WalError> append(const Document& document) const;
  [[nodiscard]] std::expected<std::vector<Document>,WalError> replay(const WalLimits& limits={}) const;
  [[nodiscard]] std::expected<void,WalError> reset() const;
  [[nodiscard]] const std::filesystem::path& path()const noexcept{return path_;}
 private: std::filesystem::path path_;
};
}  // namespace dse::storage
