/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2022
 * Author: Bjorn Leffler leffler@google.com
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <unistd.h>

#include <array>
#include <tuple>
#include <string_view>
#include <mutex>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>

#include "prometheus/counter.h"
#include "prometheus/exposer.h"
#include "prometheus/family.h"
#include "prometheus/histogram.h"
#include "prometheus/registry.h"

#include "monitoring.h"
#include "nfs_convert.h"
#include "log.h"
#include "client_mgr.h"
#include "export_mgr.h"

#include "monitoring_internal.h"

/*
 * This file contains the C++ monitoring implementation for Ganesha.
 */

// 24 size buckets: 2 bytes to 16 MB as powers of 2.
static const std::initializer_list<double> requestSizeBuckets =
{2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608, 16777216};

// 36 time buckets: 0.001 ms (1us) to 12 seconds. Sub-10us (0.001/0.0025/0.005) + sub-100us
// (0.01/0.025/0.05) low end for fine-grained latency analysis (e.g. ganesha over a RAM-backed
// FSAL, where per-op processing is single-digit us); the rest is 0.1 ms -> 12 s at 50% increases.
static const std::initializer_list<double> latencyBuckets =
{0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.15, 0.225, 0.337, 0.506, 0.759, 1.13, 1.70, 2.56, 3.84, 5.76, 8.64,
 12.9, 19.4, 29.1, 43.7, 65.6, 98.5, 147, 221, 332, 498, 748, 1122, 1683, 2525,
 3787, 5681, 8522, 12783};

static const char kClient[] = "client_ip";
static const char kServer[] = "server_ip";
static const char kExport[] = "export";  // export id in Export Block
static const char kExportName[] = "export_name"; // the path in Export Block
static const char kOperation[] = "operation";
static const char kStatus[] = "status";
static const char kVersion[] = "version";  // NFSv3, NFSv4..

namespace ganesha_monitoring {

class Metrics {
 public:
  Metrics(prometheus::Registry &registry);

  // Counters
  prometheus::Family<prometheus::Counter> &mdcacheCacheHitsTotal;
  prometheus::Family<prometheus::Counter> &mdcacheCacheMissesTotal;
  prometheus::Family<prometheus::Counter> &mdcacheCacheHitsByExportTotal;
  prometheus::Family<prometheus::Counter> &mdcacheCacheMissesByExportTotal;
  prometheus::Counter &attrsGetHitsTotal;
  prometheus::Counter &attrsGetMissMaskTotal;
  prometheus::Counter &rpcsReceivedTotal;
  prometheus::Counter &rpcsCompletedTotal;
  prometheus::Family<prometheus::Counter> &errorsByVersionOperationStatus;

  // Per client metrics.
  // Only track request and throughput rates to reduce memory overhead.
  // NFS request metrics below also generate latency percentiles, etc.
  prometheus::Family<prometheus::Counter> &clientRequestsTotal;
  prometheus::Family<prometheus::Counter> &clientBytesReceivedTotal;
  prometheus::Family<prometheus::Counter> &clientBytesSentTotal;

  // Gauges
  prometheus::Family<prometheus::Gauge> &rpcsInFlight;
  prometheus::Gauge &totalRpcsInFlight;
  prometheus::Family<prometheus::Gauge> &lastClientUpdate;

  // Per {operation} NFS request metrics.
  prometheus::Family<prometheus::Counter> &requestsTotalByOperation;
  prometheus::Family<prometheus::Counter> &bytesReceivedTotalByOperation;
  prometheus::Family<prometheus::Counter> &bytesSentTotalByOperation;
  prometheus::Family<prometheus::Histogram> &requestSizeByOperation;
  prometheus::Family<prometheus::Histogram> &responseSizeByOperation;
  prometheus::Family<prometheus::Histogram> &latencyByOperation;

