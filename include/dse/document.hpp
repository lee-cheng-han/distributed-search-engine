#pragma once

#include "dse/types.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace dse {

struct Document {
  DocumentId id;
  std::map<std::string, std::string, std::less<>> fields;
  std::map<std::string, std::string, std::less<>> stored_metadata;
  std::uint64_t version{1};
  bool deleted{false};
};

}  // namespace dse
