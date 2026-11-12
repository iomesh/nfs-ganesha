/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "observation_stats.h"
#include <stdlib.h>
#include <string.h>

const uint64_t observation_bucket_ns[OBSERVATION_BUCKET_COUNT] = {
    1000,	2500,	    5000,	10000,	    25000,	50000,
    100000,	150000,	    225000,	337000,	    506000,	759000,
    1130000,	1700000,    2560000,	3840000,    5760000,	8640000,
    12900000,	19400000,   29100000,	43700000,   65600000,	98500000,
    147000000,	221000000,  332000000,	498000000,  748000000,	1122000000,
    1683000000, 2525000000, 3787000000, 5681000000, 8522000000, 12783000000};

static void observation_lock(struct proto_op *op)
{
	while (__atomic_test_and_set(&op->lock, __ATOMIC_ACQUIRE)) {
		/* Only a selected duration update or snapshot holds this lock.
		 */
	}
}

static void observation_unlock(struct proto_op *op)
{
	__atomic_clear(&op->lock, __ATOMIC_RELEASE);
}

void observation_count(struct proto_op *op, bool success, bool dup,
		       bool sampled)
{
	__atomic_fetch_add(&op->total, 1, __ATOMIC_RELAXED);
	if (!success) {
		__atomic_fetch_add(&op->errors, 1, __ATOMIC_RELAXED);
	}
	if (dup) {
		__atomic_fetch_add(&op->dups, 1, __ATOMIC_RELAXED);
	}
	if (sampled) {
		__atomic_fetch_add(&op->sampled_total, 1, __ATOMIC_RELAXED);
	}
}

static void record_duration(struct proto_op *op, uint64_t ns, bool dup)
{
	struct op_latency *latency = dup ? &op->dup_latency : &op->latency;
	unsigned int bucket = 0;

	while (bucket < OBSERVATION_BUCKET_COUNT &&
	       ns > observation_bucket_ns[bucket]) {
		bucket++;
	}
	latency->latency += ns;
	latency->samples++;
	if (latency->min == 0 || ns < latency->min) {
		latency->min = ns;
	}
	if (ns > latency->max) {
		latency->max = ns;
	}
	if (op->buckets != NULL && bucket < OBSERVATION_BUCKET_COUNT) {
		op->buckets[bucket]++;
	}
}

void observation_latency(struct proto_op *op, uint64_t ns, bool dup)
{
	observation_lock(op);
	record_duration(op, ns, dup);
	observation_unlock(op);
}

void observation_record(struct proto_op *op, uint64_t ns, bool success,
			bool dup, bool sampled)
{
	if (!sampled) {
		observation_count(op, success, dup, false);
		return;
	}
	observation_lock(op);
	observation_count(op, success, dup, true);
	record_duration(op, ns, dup);
	observation_unlock(op);
}

void observation_enable_histogram(struct proto_op *op)
{
	observation_lock(op);
	if (op->buckets == NULL) {
		op->buckets =
		    calloc(OBSERVATION_BUCKET_COUNT, sizeof(uint64_t));
		if (op->buckets == NULL) {
			abort();
		}
	}
	observation_unlock(op);
}

static void snapshot_current(struct proto_op *op,
			     struct observation_snapshot *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->total = __atomic_load_n(&op->total, __ATOMIC_RELAXED);
	snapshot->errors = __atomic_load_n(&op->errors, __ATOMIC_RELAXED);
	snapshot->dups = __atomic_load_n(&op->dups, __ATOMIC_RELAXED);
	snapshot->sampled_total =
	    __atomic_load_n(&op->sampled_total, __ATOMIC_RELAXED);
	snapshot->latency = op->latency;
	snapshot->dup_latency = op->dup_latency;
	snapshot->epoch = op->epoch;
	if (op->buckets != NULL) {
		memcpy(snapshot->buckets, op->buckets,
		       sizeof(snapshot->buckets));
	}
}

static void merge_latency(struct op_latency *to, const struct op_latency *from)
{
	if (from->samples == 0) {
		return;
	}
	if (to->samples == 0 || from->min < to->min) {
		to->min = from->min;
	}
	if (from->max > to->max) {
		to->max = from->max;
	}
	to->latency += from->latency;
	to->samples += from->samples;
}

static void merge_snapshot(struct observation_snapshot *to,
			   const struct observation_snapshot *from)
{
	unsigned int bucket;

	to->total += from->total;
	to->errors += from->errors;
	to->dups += from->dups;
	to->sampled_total += from->sampled_total;
	merge_latency(&to->latency, &from->latency);
	merge_latency(&to->dup_latency, &from->dup_latency);
	for (bucket = 0; bucket < OBSERVATION_BUCKET_COUNT; bucket++) {
		to->buckets[bucket] += from->buckets[bucket];
	}
}

void observation_snapshot(struct proto_op *op, bool lifetime,
			  struct observation_snapshot *snapshot)
{
	observation_lock(op);
	snapshot_current(op, snapshot);
	if (lifetime && op->history != NULL) {
		merge_snapshot(snapshot, op->history);
	}
	observation_unlock(op);
}

void observation_reset(struct proto_op *op)
{
	struct observation_snapshot previous;

	observation_lock(op);
	snapshot_current(op, &previous);
	/* Counter exchanges preserve concurrent count-only updates. */
	previous.total = __atomic_exchange_n(&op->total, 0, __ATOMIC_RELAXED);
	previous.errors = __atomic_exchange_n(&op->errors, 0, __ATOMIC_RELAXED);
	previous.dups = __atomic_exchange_n(&op->dups, 0, __ATOMIC_RELAXED);
	previous.sampled_total =
	    __atomic_exchange_n(&op->sampled_total, 0, __ATOMIC_RELAXED);
	if (op->buckets != NULL && op->history == NULL) {
		op->history = calloc(1, sizeof(*op->history));
		if (op->history == NULL) {
			abort();
		}
	}
	if (op->history != NULL) {
		merge_snapshot(op->history, &previous);
	}
	memset(&op->latency, 0, sizeof(op->latency));
	memset(&op->dup_latency, 0, sizeof(op->dup_latency));
	if (op->buckets != NULL) {
		memset(op->buckets, 0, sizeof(previous.buckets));
	}
	op->epoch++;
	observation_unlock(op);
}

void observation_destroy(struct proto_op *op)
{
	/* The owner has already excluded writers and readers. */
	free(op->buckets);
	free(op->history);
}

void observation_now(struct timespec *timestamp)
{
	clock_gettime(CLOCK_MONOTONIC, timestamp);
}
