/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Copyright SmartX, 2026
 *  Contributor: Storage Team
 *
 * --------------------------
 *
 * Native, compile-gated fault-injection facility for nfs-ganesha.
 *
 * A small name -> action fail-point registry with an HTTP control plane
 * (GET/PUT/POST/DELETE /debug/failpoints[/<name>]) that mirrors the gofail /
 * fail-rs cross-stack pattern used elsewhere in the deployment.  The route
 * name matches the Rust status_server so a single orchestrator FaultClient
 * needs no per-seam special-casing.
 *
 * The developer-facing macros FAIL_POINT()/FAIL_POINT_RET() mark seams in the
 * request path.  When ENABLE_FAULT_INJECTION is NOT defined (the default /
 * production build) the macros expand to nothing -- zero text, zero runtime
 * cost -- so the feature is entirely absent from a release binary.
 *
 * The compile gate is wired like USE_MONITORING / ENABLE_SFS:
 *   CMake option ENABLE_FAULT_INJECTION -> config-h.in.cmake
 *   #cmakedefine ENABLE_FAULT_INJECTION 1 -> config.h.
 * (This mirrors, but corrects, ganesha's dead err_inject stub, whose CMake
 *  guard "if(ERROR_INJECTION)" never matched the option ENABLE_ERROR_INJECTION
 *  and whose _ERROR_INJECTION macro was never emitted into config.h.)
 *
 * This header declares only the pure-C registry API; it is safe to include
 * from C and C++ (declarations carry C linkage).  The civetweb HTTP server
 * that drives this registry lives in the C++ monitoring module
 * (monitoring/fault_control.cc), because it also serves /metrics.
 */

#ifndef FAIL_INJECT_H
#define FAIL_INJECT_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef ENABLE_FAULT_INJECTION

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum length of an action body accepted by PUT /debug/failpoints/<name>. */
#define FAULT_CONTROL_BODY_MAX 256

/*
 * ---------------------------------------------------------------------------
 * Registry lifecycle.  fault_registry_init() is idempotent and is called by
 * the control server on startup (fault_control_start); it is exposed here so
 * in-process unit tests can arm points without the HTTP front end.
 * ---------------------------------------------------------------------------
 */
void fault_registry_init(void);
void fault_registry_destroy(void);

/*
 * ---------------------------------------------------------------------------
 * Programmatic arm / clear / list (also driven by the HTTP handler).
 * ---------------------------------------------------------------------------
 */

/*
 * Arm (or re-arm) the fail point @name with the action @action_spec.  The DSL
 * borrows fail-rs semantics:
 *
 *     [<prob>%][<count>*]<task>
 *
 *   task  := off | return(<errno>) | sleep(<ms>) | panic
 *   prob  := integer 0..100      (probability the action fires, default 100)
 *   count := positive integer    (fire at most this many times, default inf)
 *
 * Examples:  "return(5)"  "20%return(5)"  "3*sleep(100)"  "50%3*panic"  "off"
 *
 * @name must be one of the seam names that this build actually evaluates (the
 * known_seams[] catalogue in fail_inject.c, kept in sync with the FAIL_POINT*
 * call sites and carrying their compile gates).  An unknown name is -EINVAL
 * rather than an inert armed point, so a mistyped seam fails the test instead
 * of silently passing it.
 *
 * Arming a point with "off" is equivalent to fault_clear(name), and -- like
 * fault_clear() -- succeeds for any syntactically valid @name, known seam or
 * not: disarming something that was never armed is a no-op, not an error.
 *
 * Returns 0 on success, or -EINVAL if @action_spec cannot be parsed, or if
 * @action_spec is not "off" and @name is not a known seam.
 */
int fault_arm(const char *name, const char *action_spec);

/* Disarm a single named point (idempotent). */
void fault_clear(const char *name);

/* Disarm every armed point. */
void fault_clear_all(void);

/*
 * Serialise the currently-armed table into @buf as
 *
 *     <name>=<action> hits=<n>\n
 *
 * lines, always NUL-terminated (unless buflen == 0).  @n is how many times the
 * action actually fired, letting a driver distinguish "armed" from "reached".
 *
 * A counted point stops firing once its last firing is spent, but it keeps
 * appearing here with its final hit count -- "nfs3.write=3*return(28) hits=3"
 * -- so that "fired its full count" is distinguishable from "never armed".
 * Only fault_clear()/fault_clear_all() remove a line.
 *
 * Returns the number of bytes that WOULD have been written excluding the NUL
 * (snprintf semantics), so a return value >= buflen indicates truncation.
 */
size_t fault_list(char *buf, size_t buflen);

/*
 * ---------------------------------------------------------------------------
 * Evaluation core -- the macros below call this; not for direct use.
 * ---------------------------------------------------------------------------
 *
 * Evaluate the fail point @name.  sleep()/panic actions run in place; a firing
 * return(<errno>) action stores the errno in *out_errno and returns true (the
 * caller must bail out).  Fast path: one lock-free atomic read when nothing is
 * armed.
 */
bool fault_should_fail(const char *name, int *out_errno);

/*
 * FAIL_POINT(name)
 *   Seam for void-returning functions (or any place where a bare `return;` is
 *   the desired failure behaviour).  sleep()/panic run in place; a firing
 *   return(...) action causes an early `return;`.
 *
 * FAIL_POINT_RET(name, ret_on_fail)
 *   Seam for value-returning functions.  On a firing return(...) action the
 *   expression @ret_on_fail is evaluated and returned.  The parsed errno is
 *   exposed to that expression as the int `fi_errno`, letting the caller
 *   translate it into its own status type, e.g.:
 *
 *       FAIL_POINT_RET("nfs3.getattr",
 *                      (res->res_getattr3.status =
 *                           nfs3_Errno_status(posix2fsal_status(fi_errno)),
 *                       NFS_REQ_OK));
 */
#define FAIL_POINT(name)						\
	do {								\
		int fi_errno = 0;					\
		if (fault_should_fail((name), &fi_errno)) {		\
			(void)fi_errno;					\
			return;						\
		}							\
	} while (0)

#define FAIL_POINT_RET(name, ret_on_fail)				\
	do {								\
		int fi_errno = 0;					\
		if (fault_should_fail((name), &fi_errno))		\
			return (ret_on_fail);				\
	} while (0)

#ifdef __cplusplus
}
#endif

#else /* !ENABLE_FAULT_INJECTION -- compile to nothing */

#define FAIL_POINT(name)			do { } while (0)
#define FAIL_POINT_RET(name, ret_on_fail)	do { } while (0)

#endif /* ENABLE_FAULT_INJECTION */

#endif				/* FAIL_INJECT_H */
