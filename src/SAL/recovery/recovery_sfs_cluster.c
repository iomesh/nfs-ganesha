#include "abstract_mem.h"
#include "config.h"
#include "log.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "bsd-base64.h"
#include "recovery_sfs.h"
#include "nfs_init.h"
#include <ctype.h>

#include "sfs_recovery_backend.h"
#include "sfs_client.h"

/* Use session_id in the SFS cluster */
uint32_t sessionid;

struct sfs_cluster_parameter sfs_cluster_param;

static struct config_item sfs_cluster_params[] = {
	CONF_ITEM_UI32("sessionid", 0, UINT32_MAX, 0,
			sfs_cluster_parameter, sessionid),
	CONFIG_EOL
};

static void *sfs_cluster_param_init(void *link_mem, void *self_struct)
{
	if (self_struct == NULL)
		return &sfs_cluster_param;
	else
		return NULL;
}

struct config_block sfs_cluster_param_blk = {
	.dbus_interface_name = "org.ganesha.nfsd.config.sfs_cluster",
	.blk_desc.name = "SFS_CLUSTER",
	.blk_desc.type = CONFIG_BLOCK,
	.blk_desc.u.blk.init = sfs_cluster_param_init,
	.blk_desc.u.blk.params = sfs_cluster_params,
	.blk_desc.u.blk.commit = noop_conf_commit
};

int sfs_load_config_from_parse(config_file_t parse_tree,
				 struct config_error_type *err_type)
{
	(void) load_config_from_parse(parse_tree,
				      &sfs_cluster_param_blk,
				      NULL,
				      true,
				      err_type);
	if (!config_error_is_harmless(err_type)) {
		LogCrit(COMPONENT_INIT,
			"Error while parsing SFS Cluster specific configuration");
		return -1;
	}

	return 0;
}

// Copied from convert_opaque_val in recovery_rados_kv.c
static int convert_opaque_val(struct display_buffer *dspbuf,
			      void *value,
			      int len,
			      int max)
{
	unsigned int i = 0;
	int b_left = display_start(dspbuf);
	int cpy = len;

	if (b_left <= 0)
		return 0;

	/* Check that the length is ok
	 * If the value is empty, display EMPTY value. */
	if (len <= 0 || len > max)
		return 0;

	/* If the value is NULL, display NULL value. */
	if (value == NULL)
		return 0;

	/* Determine if the value is entirely printable characters, */
	/* and it contains no slash character (reserved for filename) */
	for (i = 0; i < len; i++)
		if ((!isprint(((char *)value)[i])) ||
		    (((char *)value)[i] == '/'))
			break;

	if (i == len) {
		/* Entirely printable character, so we will just copy the
		 * characters into the buffer (to the extent there is room
		 * for them).
		 */
		b_left = display_len_cat(dspbuf, value, cpy);
	} else {
		b_left = display_opaque_bytes(dspbuf, value, cpy);
	}

	if (b_left <= 0)
		return 0;

	return b_left;
}


// Copied from rados_kv_create_val
static char *sfs_cluster_create_val(nfs_client_id_t *clientid, size_t *size)
{
	char *src = clientid->cid_client_record->cr_client_val;
	int src_len = clientid->cid_client_record->cr_client_val_len;
	const char *str_client_addr = "(unknown)";
	char cidstr[PATH_MAX] = { 0, }, *val;
	struct display_buffer dspbuf = {sizeof(cidstr), cidstr, cidstr};
	char cidstr_lenx[5];
	int total_len, cidstr_len, cidstr_lenx_len, str_client_addr_len;
	int ret;
	size_t lsize;

	/* get the caller's IP addr */
	if (clientid->gsh_client != NULL)
		str_client_addr = clientid->gsh_client->hostaddr_str;

	str_client_addr_len = strlen(str_client_addr);

	ret = convert_opaque_val(&dspbuf, src, src_len, sizeof(cidstr));
	assert(ret > 0);

	cidstr_len = display_buffer_len(&dspbuf);

	cidstr_lenx_len = snprintf(cidstr_lenx, sizeof(cidstr_lenx), "%d",
				   cidstr_len);

	if (unlikely(cidstr_lenx_len >= sizeof(cidstr_lenx) ||
		     cidstr_lenx_len < 0)) {
		/* cidrstr can at most be PATH_MAX or 1024, so at most
		 * 4 characters plus NUL are necessary, so we won't
		 * overrun, nor can we get a -1 with EOVERFLOW or EINVAL
		 */
		LogFatal(COMPONENT_CLIENTID,
			 "snprintf returned unexpected %d", cidstr_lenx_len);
	}

	struct in_addr s_addr;
	s_addr.s_addr = htonl(clientid->cid_client_record->cr_server_addr);
	char str_server_addr[INET_ADDRSTRLEN];
	const char *addr = inet_ntop(AF_INET, &s_addr, str_server_addr, INET_ADDRSTRLEN);
	if (unlikely(addr == NULL)) {
		LogFatal(COMPONENT_CLIENTID, "invalid server address: %u",
			 clientid->cid_client_record->cr_server_addr);
	}

	int str_server_addr_len = strlen(str_server_addr);

	lsize = str_client_addr_len + 2 + cidstr_lenx_len + 1 + cidstr_len + 3 + str_server_addr_len + 1;

	/* hold both long form clientid and IP */
	val = gsh_malloc(lsize);
	memcpy(val, str_client_addr, str_client_addr_len);
	total_len = str_client_addr_len;
	memcpy(val + total_len, "-(", 2);
	total_len += 2;
	memcpy(val + total_len, cidstr_lenx, cidstr_lenx_len);
	total_len += cidstr_lenx_len;
	val[total_len] = ':';
	total_len += 1;
	memcpy(val + total_len, cidstr, cidstr_len);
	total_len += cidstr_len;
	memcpy(val + total_len, ")=>", 3);
	total_len += 3;
	strncpy(val + total_len, str_server_addr, str_server_addr_len);
	val[lsize - 1] = '\0';

	LogDebug(COMPONENT_CLIENTID, "Created client name [%s]", val);

	if (size)
		*size = lsize;

	return val;
}

