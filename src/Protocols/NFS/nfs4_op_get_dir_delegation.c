// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 */

/**
 * @file    nfs4_op_get_dir_delegation.c
 * @brief   NFS4_OP_GET_DIR_DELEGATION support.
 */

#include "config.h"
#include "hashtable.h"
#include "log.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "nfs_proto_functions.h"

/**
 * @brief NFS4_OP_GET_DIR_DELEGATION
 *
 * Ganesha does not grant directory delegations. Return the operation's
 * non-fatal unavailable result instead of OP_ILLEGAL so later compound
 * operations, such as GETATTR, can still be processed.
 *
 * @param[in]     op   Arguments for nfs4_op
 * @param[in,out] data Compound request's data
 * @param[out]    resp Results for nfs4_op
 *
 * @return NFS_REQ_OK with GDD4_UNAVAIL.
 */
enum nfs_req_result nfs4_op_get_dir_delegation(struct nfs_argop4 *op,
					       compound_data_t *data,
					       struct nfs_resop4 *resp)
{
	GET_DIR_DELEGATION4args * const arg_GET_DIR_DELEGATION4
		__attribute__ ((unused))
		= &op->nfs_argop4_u.opget_dir_delegation;
	GET_DIR_DELEGATION4res * const res_GET_DIR_DELEGATION4 =
		&resp->nfs_resop4_u.opget_dir_delegation;
	GET_DIR_DELEGATION4res_non_fatal *res_non_fatal =
		&res_GET_DIR_DELEGATION4->GET_DIR_DELEGATION4res_u
			 .gddr_res_non_fatal4;

	resp->resop = NFS4_OP_GET_DIR_DELEGATION;
	res_GET_DIR_DELEGATION4->gddr_status = NFS4_OK;
	res_non_fatal->gddrnf_status = GDD4_UNAVAIL;
	res_non_fatal->GET_DIR_DELEGATION4res_non_fatal_u.gddrnf_signal =
		false;

	return NFS_REQ_OK;
}

/**
 * @brief Free memory allocated for GET_DIR_DELEGATION result
 *
 * @param[in,out] resp nfs4_op results
 */
void nfs4_op_get_dir_delegation_Free(nfs_resop4 *resp)
{
	/* Nothing to be done */
}
