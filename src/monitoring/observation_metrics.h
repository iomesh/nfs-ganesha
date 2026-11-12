/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef GANESHA_OBSERVATION_METRICS_H
#define GANESHA_OBSERVATION_METRICS_H

#include "observation_stats.h"
#include <prometheus/collectable.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ganesha_monitoring {

/* A view registry, not a second accumulator. Records and labels are registered
 * on cold paths; request caches retain direct pointers to their records. */
class ObservationMetrics : public prometheus::Collectable {
 public:
  using Labels = std::map<std::string, std::string>;
  struct proto_op *Counter(const std::string &name, const std::string &help, const Labels &labels,
                           struct proto_op *source = nullptr);
  struct proto_op *Duration(const std::string &name, const std::string &help, const Labels &labels);
  void Histogram(const std::string &name, const std::string &help, const Labels &labels,
                 struct proto_op *source);
  std::vector<prometheus::MetricFamily> Collect() const override;

 private:
  struct View {
    std::string name;
    std::string help;
    Labels labels;
    bool histogram;
    std::vector<struct proto_op *> sources;
  };
  struct OwnedRecord {
    struct proto_op stats {};
    ~OwnedRecord() { observation_destroy(&stats); }
  };
  struct proto_op *Register(const std::string &name, const std::string &help, const Labels &labels,
                            bool histogram, struct proto_op *source);
  mutable std::mutex registration;
  std::map<std::pair<std::string, Labels>, View> views;
  std::vector<std::unique_ptr<OwnedRecord>> records;
};

extern std::shared_ptr<ObservationMetrics> observations;
}  // namespace ganesha_monitoring
#endif
