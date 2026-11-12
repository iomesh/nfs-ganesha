/* SPDX-License-Identifier: LGPL-3.0-or-later */
#include "config.h"
#ifdef USE_PERF_MONITORING
#include <stddef.h>
#include "config_parsing.h"
#include "monitoring_config.h"

struct monitoring_parameter monitoring_param;

static struct config_item monitoring_params[] = {
	CONF_ITEM_BOOL("Enabled", true, monitoring_parameter, enabled),
	CONF_ITEM_BOOL("Overall", true, monitoring_parameter, overall),
	CONF_ITEM_BOOL("Stages", false, monitoring_parameter, stages),
	CONF_ITEM_UI32("Sample_Every", 1, UINT32_MAX, 1,
		       monitoring_parameter, sample_every),
	CONFIG_EOL
};

struct config_block monitoring_config = {
	.dbus_interface_name = "org.ganesha.nfsd.config.monitoring",
	.blk_desc.name = "MONITORING",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.flags = CONFIG_UNIQUE,
	.blk_desc.u.blk.init = noop_conf_init,
	.blk_desc.u.blk.params = monitoring_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

#endif /* USE_PERF_MONITORING */
