#include "dse/telemetry/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dse::telemetry {
namespace {
bool valid_name(std::string_view name) {
  return !name.empty() && std::ranges::all_of(name, [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == ':';
  });
}
}

void MetricsRegistry::increment(std::string_view name, std::uint64_t amount) {
  if (!valid_name(name)) return;
  std::lock_guard lock(mutex_); auto& value = counters_[std::string(name)];
  value = amount > std::numeric_limits<std::uint64_t>::max() - value
              ? std::numeric_limits<std::uint64_t>::max() : value + amount;
}
void MetricsRegistry::add_gauge(std::string_view name, std::int64_t amount) {
  if (!valid_name(name)) return;
  std::lock_guard lock(mutex_);
  auto& value = gauges_[std::string(name)];
  if (amount > 0 && value > std::numeric_limits<std::int64_t>::max() - amount)
    value = std::numeric_limits<std::int64_t>::max();
  else if (amount < 0 && value < std::numeric_limits<std::int64_t>::min() - amount)
    value = std::numeric_limits<std::int64_t>::min();
  else value += amount;
}
void MetricsRegistry::set_gauge(std::string_view name, std::int64_t value) {
  if (!valid_name(name)) return;
  std::lock_guard lock(mutex_);
  gauges_[std::string(name)] = value;
}
void MetricsRegistry::observe(std::string_view name, double value, std::vector<double> bounds) {
  if (!valid_name(name) || !std::isfinite(value) || !std::ranges::is_sorted(bounds) ||
      std::ranges::adjacent_find(bounds) != bounds.end()) return;
  std::lock_guard lock(mutex_); auto [found, inserted] = histograms_.try_emplace(std::string(name));
  if (inserted) {
    found->second.bounds = std::move(bounds);
    found->second.buckets.resize(found->second.bounds.size());
  } else if (found->second.bounds != bounds) return;
  for (std::size_t index = 0; index < found->second.bounds.size(); ++index)
    if (value <= found->second.bounds[index]) ++found->second.buckets[index];
  ++found->second.count; found->second.sum += value;
}
std::uint64_t MetricsRegistry::counter(std::string_view name) const { std::lock_guard lock(mutex_); const auto found = counters_.find(name); return found == counters_.end() ? 0U : found->second; }
std::int64_t MetricsRegistry::gauge(std::string_view name) const { std::lock_guard lock(mutex_); const auto found = gauges_.find(name); return found == gauges_.end() ? 0 : found->second; }
HistogramSnapshot MetricsRegistry::histogram(std::string_view name) const { std::lock_guard lock(mutex_); const auto found = histograms_.find(name); if (found == histograms_.end()) return {}; return {found->second.bounds, found->second.buckets, found->second.count, found->second.sum}; }
std::string MetricsRegistry::prometheus() const {
  std::lock_guard lock(mutex_); std::ostringstream output; output << std::setprecision(17);
  for (const auto& [name, value] : counters_) output << name << ' ' << value << '\n';
  for (const auto& [name, value] : gauges_) output << name << ' ' << value << '\n';
  for (const auto& [name, histogram] : histograms_) {
    for (std::size_t index = 0; index < histogram.bounds.size(); ++index)
      output << name << "_bucket{le=\"" << histogram.bounds[index] << "\"} " << histogram.buckets[index] << '\n';
    output << name << "_bucket{le=\"+Inf\"} " << histogram.count << '\n';
    output << name << "_sum " << histogram.sum << '\n' << name << "_count " << histogram.count << '\n';
  }
  return output.str();
}
}  // namespace dse::telemetry
