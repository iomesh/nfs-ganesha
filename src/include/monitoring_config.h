/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef GANESHA_MONITORING_CONFIG_H
#define GANESHA_MONITORING_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_PERF_MONITORING
/* Startup-only settings for the MONITORING block. */
struct monitoring_parameter {
	bool enabled;
	bool overall;
	bool stages;
	uint32_t sample_every;
};

struct config_block;
extern struct config_block monitoring_config;
extern struct monitoring_parameter monitoring_param;

#endif /* USE_PERF_MONITORING */

#ifdef __cplusplus
}
#endif

#endif /* GANESHA_MONITORING_CONFIG_H */