  // Per {operation, export_id} NFS request metrics.
  prometheus::Family<prometheus::Counter> &requestsTotalByOperationExport;
  prometheus::Family<prometheus::Counter> &bytesReceivedTotalByOperationExport;
  prometheus::Family<prometheus::Counter> &bytesSentTotalByOperationExport;
  prometheus::Family<prometheus::Histogram> &requestSizeByOperationExport;
  prometheus::Family<prometheus::Histogram> &responseSizeByOperationExport;
  prometheus::Family<prometheus::Histogram> &latencyByOperationExport;
};

Metrics::Metrics(prometheus::Registry &registry) :
  // Counters
  mdcacheCacheHitsTotal(
      prometheus::BuildCounter()
      .Name("mdcache_cache_hits_total")
      .Help("Counter for total cache hits in mdcache.")
      .Register(registry)),
  mdcacheCacheMissesTotal(
      prometheus::BuildCounter()
      .Name("mdcache_cache_misses_total")
      .Help("Counter for total cache misses in mdcache.")
      .Register(registry)),
  mdcacheCacheHitsByExportTotal(
      prometheus::BuildCounter()
      .Name("mdcache_cache_hits_by_export_total")
      .Help("Counter for total cache hits in mdcache, by export.")
      .Register(registry)),
  mdcacheCacheMissesByExportTotal(
      prometheus::BuildCounter()
      .Name("mdcache_cache_misses_by_export_total")
      .Help("Counter for total cache misses in mdcache, by export.")
      .Register(registry)),
  attrsGetHitsTotal(
      prometheus::BuildCounter()
      .Name("attrs_get_hit_total")
      .Help("attrs_table get hits (entry present and mask sufficient).")
      .Register(registry)
      .Add({})),
  attrsGetMissMaskTotal(
      prometheus::BuildCounter()
      .Name("attrs_get_miss_mask_total")
      .Help("attrs_table get misses due to insufficient attr mask.")
      .Register(registry)
      .Add({})),
  rpcsReceivedTotal(
      prometheus::BuildCounter()
      .Name("rpcs_received_total")
      .Help("Counter for total RPCs received.")
      .Register(registry)
      .Add({})),
  rpcsCompletedTotal(
      prometheus::BuildCounter()
      .Name("rpcs_completed_total")
      .Help("Counter for total RPCs completed.")
      .Register(registry)
      .Add({})),
  errorsByVersionOperationStatus(
      prometheus::BuildCounter()
      .Name("nfs_errors_total")
      .Help("Error count by version, operation and status.")
      .Register(registry)),

  // Per client metrics.
  clientRequestsTotal(
      prometheus::BuildCounter()
      .Name("client_requests_total")
      .Help("Total requests by client.")
      .Register(registry)),
  clientBytesReceivedTotal(
      prometheus::BuildCounter()
      .Name("client_bytes_received_total")
      .Help("Total request bytes by client.")
      .Register(registry)),
  clientBytesSentTotal(
      prometheus::BuildCounter()
      .Name("client_bytes_sent_total")
      .Help("Total response bytes sent by client.")
      .Register(registry)),

  // Gauges
  rpcsInFlight(
      prometheus::BuildGauge()
      .Name("rpcs_in_flight")
      .Help("Number of NFS requests received or in flight.")
      .Register(registry)),
  totalRpcsInFlight(rpcsInFlight.Add({})),
  lastClientUpdate(
      prometheus::BuildGauge()
      .Name("last_client_update")
      .Help("Last update timestamp, per client.")
      .Register(registry)),

  // Per {operation} NFS request metrics.
  requestsTotalByOperation(
      prometheus::BuildCounter()
      .Name("nfs_requests_total")
      .Help("Total requests.")
      .Register(registry)),
  bytesReceivedTotalByOperation(
      prometheus::BuildCounter()
      .Name("nfs_bytes_received_total")
      .Help("Total request bytes.")
      .Register(registry)),
  bytesSentTotalByOperation(
      prometheus::BuildCounter()
      .Name("nfs_bytes_sent_total")
      .Help("Total response bytes.")
      .Register(registry)),
  requestSizeByOperation(
      prometheus::BuildHistogram()
      .Name("nfs_request_size_bytes")
      .Help("Request size in bytes.")
      .Register(registry)),
  responseSizeByOperation(
      prometheus::BuildHistogram()
      .Name("nfs_response_size_bytes")
      .Help("Response size in bytes.")
      .Register(registry)),
  latencyByOperation(
      prometheus::BuildHistogram()
      .Name("nfs_latency_ms")
      .Help("Request latency in ms.")
      .Register(registry)),

