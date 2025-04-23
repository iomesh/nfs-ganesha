// SPDX-License-Identifier: LGPL-3.0-or-later
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

#include "config.h"

#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#ifdef RPC_VSOCK
#include <linux/vm_sockets.h>
#endif /* VSOCK */
#include <sys/types.h>
#include <sys/param.h>
#include <pthread.h>
#include <assert.h>
#include <arpa/inet.h>
#include <fnmatch.h>
#include "gsh_list.h"
#include "fsal.h"
#include "log.h"
#include "avltree.h"
#include "gsh_types.h"
#include "service_ip_mgr.h"
#include "abstract_atomic.h"
#include "gsh_intrinsic.h"

#include "monitoring.h"

struct service_stats_by_ip {
	struct avltree t;
	pthread_rwlock_t sip_lock;
	/*
	 * Fast fore-end table by hash of ipv4addr.
	 * Collisions are handled by avl tree, which almost never happens.
	 */
	struct avltree_node **cache;
	uint32_t cache_sz;
};

static struct service_stats_by_ip service_stats_by_ip;

/**
 * @brief Compute cache slot for an entry
 *
 * This function computes a hash slot, taking an address modulo the
 * number of cache slots (which should be prime).
 *
 * @param wt [in] The table
 * @param ptr [in] Entry address
 *
 * @return The computed offset.
 */
static inline int eip_cache_offsetof(
	struct service_stats_by_ip *eid, uint64_t k)
{
	return k % eid->cache_sz;
}

/**
 * @brief IP address comparator for AVL tree walk
 * @return 0 if they are equal; -1 if lhs is less; 1 if lhs is greater.
 */

static int service_ip_cmpf(const struct avltree_node *lhs,
			   const struct avltree_node *rhs)
{
	struct service_ip_stats *lk, *rk;

	lk = avltree_container_of(lhs, struct service_ip_stats, node_k);
	rk = avltree_container_of(rhs, struct service_ip_stats, node_k);
	if (lk->ipv4addr == rk->ipv4addr)
		return 0;
	else if (lk->ipv4addr < rk->ipv4addr)
		return -1;
	else
		return 1;
}

void inc_gsh_service_ip_inflight_count(sockaddr_t *service_ipv4addr)
{
	struct avltree_node *node = NULL;
	struct service_ip_stats *stats;
	struct service_ip_stats v;
	void **cache_slot;
	int64_t inflight_count;
	in_addr_t ipv4addr = get_ip_addr(service_ipv4addr);
	uint64_t hash = ipv4addr;

	if (ipv4addr == 0) {
		LogDebug(COMPONENT_DISPATCH,
			"failed to parse ipv4addr, invalid ss_family %u or not IPv4-mapped IPv6",
			service_ipv4addr->ss_family);
		return;
	}

	PTHREAD_RWLOCK_rdlock(&service_stats_by_ip.sip_lock);
	/* check cache */
	cache_slot = (void **)&(service_stats_by_ip.cache[
		eip_cache_offsetof(&service_stats_by_ip, hash)]);
	node = (struct avltree_node *)atomic_fetch_voidptr(cache_slot);
	if (node) {
		v.ipv4addr = ipv4addr;
		if (service_ip_cmpf(&v.node_k, node) == 0) {
			LogDebug(COMPONENT_HASHTABLE_CACHE,
				"service_ip_mgr cache hit slot %d",
				eip_cache_offsetof(&service_stats_by_ip, hash));
			stats = avltree_container_of(node,
				struct service_ip_stats, node_k);
			goto out;
		}
	}

	/* fall back to AVL */
	node = avltree_lookup(&v.node_k, &service_stats_by_ip.t);
	if (node) {
		stats = avltree_container_of(node,
			struct service_ip_stats, node_k);
		/* update cache */
		atomic_store_voidptr(cache_slot, node);
		goto out;
	}
	PTHREAD_RWLOCK_unlock(&service_stats_by_ip.sip_lock);

	stats = gsh_calloc(1, sizeof(*stats));
	stats->ipv4addr = ipv4addr;

	PTHREAD_RWLOCK_wrlock(&service_stats_by_ip.sip_lock);
	node = avltree_insert(&stats->node_k, &service_stats_by_ip.t);
	if (node) {
		gsh_free(stats); /* somebody beat us to it */
		stats = avltree_container_of(node,
			struct service_ip_stats, node_k);
	} else {
		char hostaddr_str[SOCK_NAME_MAX];

		if (!sprint_sockip(service_ipv4addr, hostaddr_str,
						   sizeof(hostaddr_str))) {
			(void)strlcpy(hostaddr_str, "<unknown>",
						  sizeof(hostaddr_str));
		}
		LogInfo(COMPONENT_HASHTABLE,
			"service_ip_mgr add service_ip %s to slot %d",
			hostaddr_str,
			eip_cache_offsetof(&service_stats_by_ip, hash));
		/* update cache */
		atomic_store_voidptr(cache_slot, &stats->node_k);
	}

out:
	PTHREAD_RWLOCK_unlock(&service_stats_by_ip.sip_lock);

	inflight_count = atomic_inc_int64_t(&stats->inflight_count);
#ifdef USE_MONITORING
	monitoring_service_ip_rpcs_in_flight(ipv4addr, inflight_count);
#endif
}

