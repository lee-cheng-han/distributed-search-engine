#include "dse/storage/segment.hpp"

#include "dse/analysis/analyzer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dse::storage {
namespace {

constexpr std::array<std::byte, 8> kMagic{std::byte{'D'}, std::byte{'S'}, std::byte{'E'},
                                          std::byte{'S'}, std::byte{'E'}, std::byte{'G'},
                                          std::byte{'0'}, std::byte{'1'}};
constexpr std::uint16_t kMajor = 1;
constexpr std::uint16_t kMinor = 0;
constexpr std::uint32_t kHeaderBytes = 64;
constexpr std::uint32_t kDirectoryEntryBytes = 32;
constexpr std::uint32_t kSectionCount = 6;

enum class Section : std::uint32_t { schema = 1, documents, statistics, terms, postings, positions };

SegmentError fail(SegmentErrorCode code, std::string message) {
  return {code, std::move(message)};
}

std::expected<void, SegmentError> sync_path(const std::filesystem::path& path,
                                            bool directory) {
#if defined(__unix__) || defined(__APPLE__)
  const int flags = O_RDONLY | (directory ? O_DIRECTORY : 0);
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0)
    return std::unexpected(fail(SegmentErrorCode::io_error, "cannot open path for fsync"));
  const int result = ::fsync(descriptor);
  const int saved = errno;
  ::close(descriptor);
  if (result != 0)
    return std::unexpected(fail(SegmentErrorCode::io_error,
                                "fsync failed: " +
                                    std::error_code(saved, std::generic_category()).message()));
#else
  (void)path;
  (void)directory;
#endif
  return {};
}

class Bytes {
 public:
  void u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }
  void u16(std::uint16_t value) { integer(value, 2); }
  void u32(std::uint32_t value) { integer(value, 4); }
  void u64(std::uint64_t value) { integer(value, 8); }
  void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
  void string(std::string_view value) {
    u64(value.size());
    for (const auto byte : value) data_.push_back(static_cast<std::byte>(byte));
  }
  void append(std::span<const std::byte> value) { data_.insert(data_.end(), value.begin(), value.end()); }
  void resize(std::size_t size) { data_.resize(size); }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::vector<std::byte>& data() noexcept { return data_; }
  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }

 private:
  void integer(std::uint64_t value, unsigned bytes) {
    for (unsigned index = 0; index < bytes; ++index) {
      data_.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }
  }
  std::vector<std::byte> data_;
};

std::uint32_t crc32c(std::span<const std::byte> bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const auto item : bytes) {
    crc ^= static_cast<std::uint8_t>(item);
    for (unsigned bit = 0; bit < 8U; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return ~crc;
}

std::uint64_t schema_fingerprint(const index::IndexSchema& schema) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto add = [&](std::string_view value) {
    for (const char character : value) {
      hash ^= static_cast<unsigned char>(character);
      hash *= 1099511628211ULL;
    }
    hash ^= 0xffU; hash *= 1099511628211ULL;
  };
  for (const auto& [name, field] : schema.fields()) {
    add(name);
    add(std::to_string(static_cast<unsigned>(field.type)));
    add(field.indexed ? "1" : "0");
    add(field.stored ? "1" : "0");
    add(std::to_string(std::bit_cast<std::uint64_t>(field.boost)));
    add(field.analyzer ? field.analyzer->descriptor() : "none");
  }
  return hash;
}

Bytes encode_schema(const index::IndexSchema& schema) {
  Bytes output;
  output.u64(schema_fingerprint(schema));
  output.u64(schema.fields().size());
  for (const auto& [name, field] : schema.fields()) {
    output.string(name);
    output.u8(static_cast<std::uint8_t>(field.type));
    output.u8(field.indexed ? 1U : 0U);
    output.u8(field.stored ? 1U : 0U);
    output.u8(0);
    output.f64(field.boost);
    output.string(field.analyzer ? field.analyzer->descriptor() : std::string_view{});
  }
  return output;
}

