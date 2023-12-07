// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * ---------------------------------------
 *
 * All the display functions and error handling.
 *
 */
#include "config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <libgen.h>
#include <sys/resource.h>
#include <execinfo.h>
#include <assert.h>

#include "log.h"
#include "gsh_list.h"
#include "gsh_rpc.h"
#include "common_utils.h"
#include "abstract_mem.h"

#ifdef USE_DBUS
#include "gsh_dbus.h"
#endif

#include "nfs_core.h"
#include "config_parsing.h"

#ifdef USE_LTTNG
#include "gsh_lttng/logger.h"
#endif

#include "minitrace.h"
#include "minitrace_init.h"

double minitrace_sample_ratio = 0.0;
char *otlp_endpoint = NULL;
bool minitrace_reinit = false;

struct minitrace_config {
	char *otlp_endpoint;
	char *minitrace_sample_ratio;
};

static struct config_item minitrace_params[] = {
	CONF_ITEM_STR("Otlp_Endpoint", 1, 100, NULL,
		       minitrace_config, otlp_endpoint),
	CONF_ITEM_STR("Minitrace_Sample_Ratio", 1, 10, "0.0",
		       minitrace_config, minitrace_sample_ratio),
	CONFIG_EOL
};

static void *minitrace_conf_init(void *link_mem, void *self_struct)
{
	assert(link_mem != NULL || self_struct != NULL);

	if (link_mem == NULL)
		return self_struct;
	else if (self_struct == NULL)
		return link_mem;
	return NULL;
}

#define LogChanges(format, args...) \
	DisplayLogComponentLevel(COMPONENT_LOG, \
				 __FILE__, \
				 __LINE__, \
				 __func__, \
				 NIV_NULL, \
				 "LOG: " format, \
				 ## args)

static int minitrace_conf_commit(void *node, void *link_mem, void *self_struct,
			   struct config_error_type *err_type)
{
	struct minitrace_config *conf = self_struct;

	if (conf->minitrace_sample_ratio) {
		minitrace_sample_ratio = atof(conf->minitrace_sample_ratio);
		if (minitrace_sample_ratio < 0.0 || minitrace_sample_ratio > 1.0)
			minitrace_sample_ratio = 0.0;
		LogChanges("Changed minitrace_sample_ratio to %f",
			   minitrace_sample_ratio);
		gsh_free(conf->minitrace_sample_ratio);
	}

	if (conf->otlp_endpoint) {
		if (otlp_endpoint) {
			gsh_free(otlp_endpoint);
		}
		otlp_endpoint = conf->otlp_endpoint;
		LogChanges("Changing otlp_endpoint to %s",
			   otlp_endpoint);
		// restart_minitrace();
		minitrace_reinit = true;
		LogChanges("Minitrace reinit");
	}

	return 0;
}

struct config_block minitrace_param = {
	.dbus_interface_name = "org.ganesha.nfsd.config.minitrace",
	.blk_desc.name = "MINITRACE",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.flags = CONFIG_UNIQUE,  /* too risky to have more */
	.blk_desc.u.blk.init = minitrace_conf_init,
	.blk_desc.u.blk.params = minitrace_params,
	.blk_desc.u.blk.commit = minitrace_conf_commit
};

int read_minitrace_config(config_file_t in_config,
		    struct config_error_type *err_type)
{
	struct minitrace_config conf;

	memset(&conf, 0, sizeof(struct minitrace_config));
	(void)load_config_from_parse(in_config,
				     &minitrace_param,
				     &conf,
				     true,
				     err_type);
	if (config_error_is_harmless(err_type))
		return 0;
	else
		return -1;
}