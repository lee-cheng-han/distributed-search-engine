#include "dse/index/in_memory_index.hpp"
#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"
#include "dse/query/planner.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  auto parsed = dse::query::parse(input);
  if (!parsed) return 0;

  static const dse::index::InMemoryIndex index;
  auto planned = dse::query::QueryPlanner(index).plan(**parsed);
  if (!planned) return 0;

  const auto canonical = dse::query::canonicalize(planned->root());
  auto reparsed = dse::query::parse(input);
  if (!reparsed) __builtin_trap();
  auto replanned = dse::query::QueryPlanner(index).plan(**reparsed);
  if (!replanned || canonical != dse::query::canonicalize(replanned->root())) __builtin_trap();

  const auto result = dse::query::QueryExecutor(index).search(*planned, size % 32U);
  if (!result) __builtin_trap();
  return 0;
}
