#include "dse/analysis/analyzer.hpp"
#include "dse/index/document_file.hpp"
#include "dse/index/in_memory_index.hpp"
#include "dse/query/executor.hpp"
#include "dse/query/parser.hpp"

#include <charconv>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

struct Arguments {
  std::filesystem::path documents;
  std::string query;
  std::size_t top_k{10};
};

void usage(std::ostream& output) {
  output << "Usage: dse_index_cli --documents PATH --query QUERY [--top-k K]\n";
}

std::optional<Arguments> parse_arguments(int argc, char** argv) {
  Arguments arguments;
  bool has_documents = false;
  bool has_query = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--help") {
      usage(std::cout);
      return std::nullopt;
    }
    if (index + 1 >= argc) {
      std::cerr << "missing value for " << option << '\n';
      usage(std::cerr);
      return std::nullopt;
    }
    const std::string_view value(argv[++index]);
    if (option == "--documents") {
      arguments.documents = value;
      has_documents = true;
    } else if (option == "--query") {
      arguments.query = value;
      has_query = true;
    } else if (option == "--top-k") {
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), arguments.top_k);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          arguments.top_k == 0U) {
        std::cerr << "--top-k must be a positive integer\n";
        return std::nullopt;
      }
    } else {
      std::cerr << "unknown option: " << option << '\n';
      usage(std::cerr);
      return std::nullopt;
    }
  }
  if (!has_documents || !has_query) {
    usage(std::cerr);
    return std::nullopt;
  }
  return arguments;
}

std::string json_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (byte < 0x20U) {
          escaped += "\\u00";
          escaped.push_back(hex[byte >> 4U]);
          escaped.push_back(hex[byte & 0x0fU]);
        } else {
          escaped.push_back(static_cast<char>(byte));
        }
    }
  }
  return escaped;
}

int run(const Arguments& arguments) {
  auto documents = dse::index::read_documents(arguments.documents);
  if (!documents) {
    std::cerr << "document error at line " << documents.error().line << ", column "
              << documents.error().column << ": " << documents.error().message << '\n';
    return 2;
  }

  dse::index::InMemoryIndex index;
  for (auto& document : *documents) {
    if (!index.put(std::move(document))) {
      std::cerr << "document rejected because its ID is empty or version is stale\n";
      return 2;
    }
  }
  std::string invariant_reason;
  if (!index.validate_invariants(&invariant_reason)) {
    std::cerr << "index invariant failed: " << invariant_reason << '\n';
    return 3;
  }

  auto query = dse::query::parse(arguments.query);
  if (!query) {
    std::cerr << "query error at byte " << query.error().position << ": "
              << query.error().message << '\n';
    return 2;
  }
  const dse::query::QueryExecutor executor(index);
  auto result = executor.search(**query, {.top_k = arguments.top_k});
  if (!result) {
    std::cerr << "execution error: " << result.error().message << '\n';
    return 3;
  }

  std::cout << std::setprecision(17) << "{\"indexed_documents\":" << index.live_document_count()
            << ",\"total_hits\":" << result->total_hits << ",\"hits\":[";
  for (std::size_t index_value = 0; index_value < result->hits.size(); ++index_value) {
    if (index_value != 0U) std::cout << ',';
    const auto& hit = result->hits[index_value];
    const auto* record = index.document(hit.document_id);
    std::cout << "{\"document_id\":\"" << json_escape(hit.document_id.value())
              << "\",\"score\":" << hit.score << ",\"fields\":{";
    if (record != nullptr) {
      bool first = true;
      for (const auto& [field, value] : record->document.fields) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << '"' << json_escape(field) << "\":\"" << json_escape(value) << '"';
      }
    }
    std::cout << "}}";
  }
  std::cout << "]}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const auto arguments = parse_arguments(argc, argv);
  if (!arguments) return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
  return run(*arguments);
}
