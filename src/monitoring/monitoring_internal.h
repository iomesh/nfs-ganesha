/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Google Inc., 2022
 * Author: Bjorn Leffler leffler@google.com
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * @brief Internal C++ monitoring header for NFS Ganesha.
 */

#ifndef GANESHA_MONITORING_INTERNAL_H
#define GANESHA_MONITORING_INTERNAL_H

#ifdef __cplusplus

extern "C" {
#include "monitoring.h"  /* NOLINT */
}

#include <memory>
#include <string>

#include "prometheus/exposer.h"
#include "prometheus/registry.h"

namespace ganesha_monitoring {

extern std::unique_ptr < prometheus::Exposer > exposer;
extern std::shared_ptr < prometheus::Registry > registry;

#ifdef ENABLE_FAULT_INJECTION
/*
 * Test builds only: start our own civetweb on @port serving BOTH /metrics
 * (serialised from @registry) and /debug/failpoints (see fault_control.cc).
 * Called from monitoring_init() in place of constructing a prometheus::Exposer.
 */
void fault_control_start(uint16_t port,
			 std::shared_ptr < prometheus::Registry > registry);
#endif  /* ENABLE_FAULT_INJECTION */

}  /* namespace ganesha_monitoring */

#endif  /* __cplusplus */

#endif  /* GANESHA_MONITORING_INTERNAL_H */
