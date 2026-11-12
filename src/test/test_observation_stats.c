/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "observation_stats.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

struct writer {
	struct proto_op *statistics;
	uint64_t sum;
	uint64_t samples;
};

static void *record_requests(void *argument)
{
	struct writer *writer = argument;
	unsigned int index;

	for (index = 0; index < 1024; index++) {
		struct timespec started, completed;
		bool sampled = index % 4 == 0;
		uint64_t duration = 0;

		if (sampled) {
			observation_now(&started);
			observation_now(&completed);
			duration =
			    (uint64_t)(completed.tv_sec - started.tv_sec) *
				1000000000 +
			    completed.tv_nsec - started.tv_nsec;
			writer->sum += duration;
			writer->samples++;
		}
		observation_record(writer->statistics, duration, index % 8 != 0,
				   index % 16 == 0, sampled);
	}
	return NULL;
}

int main(void)
{
	struct proto_op statistics = {0};
	struct observation_snapshot active, lifetime;
	struct writer writers[4] = {0};
	pthread_t threads[4];
	uint64_t expected_sum = 0, expected_samples = 0;
	unsigned int index;

	observation_enable_histogram(&statistics);
	observation_snapshot(&statistics, false, &active);
	assert(active.total == 0 && active.latency.samples == 0);
	for (index = 0; index < 4; index++) {
		writers[index].statistics = &statistics;
		assert(pthread_create(&threads[index], NULL, record_requests,
				      &writers[index]) == 0);
	}
	for (index = 0; index < 4; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
		expected_sum += writers[index].sum;
		expected_samples += writers[index].samples;
	}
	observation_snapshot(&statistics, false, &active);
	assert(active.total == 4096 && active.errors == 512 &&
	       active.dups == 256);
	assert(active.sampled_total == expected_samples);
	assert(active.latency.samples + active.dup_latency.samples ==
	       expected_samples);
	assert(active.latency.latency + active.dup_latency.latency ==
	       expected_sum);
	observation_reset(&statistics);
	observation_snapshot(&statistics, false, &active);
	observation_snapshot(&statistics, true, &lifetime);
	assert(active.total == 0 && active.latency.samples == 0 &&
	       active.epoch == 1);
	assert(lifetime.total == 4096 &&
	       lifetime.sampled_total == expected_samples);
	assert(lifetime.latency.latency + lifetime.dup_latency.latency ==
	       expected_sum);
	/* An unselected completion contributes business counts, no duration. */
	observation_record(&statistics, 0, true, false, false);
	observation_snapshot(&statistics, false, &active);
	observation_snapshot(&statistics, true, &lifetime);
	assert(active.total == 1 && active.sampled_total == 0);
	assert(lifetime.total == 4097 &&
	       lifetime.sampled_total == expected_samples);
	observation_destroy(&statistics);
	puts("observation_stats: full counters, sampled durations, snapshots "
	     "and reset passed");
	return 0;
}
