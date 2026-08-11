#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace dse {

template <typename Tag, typename Value>
class StrongId {
 public:
  constexpr StrongId() = default;
  explicit constexpr StrongId(Value value) : value_(std::move(value)) {}
  [[nodiscard]] constexpr const Value& value() const noexcept { return value_; }
  auto operator<=>(const StrongId&) const = default;

 private:
  Value value_{};
};

using DocumentId = StrongId<struct DocumentIdTag, std::string>;
using NodeId = StrongId<struct NodeIdTag, std::string>;
using ShardId = StrongId<struct ShardIdTag, std::uint32_t>;
using SegmentId = StrongId<struct SegmentIdTag, std::uint64_t>;
using TermId = StrongId<struct TermIdTag, std::uint64_t>;
using GenerationId = StrongId<struct GenerationIdTag, std::uint64_t>;

}  // namespace dse
