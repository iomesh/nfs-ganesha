/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef OBSERVATION_STATS_H
#define OBSERVATION_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define OBSERVATION_BUCKET_COUNT 36
#define OBSERVATION_SCHEMA_VERSION 1

/* Nanoseconds throughout storage. Exporters alone convert units. */
struct op_latency {
	uint64_t latency;
	uint64_t min;
	uint64_t max;
	uint64_t samples;
};

struct observation_snapshot {
	uint64_t total;
	uint64_t errors;
	uint64_t dups;
	uint64_t sampled_total;
	struct op_latency latency;
	struct op_latency dup_latency;
	uint64_t buckets[OBSERVATION_BUCKET_COUNT];
	uint64_t epoch;
};

/* Zero initialization is sufficient. Owners must outlive all readers. The
 * optional histogram/history are allocated only on registration/reset. */
struct proto_op {
	uint64_t total;
	uint64_t errors;
	uint64_t dups;
	struct op_latency latency;
	struct op_latency dup_latency;
	uint64_t sampled_total;
	unsigned char lock;
	uint64_t *buckets;
	struct observation_snapshot *history;
	uint64_t epoch;
};

#ifdef __cplusplus
extern "C" {
#endif
extern const uint64_t observation_bucket_ns[OBSERVATION_BUCKET_COUNT];
void observation_count(struct proto_op *op, bool success, bool dup,
		       bool sampled);
void observation_latency(struct proto_op *op, uint64_t ns, bool dup);
void observation_record(struct proto_op *op, uint64_t ns, bool success,
			bool dup, bool sampled);
void observation_enable_histogram(struct proto_op *op);
void observation_snapshot(struct proto_op *op, bool lifetime,
			  struct observation_snapshot *snapshot);
void observation_reset(struct proto_op *op);
void observation_destroy(struct proto_op *op);
void observation_now(struct timespec *timestamp);
#ifdef __cplusplus
}
#endif
#endif
