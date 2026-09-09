#include "dse/analysis/analyzer.hpp"
#include "dse/evaluation/metrics.hpp"
#include "dse/index/in_memory_index.hpp"
#include "dse/query/ast.hpp"
#include "dse/query/executor.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

namespace {
using Sections = std::map<std::string, std::map<char, std::string>>;
Sections read_sections(const std::filesystem::path& path) {
  std::ifstream input(path); Sections records; std::string line; std::string id; char section{};
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with(".I ")) {
      id = line.substr(3);
      const auto first = id.find_first_not_of('0');
      if (first != std::string::npos) id.erase(0, first);
      section = 0; records[id];
    }
    else if (line.size() == 2U && line[0] == '.') section = line[1];
    else if (!id.empty() && section != 0) { auto& value = records[id][section]; if (!value.empty()) value += ' '; value += line; }
  }
  return records;
}
std::vector<std::string> read_record_order(const std::filesystem::path& path) {
  std::ifstream input(path); std::vector<std::string> ids; std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.starts_with(".I ")) continue;
    auto id = line.substr(3); const auto first = id.find_first_not_of('0');
    if (first != std::string::npos) id.erase(0, first);
    ids.push_back(std::move(id));
  }
  return ids;
}
dse::query::Query token_query(std::string_view text) {
  const auto tokens = dse::analysis::StandardAnalyzer().analyze(text);
  dse::query::Query result;
  for (const auto& token : tokens) {
    auto term = dse::query::make_query<dse::query::TermQuery>(0, token.term);
    result = result ? dse::query::make_query<dse::query::OrQuery>(0, std::move(result), std::move(term))
                    : std::move(term);
  }
  return result;
}
}

int main(int argc, char** argv) {
  if (argc != 4) { std::cerr << "usage: dse_cranfield_eval DATA_DIRECTORY OUTPUT_JSON TOP_K\n"; return 2; }
  const std::filesystem::path directory(argv[1]); std::size_t top_k{};
  try { top_k = std::stoul(argv[3]); } catch (...) { return 2; }
  const auto documents = read_sections(directory / "cran.all.1400");
  const auto queries = read_sections(directory / "cran.qry");
  const auto query_order = read_record_order(directory / "cran.qry");
  if (documents.size() != 1'400U || queries.size() != 225U || top_k == 0U) {
    std::cerr << "unexpected Cranfield collection shape\n"; return 3;
  }
  std::map<std::string, dse::evaluation::RelevanceJudgments> qrels;
  std::ifstream relevance(directory / "cranqrel"); std::string query_id; std::string document_id; int code{};
  while (relevance >> query_id >> document_id >> code) {
    if (code != -1 && (code < 1 || code > 4)) return 4;
    qrels[query_id][dse::DocumentId(document_id)] =
        code == -1 ? 0U : static_cast<unsigned>(5 - code);
  }
  dse::index::InMemoryIndex index;
  for (const auto& [id, fields] : documents) {
    const auto title = fields.find('T'); const auto body = fields.find('W');
    auto inserted = index.put({.id = dse::DocumentId(id),
        .fields = {{"title", title == fields.end() ? "" : title->second},
                   {"body", body == fields.end() ? "" : body->second}}, .stored_metadata = {}, .version = 1});
    if (!inserted) { std::cerr << inserted.error().message << '\n'; return 5; }
  }
  double precision{}, recall{}, average_precision{}, reciprocal_rank{}, ndcg{}; std::size_t evaluated{};
  for (std::size_t query_number = 0; query_number < query_order.size(); ++query_number) {
    const auto& fields = queries.at(query_order[query_number]);
    const auto text = fields.find('W'); if (text == fields.end()) continue;
    auto query = token_query(text->second); if (!query) continue;
    dse::query::SearchOptions options; options.top_k = top_k;
    auto result = dse::query::QueryExecutor(index).search(*query, options); if (!result) return 6;
    std::vector<dse::DocumentId> ranked; ranked.reserve(result->hits.size());
    for (const auto& hit : result->hits) ranked.push_back(hit.document_id);
    const auto judgments = qrels.find(std::to_string(query_number + 1U));
    const auto metrics = dse::evaluation::evaluate(ranked,
        judgments == qrels.end() ? dse::evaluation::RelevanceJudgments{} : judgments->second, top_k);
    precision += metrics.precision_at_k; recall += metrics.recall_at_k;
    average_precision += metrics.average_precision; reciprocal_rank += metrics.reciprocal_rank;
    ndcg += metrics.ndcg_at_k; ++evaluated;
  }
  std::ofstream output(argv[2]); if (!output || evaluated == 0U) return 7;
  output << std::setprecision(10) << "{\n  \"dataset\": \"Cranfield 1400\",\n"
      << "  \"documents\": " << documents.size() << ",\n  \"queries\": " << evaluated
      << ",\n  \"qrels\": " << [&] { std::size_t count{}; for (const auto& [id, values] : qrels) { (void)id; count += values.size(); } return count; }()
      << ",\n  \"top_k\": " << top_k << ",\n  \"macro_precision_at_k\": " << precision / static_cast<double>(evaluated)
      << ",\n  \"macro_recall_at_k\": " << recall / static_cast<double>(evaluated)
      << ",\n  \"map_at_k\": " << average_precision / static_cast<double>(evaluated)
      << ",\n  \"mrr_at_k\": " << reciprocal_rank / static_cast<double>(evaluated)
      << ",\n  \"macro_ndcg_at_k\": " << ndcg / static_cast<double>(evaluated) << "\n}\n";
  return output ? 0 : 8;
}
