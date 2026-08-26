#pragma once

#include "dse/storage/segment.hpp"
#include "dse/types.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dse::storage {

struct ManifestSegment {
  SegmentId id;
  std::string filename;
  auto operator<=>(const ManifestSegment&) const = default;
};

struct IndexManifest {
  GenerationId generation;
  std::vector<ManifestSegment> segments;
  auto operator<=>(const IndexManifest&) const = default;
};

enum class ManifestErrorCode {
  io_error,
  corruption,
  unsupported_version,
  resource_limit,
  invalid_manifest,
  injected_failure,
};

struct ManifestError {
  ManifestErrorCode code;
  std::string message;
};

enum class ManifestFaultPoint { none, after_manifest_publish };

struct ManifestPublishOptions {
  ManifestFaultPoint fault_point{ManifestFaultPoint::none};
};

class OpenGeneration {
 public:
  [[nodiscard]] const IndexManifest& manifest() const noexcept { return manifest_; }
  [[nodiscard]] const std::vector<std::shared_ptr<const SegmentReader>>& segments() const noexcept {
    return segments_;
  }

 private:
  friend class ManifestStore;
  OpenGeneration(IndexManifest manifest,
                 std::vector<std::shared_ptr<const SegmentReader>> segments)
      : manifest_(std::move(manifest)), segments_(std::move(segments)) {}
  IndexManifest manifest_;
  std::vector<std::shared_ptr<const SegmentReader>> segments_;
};

class ManifestStore {
 public:
  explicit ManifestStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

  [[nodiscard]] std::expected<void, ManifestError> publish(
      const IndexManifest& manifest, const ManifestPublishOptions& options = {}) const;
  [[nodiscard]] std::expected<IndexManifest, ManifestError> load_current() const;
  [[nodiscard]] std::expected<OpenGeneration, ManifestError> open_current(
      const SegmentReadLimits& limits = {}) const;

 private:
  std::filesystem::path directory_;
};

}  // namespace dse::storage