static struct service_ip_stats *
lookup_gsh_service_ip_stats(in_addr_t ipv4addr)
{
	struct avltree_node *node = NULL;
	struct service_ip_stats *stats = NULL;
	struct service_ip_stats v;
	void **cache_slot;
	uint64_t hash = ipv4addr;

	PTHREAD_RWLOCK_rdlock(&service_stats_by_ip.sip_lock);

	/* check cache */
	cache_slot = (void **)&(service_stats_by_ip.cache[
		eip_cache_offsetof(&service_stats_by_ip, hash)]);
	node = (struct avltree_node *)atomic_fetch_voidptr(cache_slot);
	if (node) {
		v.ipv4addr = ipv4addr;
		if (service_ip_cmpf(&v.node_k, node) == 0) {
			LogDebug(COMPONENT_HASHTABLE_CACHE,
				"service_ip_mgr cache hit slot %d",
				eip_cache_offsetof(&service_stats_by_ip, hash));
			stats = avltree_container_of(node,
				struct service_ip_stats, node_k);
			goto out;
		}
	}

	/* fall back to AVL */
	node = avltree_lookup(&v.node_k, &service_stats_by_ip.t);
	if (node) {
		stats = avltree_container_of(node,
			struct service_ip_stats, node_k);
		/* update cache */
		atomic_store_voidptr(cache_slot, node);
		goto out;
	}

out:
	PTHREAD_RWLOCK_unlock(&service_stats_by_ip.sip_lock);

	return stats;
}

void dec_gsh_service_ip_inflight_count(sockaddr_t *service_ipv4addr)
{
	int64_t inflight_count;
	struct service_ip_stats *stats;
	in_addr_t ipv4addr = get_ip_addr(service_ipv4addr);

	if (ipv4addr == 0) {
		LogDebug(COMPONENT_DISPATCH,
			"failed to parse ipv4addr, invalid ss_family %u or not IPv4-mapped IPv6",
			service_ipv4addr->ss_family);
		return;
	}

	stats = lookup_gsh_service_ip_stats(ipv4addr);
	if (!stats) {
		char hostaddr_str[SOCK_NAME_MAX];

		if (!sprint_sockip(service_ipv4addr, hostaddr_str,
						   sizeof(hostaddr_str))) {
			(void)strlcpy(hostaddr_str, "<unknown>",
						  sizeof(hostaddr_str));
		}
		LogFatal(COMPONENT_HASHTABLE,
				 "service_ip_mgr miss service_ip %s",
				 hostaddr_str);
		return;  /* It never comes here */
	}

	inflight_count = atomic_dec_int64_t(&stats->inflight_count);
#ifdef USE_MONITORING
	monitoring_service_ip_rpcs_in_flight(ipv4addr, inflight_count);
#endif
}

/**
 * @brief get the count of inflight requests with given service ip
 *
 * @return the inflight count. Negative value denotes not found stats.
 */
int64_t get_gsh_service_ip_inflight_count(in_addr_t service_ipv4addr)
{
	struct service_ip_stats *stats =
		lookup_gsh_service_ip_stats(service_ipv4addr);

	if (stats)
		return atomic_fetch_int64_t(&stats->inflight_count);
	else
		return -1;
}

/* Cleanup on shutdown */
void serivce_ip_mgr_cleanup(void)
{
	PTHREAD_RWLOCK_destroy(&service_stats_by_ip.sip_lock);
}

struct cleanup_list_element service_ip_mgr_cleanup_element = {
	.clean = serivce_ip_mgr_cleanup,
};

/**
 * @brief Initialize service_ip manager
 */

void service_ip_pkginit(void)
{
	PTHREAD_RWLOCK_init(&service_stats_by_ip.sip_lock, NULL);
	avltree_init(&service_stats_by_ip.t, service_ip_cmpf, 0);
	service_stats_by_ip.cache_sz = 1023;
	service_stats_by_ip.cache =
		gsh_calloc(service_stats_by_ip.cache_sz,
			sizeof(struct avltree_node *));
	RegisterCleanup(&service_ip_mgr_cleanup_element);
}

/** @} */
