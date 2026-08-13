#include "dse/index/document_file.hpp"

#include <charconv>
#include <fstream>
#include <string_view>

namespace dse::index {
namespace {

constexpr std::string_view kHeader = "document_id\tversion\tdeleted\ttitle\tbody\ttags\ttimestamp";

std::unexpected<DocumentFileError> fail(DocumentFileErrorCode code, std::size_t line,
                                        std::size_t column, std::string message) {
  return std::unexpected(DocumentFileError{code, line, column, std::move(message)});
}

std::expected<std::vector<std::string>, DocumentFileError> split_line(std::string_view line,
                                                                      std::size_t line_number) {
  std::vector<std::string> columns(1);
  for (std::size_t cursor = 0; cursor < line.size(); ++cursor) {
    if (line[cursor] == '\t') {
      columns.emplace_back();
      continue;
    }
    if (line[cursor] != '\\') {
      columns.back().push_back(line[cursor]);
      continue;
    }
    if (++cursor >= line.size()) {
      return fail(DocumentFileErrorCode::invalid_escape, line_number, cursor,
                  "trailing backslash in escaped TSV value");
    }
    switch (line[cursor]) {
      case 't':
        columns.back().push_back('\t');
        break;
      case 'n':
        columns.back().push_back('\n');
        break;
      case 'r':
        columns.back().push_back('\r');
        break;
      case '\\':
        columns.back().push_back('\\');
        break;
      default:
        return fail(DocumentFileErrorCode::invalid_escape, line_number, cursor,
                    "unsupported TSV escape sequence");
    }
  }
  return columns;
}

}  // namespace

std::expected<std::vector<Document>, DocumentFileError> read_documents(std::istream& input) {
  std::string line;
  if (!std::getline(input, line) || line != kHeader) {
    return fail(DocumentFileErrorCode::invalid_header, 1, 0,
                "expected escaped TSV header: " + std::string(kHeader));
  }

  std::vector<Document> documents;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty()) continue;
    auto columns = split_line(line, line_number);
    if (!columns) return std::unexpected(columns.error());
    if (columns->size() != 7U) {
      return fail(DocumentFileErrorCode::invalid_column_count, line_number, line.size(),
                  "document row must contain exactly seven columns");
    }
    if ((*columns)[0].empty()) {
      return fail(DocumentFileErrorCode::empty_document_id, line_number, 0,
                  "document ID must not be empty");
    }

    std::uint64_t version = 0;
    const auto& version_text = (*columns)[1];
    const auto version_result = std::from_chars(
        version_text.data(), version_text.data() + version_text.size(), version);
    if (version_result.ec != std::errc{} ||
        version_result.ptr != version_text.data() + version_text.size() || version == 0U) {
      return fail(DocumentFileErrorCode::invalid_version, line_number, 1,
                  "version must be a positive uint64 value");
    }
    const auto& deleted_text = (*columns)[2];
    if (deleted_text != "0" && deleted_text != "1") {
      return fail(DocumentFileErrorCode::invalid_deleted, line_number, 2,
                  "deleted must be 0 or 1");
    }

    Document document{.id = DocumentId(std::move((*columns)[0])),
                      .fields = {{"body", std::move((*columns)[4])},
                                 {"tags", std::move((*columns)[5])},
                                 {"title", std::move((*columns)[3])}},
                      .stored_metadata = {{"timestamp", std::move((*columns)[6])}},
                      .version = version,
                      .deleted = deleted_text == "1"};
    documents.push_back(std::move(document));
  }
  return documents;
}

std::expected<std::vector<Document>, DocumentFileError> read_documents(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return fail(DocumentFileErrorCode::open_failed, 0, 0,
                "unable to open document file: " + path.string());
  }
  return read_documents(input);
}

}  // namespace dse::index
