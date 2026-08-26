#include "dse/storage/segment.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > (1U << 20U)) return 0;
  const auto path = std::filesystem::temp_directory_path() / "dse-segment-reader-fuzz.dseg";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return 0;
    output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  }
  (void)dse::storage::SegmentReader::open(
      path, {.maximum_file_bytes = 1U << 20U,
             .maximum_documents = 10'000,
             .maximum_terms = 100'000,
             .maximum_postings = 1'000'000,
             .maximum_positions = 4'000'000,
             .maximum_string_bytes = 64U << 10U,
             .maximum_fields = 1'024});
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  return 0;
}
