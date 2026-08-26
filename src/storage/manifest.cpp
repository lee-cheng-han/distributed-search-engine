#include "dse/storage/manifest.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <set>
#include <span>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dse::storage {
namespace {

constexpr std::string_view kManifestMagic = "DSEMAN01";
constexpr std::string_view kCurrentMagic = "DSECUR01";
constexpr std::uint16_t kMajor = 1;
constexpr std::uint16_t kMinor = 0;
constexpr std::uint64_t kMaximumManifestBytes = 16U << 20U;
constexpr std::uint64_t kMaximumSegments = 1'000'000;

ManifestError fail(ManifestErrorCode code, std::string message) {
  return {code, std::move(message)};
}

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

class Buffer {
 public:
  void u16(std::uint16_t value) { integer(value, 2); }
  void u32(std::uint32_t value) { integer(value, 4); }
  void u64(std::uint64_t value) { integer(value, 8); }
  void text(std::string_view value) {
    u64(value.size());
    for (const char byte : value) bytes_.push_back(static_cast<std::byte>(byte));
  }
  void raw(std::string_view value) {
    for (const char byte : value) bytes_.push_back(static_cast<std::byte>(byte));
  }
  [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
 private:
  void integer(std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
      bytes_.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
  std::vector<std::byte> bytes_;
};

class Cursor {
 public:
  explicit Cursor(std::span<const std::byte> bytes) : bytes_(bytes) {}
  std::expected<std::uint16_t, ManifestError> u16() { auto value = integer(2); if (!value) return std::unexpected(value.error()); return static_cast<std::uint16_t>(*value); }
  std::expected<std::uint32_t, ManifestError> u32() { auto value = integer(4); if (!value) return std::unexpected(value.error()); return static_cast<std::uint32_t>(*value); }
  std::expected<std::uint64_t, ManifestError> u64() { return integer(8); }
  std::expected<std::string, ManifestError> text() {
    auto length = u64(); if (!length) return std::unexpected(length.error());
    if (*length > remaining()) return std::unexpected(fail(ManifestErrorCode::corruption, "truncated manifest string"));
    std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), static_cast<std::size_t>(*length));
    offset_ += static_cast<std::size_t>(*length); return result;
  }
  [[nodiscard]] bool done() const { return offset_ == bytes_.size(); }
 private:
  [[nodiscard]] std::size_t remaining() const { return bytes_.size() - offset_; }
  std::expected<std::uint64_t, ManifestError> integer(unsigned width) {
    if (remaining() < width) return std::unexpected(fail(ManifestErrorCode::corruption, "truncated manifest integer"));
    std::uint64_t value{}; for (unsigned index = 0; index < width; ++index) value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U); offset_ += width; return value;
  }
  std::span<const std::byte> bytes_; std::size_t offset_{};
};

std::expected<void, ManifestError> sync_file(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot open file for fsync"));
  const int result = ::fsync(descriptor); const int saved = errno; ::close(descriptor);
  if (result != 0) return std::unexpected(fail(ManifestErrorCode::io_error, "file fsync failed: " + std::error_code(saved, std::generic_category()).message()));
#else
  (void)path;
#endif
  return {};
}

std::expected<void, ManifestError> sync_directory(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot open directory for fsync"));
  const int result = ::fsync(descriptor); const int saved = errno; ::close(descriptor);
  if (result != 0) return std::unexpected(fail(ManifestErrorCode::io_error, "directory fsync failed: " + std::error_code(saved, std::generic_category()).message()));
#else
  (void)path;
#endif
  return {};
}

std::expected<void, ManifestError> durable_replace(const std::filesystem::path& temporary,
                                                   const std::filesystem::path& target,
                                                   std::span<const std::byte> bytes) {
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot create metadata temporary file"));
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); output.flush();
  if (!output) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot write metadata temporary file"));
  output.close(); auto synced = sync_file(temporary); if (!synced) return synced;
  std::error_code rename_error; std::filesystem::rename(temporary, target, rename_error);
  if (rename_error) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot publish metadata: " + rename_error.message()));
  return sync_directory(target.parent_path());
}

