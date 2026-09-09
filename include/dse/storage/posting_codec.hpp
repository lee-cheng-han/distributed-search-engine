#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>
namespace dse::storage {
enum class CodecErrorCode { truncated, overflow, non_canonical, invalid_order, resource_limit };
struct CodecError { CodecErrorCode code; std::string message; };
class VariableByteCodec {
 public:
  static void encode(std::uint32_t value,std::vector<std::byte>& output);
  [[nodiscard]] static std::expected<std::uint32_t,CodecError> decode(std::span<const std::byte> input,std::size_t& offset);
  [[nodiscard]] static std::expected<std::vector<std::byte>,CodecError> encode_deltas(std::span<const std::uint32_t> sorted);
  [[nodiscard]] static std::expected<std::vector<std::uint32_t>,CodecError> decode_deltas(std::span<const std::byte> input,std::size_t expected_count,std::size_t maximum_count);
};
}  // namespace dse::storage
