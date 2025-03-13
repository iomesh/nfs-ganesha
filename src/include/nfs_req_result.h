/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright (C) 2025 SmartX, LLC.
 * Author: Zhitao Li <zhitao.li@iomesh.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * @file   nfs_req_result.h
 * @author Zhitao Li <zhitao.li@iomesh.com>
 *
 * @brief simple nfs_req_result enum declaration and definition
 *
 * @note This enum is separate stored because forward declaration of enum
 * type is nonstandard.
 */

#ifndef NFS_REQ_REESULT_H
#define NFS_REQ_REESULT_H

enum nfs_req_result {
	NFS_REQ_OK,
	NFS_REQ_DROP,
	NFS_REQ_ERROR,
	NFS_REQ_REPLAY,
	NFS_REQ_ASYNC_WAIT,
	NFS_REQ_XPRT_DIED,
	NFS_REQ_AUTH_ERR,
};

#endif /* NFS_REQ_REESULT_H */