std::expected<std::vector<std::byte>, ManifestError> read_file(const std::filesystem::path& path) {
  std::error_code error; const auto size = std::filesystem::file_size(path, error);
  if (error) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot stat metadata: " + error.message()));
  if (size > kMaximumManifestBytes) return std::unexpected(fail(ManifestErrorCode::resource_limit, "metadata file limit exceeded"));
  std::vector<std::byte> bytes(static_cast<std::size_t>(size)); std::ifstream input(path, std::ios::binary);
  if (!input || !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot read metadata"));
  return bytes;
}

std::string manifest_name(GenerationId generation) {
  return "MANIFEST-" + std::to_string(generation.value());
}

std::expected<IndexManifest, ManifestError> decode_manifest(std::span<const std::byte> bytes) {
  if (bytes.size() < 32U || std::memcmp(bytes.data(), kManifestMagic.data(), 8) != 0) return std::unexpected(fail(ManifestErrorCode::corruption, "invalid manifest magic"));
  Cursor cursor(bytes.subspan(8)); auto major = cursor.u16(); auto minor = cursor.u16(); auto generation = cursor.u64(); auto count = cursor.u64(); auto checksum = cursor.u32();
  if (!major || !minor || !generation || !count || !checksum) return std::unexpected(fail(ManifestErrorCode::corruption, "truncated manifest header"));
  if (*major != kMajor || *minor > kMinor) return std::unexpected(fail(ManifestErrorCode::unsupported_version, "unsupported manifest version"));
  if (*generation == 0U || *count == 0U) return std::unexpected(fail(ManifestErrorCode::invalid_manifest, "manifest generation and segments must be nonzero"));
  if (*count > kMaximumSegments) return std::unexpected(fail(ManifestErrorCode::resource_limit, "manifest segment limit exceeded"));
  if (crc32c(bytes.subspan(32)) != *checksum) return std::unexpected(fail(ManifestErrorCode::corruption, "manifest checksum mismatch"));
  IndexManifest manifest{GenerationId(*generation), {}}; std::set<SegmentId> ids; std::set<std::string> names;
  for (std::uint64_t index = 0; index < *count; ++index) { auto id = cursor.u64(); auto name = cursor.text(); if (!id || !name || *id == 0U || name->empty() || std::filesystem::path(*name).is_absolute() || name->find('/') != std::string::npos || name->find('\\') != std::string::npos || !ids.emplace(SegmentId(*id)).second || !names.emplace(*name).second) return std::unexpected(fail(ManifestErrorCode::invalid_manifest, "invalid or duplicate manifest segment")); manifest.segments.push_back({SegmentId(*id), std::move(*name)}); }
  if (!cursor.done()) return std::unexpected(fail(ManifestErrorCode::corruption, "trailing manifest data"));
  return manifest;
}

Buffer encode_manifest(const IndexManifest& manifest) {
  Buffer payload; for (const auto& segment : manifest.segments) { payload.u64(segment.id.value()); payload.text(segment.filename); }
  Buffer result; result.raw(kManifestMagic); result.u16(kMajor); result.u16(kMinor); result.u64(manifest.generation.value()); result.u64(manifest.segments.size()); result.u32(crc32c(payload.bytes())); result.raw(std::string_view(reinterpret_cast<const char*>(payload.bytes().data()), payload.bytes().size())); return result;
}

Buffer encode_current(std::string_view filename) {
  Buffer payload; payload.text(filename); Buffer result; result.raw(kCurrentMagic); result.u32(crc32c(payload.bytes())); result.raw(std::string_view(reinterpret_cast<const char*>(payload.bytes().data()), payload.bytes().size())); return result;
}

std::expected<std::string, ManifestError> decode_current(std::span<const std::byte> bytes) {
  if (bytes.size() < 20U || std::memcmp(bytes.data(), kCurrentMagic.data(), 8) != 0) return std::unexpected(fail(ManifestErrorCode::corruption, "invalid CURRENT magic"));
  Cursor header(bytes.subspan(8)); auto checksum = header.u32(); if (!checksum || crc32c(bytes.subspan(12)) != *checksum) return std::unexpected(fail(ManifestErrorCode::corruption, "CURRENT checksum mismatch")); auto name = header.text(); if (!name || !header.done() || name->empty() || std::filesystem::path(*name).is_absolute() || name->find('/') != std::string::npos) return std::unexpected(fail(ManifestErrorCode::corruption, "invalid CURRENT target")); return name;
}

}  // namespace

