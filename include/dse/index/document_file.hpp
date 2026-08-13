#pragma once

#include "dse/document.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <istream>
#include <string>
#include <vector>

namespace dse::index {

enum class DocumentFileErrorCode {
  open_failed,
  invalid_header,
  invalid_column_count,
  invalid_escape,
  invalid_version,
  invalid_deleted,
  empty_document_id,
};

struct DocumentFileError {
  DocumentFileErrorCode code;
  std::size_t line;
  std::size_t column;
  std::string message;
};

[[nodiscard]] std::expected<std::vector<Document>, DocumentFileError> read_documents(
    std::istream& input);
[[nodiscard]] std::expected<std::vector<Document>, DocumentFileError> read_documents(
    const std::filesystem::path& path);

}  // namespace dse::index
