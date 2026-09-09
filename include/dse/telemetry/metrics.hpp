#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dse::telemetry {

struct HistogramSnapshot {
  std::vector<double> upper_bounds;
  std::vector<std::uint64_t> cumulative_counts;
  std::uint64_t count{};
  double sum{};
};

class MetricsRegistry {
 public:
  void increment(std::string_view name, std::uint64_t amount = 1);
  void add_gauge(std::string_view name, std::int64_t amount);
  void set_gauge(std::string_view name, std::int64_t value);
  void observe(std::string_view name, double value,
               std::vector<double> bounds = {0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0});
  [[nodiscard]] std::uint64_t counter(std::string_view name) const;
  [[nodiscard]] std::int64_t gauge(std::string_view name) const;
  [[nodiscard]] HistogramSnapshot histogram(std::string_view name) const;
  [[nodiscard]] std::string prometheus() const;

 private:
  struct Histogram { std::vector<double> bounds; std::vector<std::uint64_t> buckets; std::uint64_t count{}; double sum{}; };
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t, std::less<>> counters_;
  std::map<std::string, std::int64_t, std::less<>> gauges_;
  std::map<std::string, Histogram, std::less<>> histograms_;
};

}  // namespace dse::telemetry