static int sfs_start_grace(const char *vip, int event) {
	assert(event == EVENT_RELEASE_IP || event == EVENT_TAKE_IP);

	nfs_grace_start_t gsp;
	gsp.event = event;
	gsp.ipaddr = (char *)vip;

	// 1. unbind vip
	uint32_t vip_u32 = ntohl(inet_addr(vip));
	if (event == EVENT_RELEASE_IP) {
		unbind_ipv4_address(vip_u32);
	}

	// 2. start grace
	int ret;
	do {
		ret = nfs_start_grace(&gsp);
		if (ret == -EAGAIN) {
			nfs_wait_for_grace_norefs();
		} else if (ret) {
			break;
		}
	} while (ret);

	// 3. bind vip
	if (event == EVENT_TAKE_IP) {
		bind_ipv4_address(vip_u32);
	}

	return -ret;
}

static void write_log(const char *file, int level, uint32_t line, const char *target, const char *message)
{
	//TODO dynamic loglevel
	DisplayLogComponentLevel(COMPONENT_RECOVERY_BACKEND, file, line, target, level, "%s", message);
}

static int sfs_cluster_recovery_init(void)
{
	if (sfs_cluster_param.sessionid == 0) {
		LogCrit(COMPONENT_INIT, "sessionid must be a non-zero value");
		return -1;
	}

	sessionid = sfs_cluster_param.sessionid;

	sfs_recovery_log_init(write_log);
	int ret = sfs_recovery_backend_init(sessionid, sfs_start_grace, bind_ipv4_address);

	LogEvent(COMPONENT_INIT,
		 "creating sfs_cluster recovery database, sessionid: %d", sessionid);

	return ret;
}

/* Try to delete old recovery db */
static void sfs_cluster_end_grace(void)
{
	// rust will panic if end grace failed
	sfs_recovery_end_grace();
}

static void sfs_cluster_read_clids(nfs_grace_start_t *gsp,
				add_clid_entry_hook add_clid_entry,
				add_rfh_entry_hook add_rfh_entry)
{
	if (gsp != NULL) {
		assert(gsp->event == EVENT_TAKE_IP);
		// rust will panic if take ip failed
		sfs_take_vip(gsp->ipaddr);
	}

	const char* recov_tag = NULL;
	while ((recov_tag = sfs_recovery_pop_clid_entry()) != NULL) {
		clid_entry_t* clid_entry;

		clid_entry = add_clid_entry((char *)recov_tag);

		if (clid_entry != NULL) {
			const char* rfh = NULL;

			while ((rfh = sfs_recovery_pop_rfh_entry(recov_tag)) != NULL) {
				add_rfh_entry(clid_entry, (char *)rfh);
				sfs_free_string(rfh);
			}
		}

		sfs_free_string(recov_tag);
	}
}

static void sfs_cluster_add_clid(nfs_client_id_t *clientid)
{
	char *recov_tag;
	int ret;

	// Serialized client identification.
	recov_tag = sfs_cluster_create_val(clientid, NULL);

	ret = sfs_recovery_add_clid(clientid->cid_clientid,
		clientid->cid_client_record->cr_server_addr, (const char*)recov_tag);

	if (ret < 0) {
		LogFatal(COMPONENT_CLIENTID, "Failed to add clid %lu",
			 clientid->cid_clientid);
		gsh_free(recov_tag);
	} else {
		clientid->cid_recov_tag = recov_tag;
		LogDebug(COMPONENT_CLIENTID, "cid_clientid: [%ld] cid_recov_tag: [%s]", clientid->cid_clientid, clientid->cid_recov_tag);
	}
}

static void sfs_cluster_rm_clid(nfs_client_id_t *clientid)
{
	// rust will panic if rm clid failed
	sfs_recovery_rm_clid(clientid->cid_clientid, clientid->cid_client_record->cr_server_addr);

	char *recov_tag = clientid->cid_recov_tag;
	clientid->cid_recov_tag = NULL;
	if (recov_tag != NULL)
		gsh_free((void *)recov_tag);
}

static void sfs_cluster_add_revoke_fh(nfs_client_id_t *delr_clid, nfs_fh4 *delr_handle)
{
	char rfhstr[NAME_MAX];
	int ret;

	ret = base64url_encode(delr_handle->nfs_fh4_val, delr_handle->nfs_fh4_len,
							rfhstr, sizeof(rfhstr));
	assert(ret != -1);

	// rust will panic if this function failed
	sfs_recovery_add_revoke_fh(delr_clid->cid_clientid, delr_clid->cid_client_record->cr_server_addr,
					(const char*)rfhstr);
}

struct nfs4_recovery_backend sfs_cluster_backend = {
	.recovery_init = sfs_cluster_recovery_init,
	.recovery_read_clids = sfs_cluster_read_clids,
	.end_grace = sfs_cluster_end_grace,
	.add_clid = sfs_cluster_add_clid,
	.rm_clid = sfs_cluster_rm_clid,
	.add_revoke_fh = sfs_cluster_add_revoke_fh,
};

void sfs_cluster_backend_init(struct nfs4_recovery_backend **backend)
{
	*backend = &sfs_cluster_backend;
}