  // Per {operation, export_id} NFS request metrics.
  requestsTotalByOperationExport(
      prometheus::BuildCounter()
      .Name("nfs_requests_by_export_total")
      .Help("Total requests by export.")
      .Register(registry)),
  bytesReceivedTotalByOperationExport(
      prometheus::BuildCounter()
      .Name("nfs_bytes_received_by_export_total")
      .Help("Total request bytes by export.")
      .Register(registry)),
  bytesSentTotalByOperationExport(
      prometheus::BuildCounter()
      .Name("nfs_bytes_sent_by_export_total")
      .Help("Total response bytes by export.")
      .Register(registry)),
  requestSizeByOperationExport(
      prometheus::BuildHistogram()
      .Name("nfs_request_size_by_export_bytes")
      .Help("Request size by export in bytes.")
      .Register(registry)),
  responseSizeByOperationExport(
      prometheus::BuildHistogram()
      .Name("nfs_response_size_by_export_bytes")
      .Help("Response size by export in bytes.")
      .Register(registry)),
  latencyByOperationExport(
      prometheus::BuildHistogram()
      .Name("nfs_latency_ms_by_export")
      .Help("Request latency by export in ms.")
      .Register(registry)) {
}

static std::unique_ptr<Metrics> metrics;

template<class K, class T=std::string>
class SimpleMap {
    public:
        SimpleMap(std::function<T(const K& k)> get_value) : get_value_(get_value){ }

        T GetOrInsert(const K& k) {
            std::shared_lock rlock(mutex_);
            auto iter = map_.find(k);
            if (iter != map_.end()) {
                return iter->second;
            }
            rlock.unlock();
            std::unique_lock wlock(mutex_);
            iter = map_.find(k);
            if (iter != map_.end()) {
                return iter->second;
            }
            auto v = get_value_(k);
            map_.emplace(k, v);
            return v;
        }

        // @ret whether the key already exists
        bool InsertOrUpdate(const K& k, const T& v) {
          std::unique_lock wlock(mutex_);
          bool exist = map_.find(k) != map_.end();
          map_[k] = v;
          return exist;
        }

    private:
        std::function<T(const K& k)> get_value_;

