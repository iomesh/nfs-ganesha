/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) SmartX, INC. 2025
 * Author: Zhitao Li zhitao.li@smartx.com
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
 * -------------
 */

/**
 * @defgroup service ip statistics
 */

/**
 * @author Zhitao Li <zhitao.li@smartx.com>
 * @brief Tracks statistics for service IPs, including inflight requests.
 * @details
 * Statistics by service IP are useful for both monitoring and handling service
 * IP migration in failure scenarios.
 *
 * To prevent data corruption caused by retried requests, all inflight requests
 * originating from the source node of a service IP MUST either complete or be
 * terminated before the IP is reassigned to another node.
 */

#ifndef SERVICE_IP_MGR_H
#define SERVICE_IP_MGR_H

#include <pthread.h>
#include <sys/types.h>

#include "avltree.h"
#include "gsh_types.h"

struct service_ip_stats {
	struct avltree_node node_k;
	int64_t inflight_count;
	in_addr_t ipv4addr;
};

void inc_gsh_service_ip_inflight_count(sockaddr_t *service_ipaddr);
void dec_gsh_service_ip_inflight_count(sockaddr_t *service_ipaddr);

int64_t get_gsh_service_ip_inflight_count(in_addr_t service_ipv4addr);

void service_ip_pkginit(void);

#endif /* !SERVICE_IP_MGR_H */
/** @} */
