#include "dse/telemetry/metrics.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

TEST(Metrics, ConcurrentCountersGaugesHistogramsAndPrometheusExport) {
  dse::telemetry::MetricsRegistry metrics;
  std::vector<std::thread> workers;
  for (std::size_t worker = 0; worker < 4U; ++worker) workers.emplace_back([&] {
    for (std::size_t item = 0; item < 1'000U; ++item) metrics.increment("queries_total");
  });
  for (auto& worker : workers) worker.join();
  metrics.set_gauge("queue_depth", 3); metrics.add_gauge("queue_depth", -1);
  metrics.observe("latency_seconds", 0.005, {0.001, 0.01});
  metrics.observe("latency_seconds", 0.1, {0.001, 0.01});
  EXPECT_EQ(metrics.counter("queries_total"), 4'000U);
  EXPECT_EQ(metrics.gauge("queue_depth"), 2);
  const auto histogram = metrics.histogram("latency_seconds");
  ASSERT_EQ(histogram.cumulative_counts.size(), 2U);
  EXPECT_EQ(histogram.cumulative_counts[0], 0U); EXPECT_EQ(histogram.cumulative_counts[1], 1U);
  EXPECT_EQ(histogram.count, 2U);
  const auto text = metrics.prometheus();
  EXPECT_NE(text.find("queries_total 4000"), std::string::npos);
  EXPECT_NE(text.find("latency_seconds_bucket{le=\"+Inf\"} 2"), std::string::npos);
}
