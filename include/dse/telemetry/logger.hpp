#pragma once

#include <string_view>

namespace dse::telemetry {

enum class LogLevel { debug, info, warning, error };

class Logger {
 public:
  virtual ~Logger() = default;
  virtual void log(LogLevel level, std::string_view operation,
                   std::string_view message) = 0;
};

class NullLogger final : public Logger {
 public:
  void log(LogLevel, std::string_view, std::string_view) override {}
};

}  // namespace dse::telemetry
