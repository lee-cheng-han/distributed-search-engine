#pragma once

#include "dse/analysis/analyzer.hpp"

#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dse::index {

enum class FieldType { text, keyword, int64, timestamp };

struct FieldDefinition {
  std::string name;
  FieldType type{FieldType::text};
  bool indexed{true};
  bool stored{true};
  double boost{1.0};
  std::shared_ptr<const analysis::Analyzer> analyzer;
};

enum class SchemaErrorCode {
  empty_field_name,
  duplicate_field,
  invalid_boost,
  missing_analyzer,
  unexpected_analyzer,
  unknown_field,
  field_not_indexed,
  field_not_stored,
  invalid_typed_value,
};

struct SchemaError {
  SchemaErrorCode code;
  std::string field;
  std::string message;
};

class IndexSchema {
 public:
  [[nodiscard]] static std::expected<IndexSchema, SchemaError> create(
      std::vector<FieldDefinition> fields);
  [[nodiscard]] static IndexSchema default_schema();

  [[nodiscard]] const FieldDefinition* find(std::string_view field) const noexcept;
  [[nodiscard]] std::expected<void, SchemaError> validate_value(std::string_view field,
                                                                std::string_view value) const;
  [[nodiscard]] const std::map<std::string, FieldDefinition, std::less<>>& fields() const noexcept {
    return fields_;
  }

 private:
  std::map<std::string, FieldDefinition, std::less<>> fields_;
};

}  // namespace dse::index