void encode_string_map(Bytes& output,
                       const std::map<std::string, std::string, std::less<>>& values) {
  output.u64(values.size());
  for (const auto& [key, value] : values) { output.string(key); output.string(value); }
}

Bytes encode_documents(const index::IndexSnapshot& snapshot) {
  Bytes output;
  output.u64(snapshot.documents().size());
  for (const auto& [external, record] : snapshot.documents()) {
    output.u32(record.internal_id.value());
    output.string(external.value());
    output.u64(record.document.version);
    output.u8(record.document.deleted ? 1U : 0U);
    encode_string_map(output, record.document.fields);
    encode_string_map(output, record.document.stored_metadata);
    output.u64(record.field_lengths.size());
    for (const auto& [field, length] : record.field_lengths) { output.string(field); output.u32(length); }
    output.u64(record.indexed_terms.size());
    for (const auto& [field, terms] : record.indexed_terms) {
      output.string(field); output.u64(terms.size());
      for (const auto& term : terms) output.string(term);
    }
  }
  return output;
}

Bytes encode_statistics(const index::IndexSnapshot& snapshot) {
  Bytes output;
  output.u64(snapshot.statistics().size());
  for (const auto& [field, stats] : snapshot.statistics()) {
    output.string(field); output.u64(stats.document_count); output.u64(stats.total_length);
  }
  return output;
}

struct EncodedIndex { Bytes terms; Bytes postings; Bytes positions; std::uint64_t term_count{}; std::uint64_t posting_count{}; std::uint64_t position_count{}; };

EncodedIndex encode_index(const index::IndexSnapshot& snapshot) {
  EncodedIndex result;
  for (const auto& [field, dictionary] : snapshot.fields()) {
    for (const auto& [term, entry] : dictionary) {
      result.terms.string(field); result.terms.string(term);
      result.terms.u32(entry.document_frequency); result.terms.u32(0);
      result.terms.u64(result.posting_count); result.terms.u64(entry.postings.size());
      ++result.term_count;
      for (const auto& posting : entry.postings) {
        result.postings.u32(posting.document_id.value()); result.postings.u32(posting.term_frequency);
        result.postings.u64(result.position_count); result.postings.u64(posting.positions.size());
        ++result.posting_count;
        for (const auto position : posting.positions) { result.positions.u32(position); ++result.position_count; }
      }
    }
  }
  return result;
}

struct SectionData { Section type; Bytes bytes; std::uint64_t count; };

void overwrite_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index)
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}
void overwrite_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index)
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

class Cursor {
 public:
  Cursor(std::span<const std::byte> bytes, const SegmentReadLimits& limits)
      : bytes_(bytes), limits_(limits) {}
  std::expected<std::uint8_t, SegmentError> u8() { auto value = integer(1); if (!value) return std::unexpected(value.error()); return static_cast<std::uint8_t>(*value); }
  std::expected<std::uint16_t, SegmentError> u16() { auto value = integer(2); if (!value) return std::unexpected(value.error()); return static_cast<std::uint16_t>(*value); }
  std::expected<std::uint32_t, SegmentError> u32() { auto value = integer(4); if (!value) return std::unexpected(value.error()); return static_cast<std::uint32_t>(*value); }
  std::expected<std::uint64_t, SegmentError> u64() { return integer(8); }
  std::expected<double, SegmentError> f64() { auto value = u64(); if (!value) return std::unexpected(value.error()); return std::bit_cast<double>(*value); }
  std::expected<std::string, SegmentError> string() {
    auto size = u64(); if (!size) return std::unexpected(size.error());
    if (*size > limits_.maximum_string_bytes) return std::unexpected(fail(SegmentErrorCode::resource_limit, "segment string limit exceeded"));
    if (*size > remaining()) return std::unexpected(fail(SegmentErrorCode::corruption, "truncated segment string"));
    std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), static_cast<std::size_t>(*size));
    offset_ += static_cast<std::size_t>(*size); return result;
  }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }
 private:
  std::expected<std::uint64_t, SegmentError> integer(unsigned width) {
    if (remaining() < width) return std::unexpected(fail(SegmentErrorCode::corruption, "truncated segment integer"));
    std::uint64_t value = 0; for (unsigned index = 0; index < width; ++index) value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U);
    offset_ += width; return value;
  }
  std::span<const std::byte> bytes_; const SegmentReadLimits& limits_; std::size_t offset_{};
};

