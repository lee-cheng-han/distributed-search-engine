#include "dse/index/in_memory_index.hpp"
#include "dse/query/cache.hpp"
#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"
#include "dse/storage/segment.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
double percentile(std::vector<double> values, double fraction) {
  std::ranges::sort(values); const auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1U)); return values[index];
}
}

int main(int argc, char** argv) {
  std::filesystem::path output;
  if (argc == 3 && std::string_view(argv[1]) == "--output") output = argv[2];
  else if (argc != 1) { std::cerr << "usage: dse_benchmark [--output FILE]\n"; return 2; }
  constexpr std::size_t document_count = 5'000; constexpr std::size_t tokens_per_document = 20;
  constexpr std::size_t query_count = 200; constexpr std::uint64_t seed = 0xD15EA5EULL;
  std::mt19937_64 random(seed); std::uniform_int_distribution<unsigned> word(0, 999);
  dse::index::InMemoryIndex index; const auto indexing_started = Clock::now();
  for (std::size_t number = 0; number < document_count; ++number) {
    std::string body;
    for (std::size_t token = 0; token < tokens_per_document; ++token) {
      if (!body.empty()) body += ' ';
      body += "term" + std::to_string(word(random));
    }
    auto inserted = index.put({.id = dse::DocumentId("doc-" + std::to_string(number)),
                               .fields = {{"body", std::move(body)}, {"title", "benchmark corpus"}},
                               .stored_metadata = {},
                               .version = 1});
    if (!inserted) { std::cerr << inserted.error().message << '\n'; return 3; }
  }
  const double indexing_seconds = std::chrono::duration<double>(Clock::now() - indexing_started).count();
  const auto temporary = std::filesystem::temp_directory_path();
  const auto plain = temporary / ("dse-benchmark-plain-" + std::to_string(seed) + ".dseg");
  const auto compressed = temporary / ("dse-benchmark-compressed-" + std::to_string(seed) + ".dseg");
  auto plain_write = dse::storage::SegmentWriter::write(plain, index.snapshot());
  auto compressed_write = dse::storage::SegmentWriter::write(compressed, index.snapshot(),
      {.compressed_postings = true});
  if (!plain_write || !compressed_write) { std::cerr << "segment benchmark write failed\n"; return 4; }
  const auto plain_bytes = std::filesystem::file_size(plain);
  const auto compressed_bytes = std::filesystem::file_size(compressed);
  auto reader = dse::storage::SegmentReader::open(compressed);
  if (!reader) { std::cerr << reader.error().message << '\n'; return 5; }
  std::vector<double> latencies; latencies.reserve(query_count); std::size_t hit_checksum{};
  for (std::size_t number = 0; number < query_count; ++number) {
    const std::string query_text = "term" + std::to_string(word(random));
    auto parsed = dse::query::parse(query_text); if (!parsed) return 6;
    const auto started = Clock::now();
    dse::query::SearchOptions options; options.top_k = 10;
    auto result = dse::query::QueryExecutor(*reader).search(**parsed, options);
    latencies.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
    if (!result) return 7;
    hit_checksum += result->total_hits;
  }
  auto parsed = dse::query::parse("benchmark"); if (!parsed) return 8;
  dse::query::SearchOptions options; options.top_k = 10;
  auto result = dse::query::QueryExecutor(*reader).search(**parsed, options);
  if (!result) return 9;
  dse::query::QueryResultCache cache(1U << 20U);
  dse::query::QueryCacheKey key{dse::GenerationId(1), "default", "benchmark", 10, "bm25-default"};
  cache.put(key, *result); const auto cache_started = Clock::now();
  for (std::size_t count = 0; count < 100'000U; ++count) if (!cache.get(key)) return 10;
  const double cache_ns = std::chrono::duration<double, std::nano>(Clock::now() - cache_started).count() / 100'000.0;
  std::ostringstream json; json << std::setprecision(10)
      << "{\n  \"schema_version\": 1,\n  \"seed\": " << seed
      << ",\n  \"documents\": " << document_count << ",\n  \"tokens_per_document\": " << tokens_per_document
      << ",\n  \"queries\": " << query_count << ",\n  \"compiler\": \"" << __VERSION__ << "\""
      << ",\n  \"hardware_threads\": " << std::thread::hardware_concurrency()
      << ",\n  \"indexing_seconds\": " << indexing_seconds
      << ",\n  \"indexing_documents_per_second\": " << static_cast<double>(document_count) / indexing_seconds
      << ",\n  \"query_latency_us\": {\"p50\": " << percentile(latencies, 0.50)
      << ", \"p95\": " << percentile(latencies, 0.95) << ", \"p99\": " << percentile(latencies, 0.99) << "}"
      << ",\n  \"cache_hit_average_ns\": " << cache_ns
      << ",\n  \"uncompressed_segment_bytes\": " << plain_bytes
      << ",\n  \"compressed_segment_bytes\": " << compressed_bytes
      << ",\n  \"compression_ratio\": " << static_cast<double>(compressed_bytes) / static_cast<double>(plain_bytes)
      << ",\n  \"result_hit_checksum\": " << hit_checksum << "\n}\n";
  std::error_code ignored; std::filesystem::remove(plain, ignored); std::filesystem::remove(compressed, ignored);
  if (output.empty()) std::cout << json.str();
  else { std::filesystem::create_directories(output.parent_path(), ignored); std::ofstream file(output); file << json.str(); if (!file) return 11; }
  return 0;
}
