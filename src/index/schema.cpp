#include "dse/index/schema.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace dse::index {
namespace {

SchemaError fail(SchemaErrorCode code, std::string field, std::string message) {
  return {code, std::move(field), std::move(message)};
}

bool valid_iso_date(std::string_view value) {
  if (value.size() != 10U || value[4] != '-' || value[7] != '-') return false;
  const auto digits = [&](std::size_t start, std::size_t count, int& output) {
    const auto parsed = std::from_chars(value.data() + start, value.data() + start + count, output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + start + count;
  };
  int year = 0;
  int month = 0;
  int day = 0;
  if (!digits(0, 4, year) || !digits(5, 2, month) || !digits(8, 2, day) || year < 1 ||
      month < 1 || month > 12) {
    return false;
  }
  constexpr int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maximum_day = days_per_month[month - 1];
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (month == 2 && leap) maximum_day = 29;
  return day >= 1 && day <= maximum_day;
}

}  // namespace

std::expected<IndexSchema, SchemaError> IndexSchema::create(std::vector<FieldDefinition> fields) {
  IndexSchema schema;
  for (auto& field : fields) {
    if (field.name.empty()) {
      return std::unexpected(
          fail(SchemaErrorCode::empty_field_name, {}, "field name must not be empty"));
    }
    if (!std::isfinite(field.boost) || field.boost < 0.0) {
      return std::unexpected(
          fail(SchemaErrorCode::invalid_boost, field.name,
               "field boost must be finite and non-negative"));
    }
    const bool needs_analyzer = field.indexed &&
                                (field.type == FieldType::text || field.type == FieldType::keyword);
    if (needs_analyzer && !field.analyzer) {
      return std::unexpected(fail(SchemaErrorCode::missing_analyzer, field.name,
                                  "indexed text and keyword fields require an analyzer"));
    }
    if (!needs_analyzer && field.analyzer) {
      return std::unexpected(fail(SchemaErrorCode::unexpected_analyzer, field.name,
                                  "typed or unindexed fields must not have an analyzer"));
    }
    const auto [iterator, inserted] = schema.fields_.emplace(field.name, std::move(field));
    if (!inserted) {
      return std::unexpected(fail(SchemaErrorCode::duplicate_field, iterator->first,
                                  "field appears more than once in schema"));
    }
  }
  return schema;
}

IndexSchema IndexSchema::default_schema() {
  const auto standard = std::make_shared<const analysis::StandardAnalyzer>();
  const auto keyword = std::make_shared<const analysis::KeywordAnalyzer>();
  auto schema = create({{"title", FieldType::text, true, true, 1.0, standard},
                        {"body", FieldType::text, true, true, 1.0, standard},
                        {"tags", FieldType::keyword, true, true, 1.0, keyword},
                        {"timestamp", FieldType::timestamp, false, true, 1.0, nullptr}});
  return std::move(schema).value();
}

const FieldDefinition* IndexSchema::find(std::string_view field) const noexcept {
  const auto iterator = fields_.find(field);
  return iterator == fields_.end() ? nullptr : &iterator->second;
}

std::expected<void, SchemaError> IndexSchema::validate_value(std::string_view field,
                                                             std::string_view value) const {
  const auto* definition = find(field);
  if (definition == nullptr) {
    return std::unexpected(
        fail(SchemaErrorCode::unknown_field, std::string(field), "field is not declared in schema"));
  }
  if (definition->type == FieldType::int64) {
    std::int64_t parsed_value = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      return std::unexpected(fail(SchemaErrorCode::invalid_typed_value, std::string(field),
                                  "value is not a valid int64"));
    }
  } else if (definition->type == FieldType::timestamp && !valid_iso_date(value)) {
    return std::unexpected(fail(SchemaErrorCode::invalid_typed_value, std::string(field),
                                "timestamp must use a valid YYYY-MM-DD date"));
  }
  return {};
}

}  // namespace dse::index