std::expected<std::shared_ptr<const analysis::Analyzer>, SegmentError> analyzer_from(
    std::string_view descriptor) {
  if (descriptor.empty()) return std::shared_ptr<const analysis::Analyzer>{};
  if (descriptor == "keyword-v1") return std::make_shared<const analysis::KeywordAnalyzer>();
  constexpr std::string_view prefix = "standard-v1:";
  if (!descriptor.starts_with(prefix)) return std::unexpected(fail(SegmentErrorCode::unsupported_version, "unsupported analyzer descriptor"));
  std::size_t cursor = prefix.size();
  const auto number = [&](std::size_t& at) -> std::optional<std::size_t> {
    const auto colon = descriptor.find(':', at); if (colon == std::string_view::npos) return {};
    std::size_t value = 0; const auto parsed = std::from_chars(descriptor.data() + at, descriptor.data() + colon, value);
    if (parsed.ec != std::errc{} || parsed.ptr != descriptor.data() + colon) return {};
    at = colon + 1; return value;
  };
  const auto count = number(cursor); if (!count) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid analyzer descriptor"));
  std::set<std::string, std::less<>> stop_words;
  for (std::size_t index = 0; index < *count; ++index) {
    const auto length = number(cursor); if (!length || *length > descriptor.size() - cursor) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid analyzer stop-word descriptor"));
    stop_words.emplace(descriptor.substr(cursor, *length)); cursor += *length;
  }
  if (cursor != descriptor.size()) return std::unexpected(fail(SegmentErrorCode::corruption, "trailing analyzer descriptor data"));
  return std::make_shared<const analysis::StandardAnalyzer>(std::move(stop_words));
}

std::expected<std::map<std::string, std::string, std::less<>>, SegmentError> read_string_map(
    Cursor& cursor, std::uint64_t maximum) {
  auto count = cursor.u64(); if (!count) return std::unexpected(count.error());
  if (*count > maximum) return std::unexpected(fail(SegmentErrorCode::resource_limit, "segment map count limit exceeded"));
  std::map<std::string, std::string, std::less<>> result;
  for (std::uint64_t index = 0; index < *count; ++index) {
    auto key = cursor.string(); if (!key) return std::unexpected(key.error()); auto value = cursor.string(); if (!value) return std::unexpected(value.error());
    if (!result.emplace(std::move(*key), std::move(*value)).second) return std::unexpected(fail(SegmentErrorCode::corruption, "duplicate segment map key"));
  }
  return result;
}

}  // namespace

std::string_view describe(SegmentErrorCode code) noexcept {
  switch (code) {
    case SegmentErrorCode::io_error: return "segment I/O error";
    case SegmentErrorCode::corruption: return "segment corruption";
    case SegmentErrorCode::unsupported_version: return "unsupported segment version";
    case SegmentErrorCode::resource_limit: return "segment resource limit exceeded";
    case SegmentErrorCode::invalid_snapshot: return "invalid index snapshot";
  }
  return "unknown segment error";
}

