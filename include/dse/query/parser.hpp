#pragma once

#include "dse/query/ast.hpp"
#include "dse/query/error.hpp"

#include <expected>
#include <string_view>

namespace dse::query {

[[nodiscard]] std::expected<Query, ParseError> parse(std::string_view input);

}  // namespace dse::query