std::expected<void, ManifestError> ManifestStore::publish(
    const IndexManifest& manifest, const ManifestPublishOptions& options) const {
  if (manifest.generation.value() == 0U || manifest.segments.empty()) return std::unexpected(fail(ManifestErrorCode::invalid_manifest, "manifest generation and segments must be nonzero"));
  std::error_code current_error;
  const bool has_current = std::filesystem::exists(directory_ / "CURRENT", current_error);
  if (current_error)
    return std::unexpected(fail(ManifestErrorCode::io_error,
                                "cannot inspect current generation: " +
                                    current_error.message()));
  if (has_current) {
    auto current = load_current();
    if (!current) return std::unexpected(current.error());
    if (manifest.generation.value() <= current->generation.value())
      return std::unexpected(fail(ManifestErrorCode::invalid_manifest,
                                  "generation must increase monotonically"));
  }
  std::set<SegmentId> ids; std::set<std::string> names;
  for (const auto& segment : manifest.segments) {
    if (segment.id.value() == 0U || segment.filename.empty() || std::filesystem::path(segment.filename).is_absolute() || segment.filename.find('/') != std::string::npos || segment.filename.find('\\') != std::string::npos || !ids.insert(segment.id).second || !names.insert(segment.filename).second) return std::unexpected(fail(ManifestErrorCode::invalid_manifest, "manifest contains invalid or duplicate segment"));
    auto opened = SegmentReader::open(directory_ / segment.filename); if (!opened) return std::unexpected(fail(ManifestErrorCode::invalid_manifest, "manifest segment cannot be opened: " + opened.error().message)); if (opened->segment_id() != segment.id) return std::unexpected(fail(ManifestErrorCode::invalid_manifest, "manifest segment ID mismatch"));
  }
  std::error_code create_error; std::filesystem::create_directories(directory_, create_error); if (create_error) return std::unexpected(fail(ManifestErrorCode::io_error, "cannot create index directory"));
  const auto name = manifest_name(manifest.generation); const auto encoded = encode_manifest(manifest);
  auto published = durable_replace(directory_ / (name + ".tmp"), directory_ / name, encoded.bytes()); if (!published) return published;
  if (options.fault_point == ManifestFaultPoint::after_manifest_publish) return std::unexpected(fail(ManifestErrorCode::injected_failure, "injected failure after manifest publication"));
  const auto current = encode_current(name); return durable_replace(directory_ / "CURRENT.tmp", directory_ / "CURRENT", current.bytes());
}

std::expected<IndexManifest, ManifestError> ManifestStore::load_current() const {
  auto current_bytes = read_file(directory_ / "CURRENT"); if (!current_bytes) return std::unexpected(current_bytes.error()); auto name = decode_current(*current_bytes); if (!name) return std::unexpected(name.error()); auto manifest_bytes = read_file(directory_ / *name); if (!manifest_bytes) return std::unexpected(manifest_bytes.error()); return decode_manifest(*manifest_bytes);
}

std::expected<OpenGeneration, ManifestError> ManifestStore::open_current(
    const SegmentReadLimits& limits) const {
  auto manifest = load_current(); if (!manifest) return std::unexpected(manifest.error()); std::vector<std::shared_ptr<const SegmentReader>> readers; readers.reserve(manifest->segments.size()); for (const auto& segment : manifest->segments) { auto reader = SegmentReader::open(directory_ / segment.filename, limits); if (!reader) return std::unexpected(fail(ManifestErrorCode::corruption, "cannot open manifest segment: " + reader.error().message)); if (reader->segment_id() != segment.id) return std::unexpected(fail(ManifestErrorCode::corruption, "manifest segment ID mismatch")); readers.push_back(std::make_shared<const SegmentReader>(std::move(*reader))); } return OpenGeneration(std::move(*manifest), std::move(readers));
}

}  // namespace dse::storage