std::expected<void, SegmentError> SegmentWriter::write(const std::filesystem::path& path,
                                                       const index::IndexSnapshot& snapshot,
                                                       const SegmentWriteOptions& options) {
  if (path.empty() || options.segment_id.value() == 0U) return std::unexpected(fail(SegmentErrorCode::invalid_snapshot, "segment path and ID must be non-empty"));
  auto encoded = encode_index(snapshot);
  std::vector<SectionData> sections;
  sections.push_back({Section::schema, encode_schema(snapshot.schema()), snapshot.schema().fields().size()});
  sections.push_back({Section::documents, encode_documents(snapshot), snapshot.documents().size()});
  sections.push_back({Section::statistics, encode_statistics(snapshot), snapshot.statistics().size()});
  sections.push_back({Section::terms, std::move(encoded.terms), encoded.term_count});
  sections.push_back({Section::postings, std::move(encoded.postings), encoded.posting_count});
  sections.push_back({Section::positions, std::move(encoded.positions), encoded.position_count});

  Bytes file; file.resize(kHeaderBytes + kSectionCount * kDirectoryEntryBytes);
  std::size_t directory = kHeaderBytes;
  for (const auto& section : sections) {
    overwrite_u32(file.data(), directory, static_cast<std::uint32_t>(section.type));
    overwrite_u64(file.data(), directory + 8U, file.size());
    overwrite_u64(file.data(), directory + 16U, section.bytes.size());
    overwrite_u64(file.data(), directory + 24U, section.count);
    file.append(section.bytes.data()); directory += kDirectoryEntryBytes;
  }
  std::copy(kMagic.begin(), kMagic.end(), file.data().begin());
  file.data()[8] = static_cast<std::byte>(kMajor & 0xffU); file.data()[9] = static_cast<std::byte>(kMajor >> 8U);
  file.data()[10] = static_cast<std::byte>(kMinor & 0xffU); file.data()[11] = static_cast<std::byte>(kMinor >> 8U);
  overwrite_u32(file.data(), 12, kHeaderBytes); overwrite_u64(file.data(), 16, file.size());
  overwrite_u64(file.data(), 24, options.segment_id.value()); overwrite_u32(file.data(), 32, kSectionCount);
  overwrite_u32(file.data(), 36, 0); overwrite_u32(file.data(), 40, crc32c(std::span(file.data()).subspan(kHeaderBytes)));

  auto temporary = path; temporary += ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return std::unexpected(fail(SegmentErrorCode::io_error, "cannot create temporary segment"));
  output.write(reinterpret_cast<const char*>(file.data().data()), static_cast<std::streamsize>(file.size())); output.flush();
  if (!output) { output.close(); std::error_code ignored; std::filesystem::remove(temporary, ignored); return std::unexpected(fail(SegmentErrorCode::io_error, "cannot write temporary segment")); }
  output.close();
  auto synced = sync_path(temporary, false);
  if (!synced) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return synced;
  }
  auto verification = SegmentReader::open(temporary);
  if (!verification) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return std::unexpected(fail(SegmentErrorCode::invalid_snapshot,
                                "serialized snapshot failed validation: " +
                                    verification.error().message));
  }
  std::error_code rename_error; std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) { std::error_code ignored; std::filesystem::remove(temporary, ignored); return std::unexpected(fail(SegmentErrorCode::io_error, "cannot publish segment: " + rename_error.message())); }
  return sync_path(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(),
                   true);
}