        std::shared_mutex mutex_;
        std::map<K, T> map_;
};

static std::atomic<uint64_t> exportLabelGeneration{1};

static SimpleMap<export_id_t, std::string> exportLabels([](const export_id_t& export_id){
    std::ostringstream ss;
    ss << "export_id=" << export_id;
    return ss.str();
});

static std::string GetExportLabel(export_id_t export_id) {
  return exportLabels.GetOrInsert(export_id);
}

static SimpleMap<in_addr_t, std::string> ip2str([](const in_addr_t& ip_addr){
  struct in_addr s_addr;
  s_addr.s_addr = htonl(ip_addr);
  char addr_str[INET_ADDRSTRLEN];
  const char *ip_str = inet_ntop(AF_INET, &s_addr, addr_str, INET_ADDRSTRLEN);
  if (unlikely(ip_str == NULL))
    return std::string("<unknown>");
  return std::string(ip_str);
});

std::unique_ptr<prometheus::Exposer> exposer;
std::shared_ptr<prometheus::Registry> registry;

static const char *trimIPv6Prefix(const char *input) {
  static constexpr char prefix[] = "::ffff:";
  constexpr size_t prefixSize = sizeof(prefix) - 1;
  if (strncmp(input, prefix, prefixSize) == 0) {
    return input + prefixSize;
  }
  return input;
}

static void toLowerCase(std::string &s) {
  transform(s.begin(), s.end(), s.begin(), ::tolower);
}

// Protocol operation numbers are finite; clients do not consume these slots.
static constexpr size_t nfs3OperationSlots = NFSPROC3_COMMIT + 1;
static constexpr size_t requestOperationSlots = nfs3OperationSlots + NFS4_OP_LAST_ONE;

struct NfsOperationMetrics {
  std::string operation;
  prometheus::Counter *requests = nullptr;
  prometheus::Histogram *latency = nullptr;
  std::map<std::pair<std::string_view, std::string_view>, prometheus::Counter *> errors;
};

static NfsOperationMetrics &GetOperationMetrics(size_t slot, const char *operation) {
  static thread_local std::array<NfsOperationMetrics, requestOperationSlots> cache;
  auto &entry = cache[slot];
  if (entry.requests == nullptr) {
    entry.operation = operation;
    toLowerCase(entry.operation);
    entry.requests = &metrics->requestsTotalByOperation.Add({{kOperation, entry.operation}});
    entry.latency = &metrics->latencyByOperation.Add({{kOperation, entry.operation}}, latencyBuckets);
  }
  return entry;
}

using UpdateKey = std::tuple<in_addr_t, export_id_t, std::string, std::string>;
struct UpdateGauge {
  const UpdateKey *key;
  prometheus::Gauge *gauge;
};
struct ClientMetricHandles {
  explicit ClientMetricHandles(const char *address) : label(trimIPv6Prefix(address)) {
    for (auto &entry : requests) {
      entry.store(nullptr, std::memory_order_relaxed);
    }
  }
  const std::string label;
  std::array<std::atomic<prometheus::Counter *>, requestOperationSlots> requests;
  std::mutex registration;
  std::map<UpdateKey, std::unique_ptr<UpdateGauge>> updates;
  std::atomic<UpdateGauge *> lastUpdate{nullptr};
};

static ClientMetricHandles &GetClientMetrics(struct gsh_client *client) {
  if (client == nullptr) {
    static ClientMetricHandles unknown("");
    return unknown;
  }
  return *static_cast<ClientMetricHandles *>(client->monitoring_metrics);
}

static prometheus::Counter &ClientRequestCounter(ClientMetricHandles &client,
                                                 size_t slot,
                                                 const std::string &operation) {
  auto *counter = client.requests[slot].load(std::memory_order_acquire);
  if (counter == nullptr) {
    std::lock_guard<std::mutex> lock(client.registration);
    counter = client.requests[slot].load(std::memory_order_relaxed);
    if (counter == nullptr) {
      counter = &metrics->clientRequestsTotal.Add({{kClient, client.label},
                                                   {kOperation, operation}});
      client.requests[slot].store(counter, std::memory_order_release);
    }
  }
  return *counter;
}

static prometheus::Gauge &ClientUpdateGauge(ClientMetricHandles &client,
                                            in_addr_t server, export_id_t exportId,
                                            const char *path, const char *version) {
  auto *cached = client.lastUpdate.load(std::memory_order_acquire);
  if (cached != nullptr && std::get<0>(*cached->key) == server &&
      std::get<1>(*cached->key) == exportId && std::get<2>(*cached->key) == path &&
      std::get<3>(*cached->key) == version) {
    return *cached->gauge;
  }
  std::lock_guard<std::mutex> lock(client.registration);
  UpdateKey key{server, exportId, path, version};
  auto found = client.updates.find(key);
  if (found == client.updates.end()) {
    auto &gauge = metrics->lastClientUpdate.Add({
        {kClient, client.label}, {kServer, ip2str.GetOrInsert(server)},
        {kExport, std::to_string(exportId)}, {kExportName, path}, {kVersion, version}});
    found = client.updates.emplace(std::move(key), std::make_unique<UpdateGauge>()).first;
    found->second->key = &found->first;
    found->second->gauge = &gauge;
  }
  cached = found->second.get();
  client.lastUpdate.store(cached, std::memory_order_release);
  return *cached->gauge;
}

struct ExportOperationMetrics {
  prometheus::Counter *requests;
  prometheus::Histogram *latency;
};
struct ExportLabelMetrics {
  explicit ExportLabelMetrics(std::string value) : label(std::move(value)) {
    for (auto &entry : operations) {
      entry.store(nullptr, std::memory_order_relaxed);
    }
  }
  ~ExportLabelMetrics() {
    for (auto &entry : operations) {
      delete entry.load(std::memory_order_relaxed);
    }
  }
  const std::string label;
  std::array<std::atomic<ExportOperationMetrics *>, requestOperationSlots> operations;
};
struct ExportMetricHandles {
  std::mutex registration;
  std::map<std::string, std::unique_ptr<ExportLabelMetrics>> labels;
  std::atomic<ExportLabelMetrics *> current{nullptr};
  std::atomic<uint64_t> generation{0};
};

static ExportOperationMetrics &ExportRequestMetrics(ExportMetricHandles &exportMetrics,
                                                    export_id_t exportId,
                                                    size_t slot,
                                                    const std::string &operation) {
  uint64_t generation = exportLabelGeneration.load(std::memory_order_acquire);
  const uint64_t observed = exportMetrics.generation.load(std::memory_order_acquire);
  auto *label = exportMetrics.current.load(std::memory_order_acquire);
  if (label == nullptr || observed != generation) {
    std::lock_guard<std::mutex> lock(exportMetrics.registration);
    generation = exportLabelGeneration.load(std::memory_order_acquire);
    if (exportMetrics.current.load(std::memory_order_relaxed) == nullptr ||
        exportMetrics.generation.load(std::memory_order_relaxed) != generation) {
      const std::string name = GetExportLabel(exportId);
      auto &entry = exportMetrics.labels[name];
      if (!entry) {
        entry = std::make_unique<ExportLabelMetrics>(name);
      }
      exportMetrics.current.store(entry.get(), std::memory_order_release);
      exportMetrics.generation.store(generation, std::memory_order_release);
    }
    label = exportMetrics.current.load(std::memory_order_relaxed);
  }
  auto *entry = label->operations[slot].load(std::memory_order_acquire);
  if (entry == nullptr) {
    std::lock_guard<std::mutex> lock(exportMetrics.registration);
    entry = label->operations[slot].load(std::memory_order_relaxed);
    if (entry == nullptr) {
      auto candidate = std::make_unique<ExportOperationMetrics>();
      candidate->requests = &metrics->requestsTotalByOperationExport.Add(
          {{kOperation, operation}, {kExport, label->label}});
      candidate->latency = &metrics->latencyByOperationExport.Add(
          {{kOperation, operation}, {kExport, label->label}}, latencyBuckets);
      entry = candidate.release();
      label->operations[slot].store(entry, std::memory_order_release);
    }
  }
  return *entry;
}

static void observeNfsRequest(size_t slot, const char *operation,
                              const nsecs_elapsed_t request_time,
                              const char *version, const char *statusLabel,
                              export_id_t export_id, const char *fullpath,
                              in_addr_t service_ip, struct gsh_client *client,
                              struct gsh_export *exp) {
  const double latency_ms = static_cast<double>(request_time) / NS_PER_MSEC;
  auto &operationMetrics = GetOperationMetrics(slot, operation);
  auto &clientMetrics = GetClientMetrics(client);
  ClientRequestCounter(clientMetrics, slot, operationMetrics.operation).Increment();
  // The IPv4 formatter always returns nonempty text (including its fallback).
  // Resolve the address only when a new last-update label set is registered.
  if (export_id != 0 && fullpath != nullptr && strlen(fullpath) > 1) {
    const int64_t epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ClientUpdateGauge(clientMetrics, service_ip, export_id, fullpath + 1, version).Set(epoch);
  }
  auto key = std::make_pair(std::string_view(version), std::string_view(statusLabel));
  auto found = operationMetrics.errors.find(key);
  if (found == operationMetrics.errors.end()) {
    auto &counter = metrics->errorsByVersionOperationStatus.Add(
        {{kVersion, version}, {kOperation, operationMetrics.operation}, {kStatus, statusLabel}});
    found = operationMetrics.errors.emplace(key, &counter).first;
  }
  found->second->Increment();
  operationMetrics.requests->Increment();
  operationMetrics.latency->Observe(latency_ms);
  if (export_id == 0) {
    return;
  }
  if (exp != nullptr && exp->export_id == export_id) {
    auto &exportMetrics = *static_cast<ExportMetricHandles *>(exp->monitoring_metrics);
    auto &handles = ExportRequestMetrics(exportMetrics, export_id, slot, operationMetrics.operation);
    handles.requests->Increment();
    handles.latency->Observe(latency_ms);
  } else {
    const std::string label = GetExportLabel(export_id);
    metrics->requestsTotalByOperationExport.Add(
        {{kOperation, operationMetrics.operation}, {kExport, label}}).Increment();
    metrics->latencyByOperationExport.Add(
        {{kOperation, operationMetrics.operation}, {kExport, label}}, latencyBuckets).Observe(latency_ms);
  }
}

/*
 * Functions used from NFS Ganesha below.
 */

extern "C" {

/* Construct the cache before the owner becomes visible to requests.
 * No Registry access is needed until the first metric update. */
void monitoring_client_metrics_init(struct gsh_client *client) {
  client->monitoring_metrics = new ClientMetricHandles(client->hostaddr_str);
}

void monitoring_export_metrics_init(struct gsh_export *exp) {
  exp->monitoring_metrics = new ExportMetricHandles;
}

/* Object managers call these only after the final request reference is gone.
 * Delete the handle owner, not the Registry-owned metric series. */
void monitoring_client_metrics_release(struct gsh_client *client) {
  delete static_cast<ClientMetricHandles *>(client->monitoring_metrics);
  client->monitoring_metrics = nullptr;
}

void monitoring_export_metrics_release(struct gsh_export *exp) {
  delete static_cast<ExportMetricHandles *>(exp->monitoring_metrics);
  exp->monitoring_metrics = nullptr;
}

void monitoring_register_export_label(const export_id_t export_id,
                                      const char *label) {
  exportLabels.InsertOrUpdate(export_id, std::string(label));
  exportLabelGeneration.fetch_add(1, std::memory_order_release);
}

void monitoring_init(const uint16_t port) {
  static bool initialised;
  if (initialised)
    return;
  std::ostringstream ss;
  ss << "0.0.0.0:" << port;
  std::string hostPort = ss.str();
  LogEvent(COMPONENT_INIT, "Init monitoring at %s", hostPort.c_str());
  exposer.reset(new prometheus::Exposer(hostPort));
  registry = std::make_shared<prometheus::Registry>();
  exposer->RegisterCollectable(registry);
  metrics.reset(new Metrics(*registry));
  initialised = true;
}

void monitoring_nfs3_request(const uint32_t proc,
                             const nsecs_elapsed_t request_time,
                             const enum nfs_req_result result,
                             const nfsstat3 status,
                             const export_id_t export_id,
                             const char *fullpath,
                             const in_addr_t service_ip,
                             struct gsh_client *client,
                             struct gsh_export *exp) {
  const char *version = "NFSv3";
  const char *operation = nfsproc3_to_str(proc);
  const char *statusLabel = result == NFS_REQ_OK ?
    nfsstat3_to_str(status) :
    nfs_req_result_to_str(result);

  const size_t slot = proc <= NFSPROC3_COMMIT ? proc : 0;
  observeNfsRequest(slot, operation, request_time, version, statusLabel, export_id,
          fullpath, service_ip, client, exp);
}

static const char *nfsversion4_to_str(uint32_t minorversion) {
  switch (minorversion) {
    case 0:
      return "NFSv4";
    case 1:
      return "NFSv4.1";
    case 2:
      return "NFSv4.2";
    default:
      return "NFSv4.3+";
  }
}

void monitoring_nfs4_request(const uint32_t op,
                             const uint32_t minor_version,
                             const nsecs_elapsed_t request_time,
                             const nfsstat4 status,
                             const export_id_t export_id,
                             const char *fullpath,
                             const in_addr_t service_ip,
                             struct gsh_client *client,
                             struct gsh_export *exp) {
  const char *operation = nfsop4_to_str(op);
  const char *version = nfsversion4_to_str(minor_version);
  const char *statusLabel = nfsstat4_to_str(status);
  const size_t slot = nfs3OperationSlots + (op < NFS4_OP_LAST_ONE ? op : 0);
  observeNfsRequest(slot, operation, request_time, version, statusLabel, export_id,
                    fullpath, service_ip, client, exp);
}

void monitoring_nfs_io(const size_t bytes_requested,
                       const size_t bytes_transferred,
                       const bool success,
                       const bool is_write,
                       const export_id_t export_id,
                       const char *client_ip) {
  const std::string operation(is_write ? "write" : "read");
  const size_t bytes_received = (is_write ? 0 : bytes_transferred);
  const size_t bytes_sent = (is_write ? bytes_transferred : 0);
  if (client_ip != NULL) {
    const char *client = trimIPv6Prefix(client_ip);
    metrics->clientBytesReceivedTotal
        .Add({{kClient, client},
              {kOperation, operation}})
        .Increment(bytes_received);
    metrics->clientBytesSentTotal
        .Add({{kClient, client},
              {kOperation, operation}})
        .Increment(bytes_sent);
  }

  // Observe metrics.
  metrics->bytesReceivedTotalByOperation
    .Add({{kOperation, operation}})
    .Increment(bytes_received);
  metrics->bytesSentTotalByOperation
    .Add({{kOperation, operation}})
    .Increment(bytes_sent);
  metrics->requestSizeByOperation
    .Add({{kOperation, operation}}, requestSizeBuckets)
    .Observe(bytes_requested);
  metrics->responseSizeByOperation
    .Add({{kOperation, operation}}, requestSizeBuckets)
    .Observe(bytes_sent);

  // Ignore export id 0. It's never used for actual exports, but can happen
  // during the setup phase, or when the export id is unknown.
  if (export_id == 0) return;

  // Observe by export metrics.
  const std::string exportLabel = GetExportLabel(export_id);
  metrics->bytesReceivedTotalByOperationExport
    .Add({{kOperation, operation}, {kExport, exportLabel}})
    .Increment(bytes_received);
  metrics->bytesSentTotalByOperationExport
    .Add({{kOperation, operation}, {kExport, exportLabel}})
    .Increment(bytes_sent);
  metrics->requestSizeByOperationExport
    .Add({{kOperation, operation}, {kExport, exportLabel}},
         requestSizeBuckets)
    .Observe(bytes_requested);
  metrics->responseSizeByOperationExport
    .Add({{kOperation, operation}, {kExport, exportLabel}},
         requestSizeBuckets)
    .Observe(bytes_sent);
}

void monitoring_mdcache_cache_hit(const char *operation,
                                  const export_id_t export_id) {
  metrics->mdcacheCacheHitsTotal.Add({{kOperation, operation}}).Increment();
  if (export_id != 0) {
    const std::string exportLabel = GetExportLabel(export_id);
    metrics->mdcacheCacheHitsByExportTotal
            .Add({{kExport, exportLabel},
                  {kOperation, operation}})
        .Increment();
  }
}

void monitoring_mdcache_cache_miss(const char *operation,
                                   const export_id_t export_id) {
  metrics->mdcacheCacheMissesTotal
      .Add({{kOperation, operation}})
      .Increment();
  if (export_id != 0) {
    metrics->mdcacheCacheMissesByExportTotal
        .Add({{kExport, GetExportLabel(export_id)},
              {kOperation, operation}})
        .Increment();
  }
}

void monitoring_attrs_get_hit(void) {
  if (metrics == nullptr) {
    return;
  }
  metrics->attrsGetHitsTotal.Increment();
}

void monitoring_attrs_get_miss_mask(void) {
  if (metrics == nullptr) {
    return;
  }
  metrics->attrsGetMissMaskTotal.Increment();
}

void monitoring_rpc_received() {
  metrics->rpcsReceivedTotal.Increment();
}

void monitoring_rpc_completed() {
  metrics->rpcsCompletedTotal.Increment();
}

void monitoring_rpcs_in_flight(const uint64_t value) {
  metrics->totalRpcsInFlight.Set(value);
}

void monitoring_service_ip_rpcs_in_flight(const in_addr_t service_ipaddr,
					  const uint64_t value) {
  // The registry owns stable series for the process lifetime. Retain only the
  // last address per thread; alternating addresses still resolve their own
  // labels, without growing a second address registry.
  static thread_local in_addr_t cachedAddress = 0;
  static thread_local prometheus::Gauge *gauge = nullptr;
  if (gauge == nullptr || cachedAddress != service_ipaddr) {
    std::string service_ip = ip2str.GetOrInsert(service_ipaddr);
    gauge = &metrics->rpcsInFlight.Add({{kServer, service_ip}});
    cachedAddress = service_ipaddr;
  }
  gauge->Set(value);
}

}  // extern "C"

}  // namespace ganesha_monitoring
