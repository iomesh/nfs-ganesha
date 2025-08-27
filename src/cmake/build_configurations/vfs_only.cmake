# SPDX-License-Identifier: BSD-3-Clause
#-------------------------------------------------------------------------------
#
# Copyright Panasas, 2012
# Contributor: Jim Lieb <jlieb@panasas.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#
#-------------------------------------------------------------------------------
# Only build VFS fsal and other useful options

set(USE_FSAL_VFS ON)
set(_MSPAC_SUPPORT OFF)
set(USE_9P OFF)
set(USE_DBUS OFF)

set(USE_NLM OFF)
set(USE_RQUOTA OFF)
set(USE_NFSACL3 OFF)
set(ENABLE_VFS_POSIX_ACL OFF)

message(STATUS "Building vfs_only configuration")
