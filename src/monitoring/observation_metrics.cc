/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "observation_metrics.h"
#include <algorithm>
#include <limits>
#include <prometheus/client_metric.h>
#include <prometheus/metric_family.h>

namespace ganesha_monitoring {
std::shared_ptr<ObservationMetrics> observations;

struct proto_op *ObservationMetrics::Register(const std::string &name, const std::string &help,
                                              const Labels &labels, bool histogram,
                                              struct proto_op *source) {
  std::lock_guard<std::mutex> lock(registration);
  const auto key = std::make_pair(name, labels);
  auto found = views.find(key);
  if (found == views.end()) {
    if (source == nullptr) {
      auto record = std::make_unique<OwnedRecord>();
      source = &record->stats;
      records.emplace_back(std::move(record));
    }
    found = views.emplace(key, View{name, help, labels, histogram, {source}}).first;
  } else if (source != nullptr &&
             std::find(found->second.sources.begin(), found->second.sources.end(), source) ==
                 found->second.sources.end()) {
    found->second.sources.push_back(source);
  }
  if (source == nullptr) {
    source = found->second.sources.front();
  }
  if (histogram) {
    observation_enable_histogram(source);
  }
  return source;
}

struct proto_op *ObservationMetrics::Counter(const std::string &name, const std::string &help,
                                             const Labels &labels, struct proto_op *source) {
  return Register(name, help, labels, false, source);
}

struct proto_op *ObservationMetrics::Duration(const std::string &name, const std::string &help,
                                              const Labels &labels) {
  return Register(name, help, labels, true, nullptr);
}

void ObservationMetrics::Histogram(const std::string &name, const std::string &help,
                                   const Labels &labels, struct proto_op *source) {
  Register(name, help, labels, true, source);
}

std::vector<prometheus::MetricFamily> ObservationMetrics::Collect() const {
  std::vector<View> snapshotViews;
  {
    std::lock_guard<std::mutex> lock(registration);
    for (const auto &entry : views) {
      snapshotViews.push_back(entry.second);
    }
  }
  std::map<std::string, prometheus::MetricFamily> families;
  /* One snapshot per source per scrape also makes aliases agree, including
   * while a reset or another selected completion happens during collection. */
  std::map<struct proto_op *, struct observation_snapshot> snapshots;
  for (const auto &view : snapshotViews) {
    auto &family = families[view.name];
    family.name = view.name;
    family.help = view.help;
    family.type =
        view.histogram ? prometheus::MetricType::Histogram : prometheus::MetricType::Counter;
    prometheus::ClientMetric metric;
    for (const auto &label : view.labels) {
      metric.label.push_back({label.first, label.second});
    }
    uint64_t buckets[OBSERVATION_BUCKET_COUNT]{};
    uint64_t samples = 0;
    uint64_t sum = 0;
    for (auto *source : view.sources) {
      auto found = snapshots.find(source);
      if (found == snapshots.end()) {
        struct observation_snapshot snapshot;
        observation_snapshot(source, true, &snapshot);
        found = snapshots.emplace(source, snapshot).first;
      }
      const auto &snapshot = found->second;
      metric.counter.value += snapshot.total;
      samples += snapshot.latency.samples + snapshot.dup_latency.samples;
      sum += snapshot.latency.latency + snapshot.dup_latency.latency;
      for (size_t bucket = 0; bucket < OBSERVATION_BUCKET_COUNT; ++bucket) {
        buckets[bucket] += snapshot.buckets[bucket];
      }
    }
    if (view.histogram) {
      metric.histogram.sample_count = samples;
      metric.histogram.sample_sum = static_cast<double>(sum) / 1000000;
      uint64_t cumulative = 0;
      for (size_t bucket = 0; bucket < OBSERVATION_BUCKET_COUNT; ++bucket) {
        cumulative += buckets[bucket];
        metric.histogram.bucket.push_back(
            {cumulative, static_cast<double>(observation_bucket_ns[bucket]) / 1000000});
      }
      metric.histogram.bucket.push_back({samples, std::numeric_limits<double>::infinity()});
    }
    family.metric.push_back(std::move(metric));
  }
  std::vector<prometheus::MetricFamily> result;
  for (auto &family : families) {
    result.push_back(std::move(family.second));
  }
  return result;
}
}  // namespace ganesha_monitoring