std::expected<SegmentReader, SegmentError> SegmentReader::open(const std::filesystem::path& path,
                                                               const SegmentReadLimits& limits) {
  std::error_code size_error; const auto file_size = std::filesystem::file_size(path, size_error);
  if (size_error) return std::unexpected(fail(SegmentErrorCode::io_error, "cannot stat segment: " + size_error.message()));
  if (file_size > limits.maximum_file_bytes) return std::unexpected(fail(SegmentErrorCode::resource_limit, "segment file size limit exceeded"));
  if (file_size < kHeaderBytes + kSectionCount * kDirectoryEntryBytes) return std::unexpected(fail(SegmentErrorCode::corruption, "segment is shorter than its header"));
  std::vector<std::byte> bytes(static_cast<std::size_t>(file_size)); std::ifstream input(path, std::ios::binary);
  if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::unexpected(fail(SegmentErrorCode::io_error, "cannot read segment"));
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid segment magic"));
  Cursor header(std::span<const std::byte>(bytes).subspan(8, kHeaderBytes - 8), limits);
  auto major = header.u16(); auto minor = header.u16(); auto header_size = header.u32(); auto recorded_size = header.u64(); auto segment_id = header.u64(); auto section_count = header.u32(); auto flags = header.u32(); auto checksum = header.u32();
  if (!major || !minor || !header_size || !recorded_size || !segment_id || !section_count || !flags || !checksum) return std::unexpected(fail(SegmentErrorCode::corruption, "truncated segment header"));
  if (*major != kMajor) return std::unexpected(fail(SegmentErrorCode::unsupported_version, "unsupported segment major version"));
  if (*minor > kMinor || *flags != 0U) return std::unexpected(fail(SegmentErrorCode::unsupported_version, "unsupported segment features"));
  if (*header_size != kHeaderBytes || *recorded_size != file_size || *segment_id == 0U || *section_count != kSectionCount) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid segment header values"));
  if (std::ranges::any_of(std::span<const std::byte>(bytes).subspan(44, 20),
                          [](std::byte value) { return value != std::byte{}; }))
    return std::unexpected(fail(SegmentErrorCode::corruption, "nonzero reserved header bytes"));
  if (crc32c(std::span(bytes).subspan(kHeaderBytes)) != *checksum) return std::unexpected(fail(SegmentErrorCode::corruption, "segment checksum mismatch"));

  struct Slice { std::span<const std::byte> bytes; std::uint64_t count; };
  std::map<Section, Slice> slices; Cursor directory(std::span<const std::byte>(bytes).subspan(kHeaderBytes, kSectionCount * kDirectoryEntryBytes), limits);
  std::uint64_t expected_offset = kHeaderBytes + kSectionCount * kDirectoryEntryBytes;
  for (std::uint32_t index = 0; index < kSectionCount; ++index) {
    auto type = directory.u32(); auto reserved = directory.u32(); auto offset = directory.u64(); auto length = directory.u64(); auto count = directory.u64();
    if (!type || !reserved || !offset || !length || !count || *reserved != 0U || *type != index + 1U || *offset != expected_offset || *length > file_size - *offset) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid segment section directory"));
    slices.emplace(static_cast<Section>(*type), Slice{std::span(bytes).subspan(static_cast<std::size_t>(*offset), static_cast<std::size_t>(*length)), *count}); expected_offset += *length;
  }
  if (expected_offset != file_size) return std::unexpected(fail(SegmentErrorCode::corruption, "trailing or missing segment section bytes"));

  Cursor schema_cursor(slices.at(Section::schema).bytes, limits); auto fingerprint = schema_cursor.u64(); auto field_count = schema_cursor.u64();
  if (!fingerprint || !field_count || *field_count != slices.at(Section::schema).count)
    return std::unexpected(fail(SegmentErrorCode::corruption, "invalid segment schema count"));
  if (*field_count > limits.maximum_fields)
    return std::unexpected(fail(SegmentErrorCode::resource_limit, "segment schema field limit exceeded"));
  std::vector<index::FieldDefinition> definitions;
  for (std::uint64_t index = 0; index < *field_count; ++index) {
    auto name = schema_cursor.string(); auto type = schema_cursor.u8(); auto indexed = schema_cursor.u8(); auto stored = schema_cursor.u8(); auto reserved = schema_cursor.u8(); auto boost = schema_cursor.f64(); auto descriptor = schema_cursor.string();
    if (!name || !type || !indexed || !stored || !reserved || !boost || !descriptor || *reserved != 0U || *type > static_cast<std::uint8_t>(index::FieldType::timestamp) || *indexed > 1U || *stored > 1U) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid persisted field definition"));
    auto analyzer = analyzer_from(*descriptor); if (!analyzer) return std::unexpected(analyzer.error());
    definitions.push_back({std::move(*name), static_cast<index::FieldType>(*type), *indexed != 0U, *stored != 0U, *boost, std::move(*analyzer)});
  }
  if (!schema_cursor.done()) return std::unexpected(fail(SegmentErrorCode::corruption, "trailing schema data"));
  auto schema = index::IndexSchema::create(std::move(definitions)); if (!schema) return std::unexpected(fail(SegmentErrorCode::corruption, schema.error().message));
  if (schema_fingerprint(*schema) != *fingerprint) return std::unexpected(fail(SegmentErrorCode::corruption, "schema fingerprint mismatch"));

  index::DocumentRecords documents; index::ExternalIdMap external_ids; Cursor docs(slices.at(Section::documents).bytes, limits); auto document_count = docs.u64();
  if (!document_count || *document_count != slices.at(Section::documents).count || *document_count > limits.maximum_documents) return std::unexpected(fail(SegmentErrorCode::resource_limit, "invalid segment document count"));
  for (std::uint64_t number = 0; number < *document_count; ++number) {
    auto internal = docs.u32(); auto external = docs.string(); auto version = docs.u64(); auto deleted = docs.u8();
    if (!internal || !external || !version || !deleted || *internal == 0U || *external == "" || *deleted > 1U) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid segment document header"));
    auto fields = read_string_map(docs, limits.maximum_fields); if (!fields) return std::unexpected(fields.error()); auto metadata = read_string_map(docs, limits.maximum_fields); if (!metadata) return std::unexpected(metadata.error());
    std::map<std::string, std::uint32_t, std::less<>> lengths; auto length_count = docs.u64(); if (!length_count || *length_count > limits.maximum_fields) return std::unexpected(fail(SegmentErrorCode::resource_limit, "field length count exceeded"));
    for (std::uint64_t i = 0; i < *length_count; ++i) { auto field = docs.string(); auto length = docs.u32(); if (!field || !length || !lengths.emplace(std::move(*field), *length).second) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid field length record")); }
    std::map<std::string, std::vector<std::string>, std::less<>> indexed_terms; auto indexed_count = docs.u64(); if (!indexed_count || *indexed_count > limits.maximum_fields) return std::unexpected(fail(SegmentErrorCode::resource_limit, "indexed field count exceeded"));
    for (std::uint64_t i = 0; i < *indexed_count; ++i) { auto field = docs.string(); auto count = docs.u64(); if (!field || !count || *count > limits.maximum_terms) return std::unexpected(fail(SegmentErrorCode::resource_limit, "indexed term count exceeded")); std::vector<std::string> terms; terms.reserve(static_cast<std::size_t>(*count)); for (std::uint64_t j = 0; j < *count; ++j) { auto term = docs.string(); if (!term) return std::unexpected(term.error()); terms.push_back(std::move(*term)); } if (!indexed_terms.emplace(std::move(*field), std::move(terms)).second) return std::unexpected(fail(SegmentErrorCode::corruption, "duplicate indexed field")); }
    DocumentId id(std::move(*external)); InternalDocumentId internal_id(*internal); Document document{.id = id, .fields = std::move(*fields), .stored_metadata = std::move(*metadata), .version = *version, .deleted = *deleted != 0U};
    if (!external_ids.emplace(internal_id, id).second || !documents.emplace(id, index::DocumentRecord{internal_id, std::move(document), std::move(lengths), std::move(indexed_terms)}).second) return std::unexpected(fail(SegmentErrorCode::corruption, "duplicate segment document ID"));
  }
  if (!docs.done()) return std::unexpected(fail(SegmentErrorCode::corruption, "trailing document data"));

  index::FieldStatisticsMap statistics; Cursor stats(slices.at(Section::statistics).bytes, limits); auto stats_count = stats.u64(); if (!stats_count || *stats_count != slices.at(Section::statistics).count || *stats_count > limits.maximum_fields) return std::unexpected(fail(SegmentErrorCode::resource_limit, "invalid statistics count"));
  for (std::uint64_t i = 0; i < *stats_count; ++i) { auto field = stats.string(); auto count = stats.u64(); auto total = stats.u64(); if (!field || !count || !total || *count == 0U || !statistics.emplace(std::move(*field), index::FieldStatistics{static_cast<std::size_t>(*count), *total, static_cast<double>(*total) / static_cast<double>(*count)}).second) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid field statistics")); }
  if (!stats.done()) return std::unexpected(fail(SegmentErrorCode::corruption, "trailing statistics data"));

  const auto& position_slice = slices.at(Section::positions); if (position_slice.count > limits.maximum_positions || position_slice.bytes.size() != position_slice.count * 4U) return std::unexpected(fail(SegmentErrorCode::resource_limit, "invalid position section size"));
  std::vector<std::uint32_t> positions; positions.reserve(static_cast<std::size_t>(position_slice.count)); Cursor pos(position_slice.bytes, limits); for (std::uint64_t i = 0; i < position_slice.count; ++i) { auto value = pos.u32(); if (!value) return std::unexpected(value.error()); positions.push_back(*value); }
  struct RawPosting { std::uint32_t document; std::uint32_t frequency; std::uint64_t start; std::uint64_t count; };
  const auto& posting_slice = slices.at(Section::postings); if (posting_slice.count > limits.maximum_postings || posting_slice.bytes.size() != posting_slice.count * 24U) return std::unexpected(fail(SegmentErrorCode::resource_limit, "invalid posting section size"));
  std::vector<RawPosting> postings; postings.reserve(static_cast<std::size_t>(posting_slice.count)); Cursor posting_cursor(posting_slice.bytes, limits); for (std::uint64_t i = 0; i < posting_slice.count; ++i) { auto document = posting_cursor.u32(); auto frequency = posting_cursor.u32(); auto start = posting_cursor.u64(); auto count = posting_cursor.u64(); if (!document || !frequency || !start || !count || *frequency != *count || *start > positions.size() || *count > positions.size() - *start) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid posting record")); postings.push_back({*document, *frequency, *start, *count}); }
  index::FieldDictionaries fields; const auto& term_slice = slices.at(Section::terms); if (term_slice.count > limits.maximum_terms) return std::unexpected(fail(SegmentErrorCode::resource_limit, "term count limit exceeded")); Cursor terms(term_slice.bytes, limits);
  for (std::uint64_t i = 0; i < term_slice.count; ++i) { auto field = terms.string(); auto term = terms.string(); auto frequency = terms.u32(); auto reserved = terms.u32(); auto start = terms.u64(); auto count = terms.u64(); if (!field || !term || !frequency || !reserved || !start || !count || *reserved != 0U || term->empty() || *frequency != *count || *start > postings.size() || *count > postings.size() - *start) return std::unexpected(fail(SegmentErrorCode::corruption, "invalid term record")); index::TermEntry entry{*frequency, {}}; entry.postings.reserve(static_cast<std::size_t>(*count)); for (std::uint64_t j = 0; j < *count; ++j) { const auto& raw = postings[static_cast<std::size_t>(*start + j)]; std::vector<std::uint32_t> posting_positions(positions.begin() + static_cast<std::ptrdiff_t>(raw.start), positions.begin() + static_cast<std::ptrdiff_t>(raw.start + raw.count)); entry.postings.push_back({InternalDocumentId(raw.document), raw.frequency, std::move(posting_positions)}); } if (!fields[*field].emplace(std::move(*term), std::move(entry)).second) return std::unexpected(fail(SegmentErrorCode::corruption, "duplicate segment term")); }
  if (!terms.done()) return std::unexpected(fail(SegmentErrorCode::corruption, "trailing term data"));
  SegmentReader reader(SegmentId(*segment_id), std::move(*schema), std::move(fields), std::move(documents), std::move(external_ids), std::move(statistics)); std::string reason; if (!reader.validate_invariants(&reason)) return std::unexpected(fail(SegmentErrorCode::corruption, "segment invariant failed: " + reason)); return reader;
}

const index::TermEntry* SegmentReader::lookup(std::string_view field, std::string_view term) const { const auto f = fields_.find(field); if (f == fields_.end()) return nullptr; const auto t = f->second.find(term); return t == f->second.end() ? nullptr : &t->second; }
const index::DocumentRecord* SegmentReader::document(const DocumentId& id) const { const auto it = documents_.find(id); return it == documents_.end() ? nullptr : &it->second; }
const index::DocumentRecord* SegmentReader::document(InternalDocumentId id) const { const auto* external = external_id(id); return external ? document(*external) : nullptr; }
std::optional<InternalDocumentId> SegmentReader::internal_id(const DocumentId& id) const noexcept { const auto* record = document(id); return record ? std::optional(record->internal_id) : std::nullopt; }
const DocumentId* SegmentReader::external_id(InternalDocumentId id) const noexcept { const auto it = external_ids_.find(id); return it == external_ids_.end() ? nullptr : &it->second; }
std::vector<InternalDocumentId> SegmentReader::live_document_ids() const { std::vector<InternalDocumentId> result; for (const auto& [id, external] : external_ids_) { const auto* record = document(external); if (record && !record->document.deleted) result.push_back(id); } return result; }
std::size_t SegmentReader::live_document_count() const noexcept { return static_cast<std::size_t>(std::ranges::count_if(documents_, [](const auto& item) { return !item.second.document.deleted; })); }
index::FieldStatistics SegmentReader::field_statistics(std::string_view field) const noexcept { const auto it = statistics_.find(field); return it == statistics_.end() ? index::FieldStatistics{} : it->second; }

bool SegmentReader::validate_invariants(std::string* reason) const {
  const auto reject = [&](std::string value) { if (reason) *reason = std::move(value); return false; };
  for (const auto& [field, dictionary] : fields_) for (const auto& [term, entry] : dictionary) {
    if (schema_.find(field) == nullptr || term.empty() || entry.document_frequency != entry.postings.size() || !std::ranges::is_sorted(entry.postings, {}, &index::Posting::document_id) || std::ranges::adjacent_find(entry.postings, {}, &index::Posting::document_id) != entry.postings.end()) return reject("invalid term entry");
    for (const auto& posting : entry.postings) { const auto* record = document(posting.document_id); if (!record || record->document.deleted || posting.term_frequency == 0U || posting.term_frequency != posting.positions.size() || !std::ranges::is_sorted(posting.positions) || std::ranges::adjacent_find(posting.positions) != posting.positions.end() || !record->field_lengths.contains(field)) return reject("invalid posting"); const auto terms = record->indexed_terms.find(field); if (terms == record->indexed_terms.end() || !std::ranges::binary_search(terms->second, term)) return reject("posting absent from document term references"); }
  }
  if (external_ids_.size() != documents_.size()) return reject("document mapping size mismatch");
  index::FieldStatisticsMap expected_statistics;
  for (const auto& [id, record] : documents_) {
    const auto external = external_ids_.find(record.internal_id); if (id != record.document.id || external == external_ids_.end() || external->second != id || (record.document.deleted && (!record.field_lengths.empty() || !record.indexed_terms.empty()))) return reject("invalid document record");
    for (const auto& [field, value] : record.document.fields) { (void)value; if (schema_.find(field) == nullptr) return reject("document references unknown field"); }
    for (const auto& [field, value] : record.document.stored_metadata) { (void)value; if (schema_.find(field) == nullptr) return reject("metadata references unknown field"); }
    if (record.document.deleted) continue;
    for (const auto& [field, length] : record.field_lengths) { if (schema_.find(field) == nullptr) return reject("length references unknown field"); auto& stats = expected_statistics[field]; ++stats.document_count; stats.total_length += length; }
    for (const auto& [field, terms] : record.indexed_terms) { if (!std::ranges::is_sorted(terms) || std::ranges::adjacent_find(terms) != terms.end()) return reject("invalid document term references"); for (const auto& term : terms) { const auto* entry = lookup(field, term); if (!entry || !std::ranges::binary_search(entry->postings, record.internal_id, {}, &index::Posting::document_id)) return reject("document term reference has no posting"); } }
  }
  if (expected_statistics.size() != statistics_.size()) return reject("field statistics size mismatch");
  for (const auto& [field, expected] : expected_statistics) { const auto actual = statistics_.find(field); if (actual == statistics_.end() || actual->second.document_count != expected.document_count || actual->second.total_length != expected.total_length) return reject("field statistics mismatch"); }
  return true;
}

}  // namespace dse::storage
