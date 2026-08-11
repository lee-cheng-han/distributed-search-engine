#pragma once

#include <cstddef>
#include <cstdint>

namespace dse {

struct EngineConfig {
  std::size_t flush_document_count{10'000};
  std::size_t query_queue_capacity{1'024};
  std::size_t indexing_queue_capacity{1'024};
  std::uint32_t default_timeout_ms{100};
};

}  // namespace dse
