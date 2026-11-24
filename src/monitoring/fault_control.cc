// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright SmartX, 2026
 *  Contributor: Storage Team
 *
 * --------------------------
 *
 * Combined civetweb control server for the fault-injection facility.
 *
 * This TU is compiled only in test builds (ENABLE_FAULT_INJECTION).  It stands
 * up ONE civetweb context on the monitoring port that serves BOTH endpoints:
 *
 *     GET    /metrics                          Prometheus text exposition
 *     GET    /debug/failpoints                 list armed fail points
 *     PUT    /debug/failpoints/<name> -d <act> arm <name>
 *     POST   /debug/failpoints/<name> -d <act> arm <name> (curl -d default)
 *     DELETE /debug/failpoints/<name>          clear <name>
 *     DELETE /debug/failpoints                 clear every point
 *
 * Why our own civetweb instead of prometheus::Exposer?  Exposer's public API
 * is metrics-only (RegisterCollectable/RegisterAuth) and keeps its CivetServer
 * private, so we cannot add a /debug/failpoints route to it nor share its
 * socket.  In a production build monitoring_init() keeps using Exposer verbatim
 * (see monitoring.cc); only when ENABLE_FAULT_INJECTION is set does it call
 * fault_control_start() and serve /metrics ourselves via prometheus's
 * TextSerializer over the shared Registry -- so no second port is opened.
 *
 * The /metrics path is C++ (prometheus Registry/TextSerializer); the
 * /debug/failpoints path drives the pure-C registry in support/fail_inject.c
 * via its extern "C" header.  Hence this control server lives in a C++ TU.
 *
 * NOTE ON LINKAGE: mg_start/mg_printf/... are compiled into
 * libprometheus-cpp-pull but are NOT re-exported.  Building this file requires
 * the civetweb C symbols to be linkable, which is why src/CMakeLists.txt
 * insists on a standalone civetweb library and fails configure without one.
 */

#include "config.h"

#ifdef ENABLE_FAULT_INJECTION

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include <prometheus/registry.h>
#include <prometheus/text_serializer.h>
#include <prometheus/civetweb.h>

#include "fail_inject.h"
#include "log.h"

/*
 * civetweb ABI guard.
 *
 * failpoints_handler() reads struct mg_request_info directly, and that struct
 * is not layout-stable across civetweb releases: 1.15 inserted local_uri_raw
 * ahead of local_uri.  Compiling against one civetweb.h while linking a
 * different libcivetweb therefore silently reads the wrong member: what this
 * TU calls local_uri lands on the neighbouring local_uri_raw (the request
 * target *before* civetweb normalises it, so "a/../b" style traversal is still
 * in it), or -- if the header describes a struct longer than the library's --
 * past the end of the object entirely.
 *
 * Pin the header at compile time here (CMake makes the same assertion against
 * the header it found), and cross-check the *linked* library at start-up via
 * mg_version(), which returns the library's own CIVETWEB_VERSION.
 */
#if !defined(CIVETWEB_VERSION) || !defined(CIVETWEB_VERSION_MAJOR) || \
	!defined(CIVETWEB_VERSION_MINOR)
#error "civetweb.h carries no version macros: cannot verify mg_request_info layout"
#endif

#if (CIVETWEB_VERSION_MAJOR * 100 + CIVETWEB_VERSION_MINOR) < 115
#error "fault control requires civetweb >= 1.15 (mg_request_info::local_uri_raw)"
#endif

namespace {

/*
 * Fold the leading "<major>.<minor>" of a civetweb version string into
 * major * 100 + minor; -1 if it does not start with two dotted decimals.
 *
 * Anything after the minor component (a patch number, a packaging suffix) is
 * deliberately ignored.  Patch releases do not restructure mg_request_info,
 * and mg_version() is not contractually obliged to spell its trailing
 * component exactly the way the CIVETWEB_VERSION macro does -- comparing the
 * whole string would reject perfectly compatible pairings.
 */
int civetweb_version_id(const char *version)
{
	char *end = nullptr;
	const char *minor_start;
	long major;
	long minor;

	if (version == nullptr)
		return -1;

	major = std::strtol(version, &end, 10);
	if (end == version || *end != '.')
		return -1;

	minor_start = end + 1;
	minor = std::strtol(minor_start, &end, 10);
	if (end == minor_start)
		return -1;

	if (major < 0 || major > 99 || minor < 0 || minor > 99)
		return -1;

	return static_cast<int>(major * 100 + minor);
}

struct mg_context *fault_ctx;
std::shared_ptr<prometheus::Registry> fault_registry;

const char kFailPrefix[] = "/debug/failpoints";

/* Emit a minimal response with an empty body and the given status line. */
int reply_status(struct mg_connection *conn, int code, const char *text)
{
	mg_printf(conn,
		  "HTTP/1.1 %d %s\r\n"
		  "Content-Length: 0\r\n"
		  "Connection: close\r\n"
		  "\r\n",
		  code, text);
	return code;
}

/* Emit a text/plain body response with an explicit content type. */
int reply_body(struct mg_connection *conn, int code, const char *text,
	       const char *content_type, const std::string &body)
{
	mg_printf(conn,
		  "HTTP/1.1 %d %s\r\n"
		  "Content-Type: %s\r\n"
		  "Content-Length: %zu\r\n"
		  "Connection: close\r\n"
		  "\r\n",
		  code, text, content_type, body.size());
	if (!body.empty())
		mg_write(conn, body.data(), body.size());
	return code;
}

/* GET /metrics -- serialise the shared Registry in Prometheus text format. */
int metrics_handler(struct mg_connection *conn, void *cbdata)
{
	(void)cbdata;
	std::string body;

	if (fault_registry) {
		const auto collected = fault_registry->Collect();
		std::ostringstream oss;

		prometheus::TextSerializer().Serialize(oss, collected);
		body = oss.str();
	}

	return reply_body(conn, 200, "OK",
			  "text/plain; version=0.0.4; charset=utf-8", body);
}

/*
 * GET/PUT/POST/DELETE /debug/failpoints[/<name>].  Registered at the prefix,
 * so civetweb routes both "/debug/failpoints" and "/debug/failpoints/<name>"
 * here; we recover <name> by stripping the prefix from the request URI.
 */
int failpoints_handler(struct mg_connection *conn, void *cbdata)
{
	const struct mg_request_info *ri = mg_get_request_info(conn);
	const char *method;
	const char *uri;
	const char *name = nullptr;
	const size_t plen = sizeof(kFailPrefix) - 1;

	(void)cbdata;

	if (ri == nullptr)
		return reply_status(conn, 500, "Internal Server Error");

	method = ri->request_method ? ri->request_method : "";
	/* local_uri (not local_uri_raw): civetweb has already collapsed
	 * "allowed/../forbidden" style traversal out of it.
	 */
	uri = ri->local_uri ? ri->local_uri : "";

	if (std::strncmp(uri, kFailPrefix, plen) == 0) {
		if (uri[plen] == '/')
			name = uri + plen + 1;	/* ".../<name>" -> "<name>" */
		else if (uri[plen] == '\0')
			name = "";		/* bare prefix         */
	}
	if (name == nullptr)
		return reply_status(conn, 404, "Not Found");

	/* GET -> list */
	if (std::strcmp(method, "GET") == 0) {
		char buf[8192];
		size_t n = fault_list(buf, sizeof(buf));

		if (n >= sizeof(buf))
			n = sizeof(buf) - 1;	/* truncated; send what fits */
		return reply_body(conn, 200, "OK", "text/plain",
				  std::string(buf, n));
	}

	/* PUT|POST /debug/failpoints/<name> -d '<action>' -> arm */
	if (std::strcmp(method, "PUT") == 0 ||
	    std::strcmp(method, "POST") == 0) {
		char action[FAULT_CONTROL_BODY_MAX];
		const size_t cap = sizeof(action) - 1;
		const long long want = ri->content_length;
		size_t len = 0;
		int rc;

		if (name[0] == '\0')
			return reply_status(conn, 400, "Bad Request");

		/* Reject rather than silently truncate an oversized action. */
		if (want > static_cast<long long>(cap))
			return reply_status(conn, 413, "Payload Too Large");

		/* mg_read() returns short reads whenever the body spans more
		 * than one TCP segment, so loop until the declared
		 * content_length is in hand (or EOF for a chunked body).  A
		 * single read here made an arm silently take a truncated action
		 * -- a flake that only shows up under a fragmenting network.
		 */
		while (len < cap) {
			int rd = mg_read(conn, action + len, cap - len);

			if (rd <= 0)
				break;		/* 0 == EOF, < 0 == error */
			len += static_cast<size_t>(rd);
			if (want >= 0 && static_cast<long long>(len) >= want)
				break;
		}

		if (want >= 0 && static_cast<long long>(len) < want) {
			/* Client promised more than it sent. */
			return reply_status(conn, 400, "Bad Request");
		}

		if (want < 0 && len == cap) {
			/* Undeclared (chunked) body: if anything is still
			 * pending, we would be truncating it.
			 */
			char overflow;

			if (mg_read(conn, &overflow, 1) > 0)
				return reply_status(conn, 413,
						    "Payload Too Large");
		}

		action[len] = '\0';

		/* trim trailing CR/LF/space that curl -d may append */
		while (len > 0 && (action[len - 1] == '\n' ||
				   action[len - 1] == '\r' ||
				   action[len - 1] == ' '))
			action[--len] = '\0';

		rc = fault_arm(name, action);
		if (rc != 0) {
			/* Either <name> is not a seam this build evaluates, or
			 * the action does not parse.  Both are -EINVAL.
			 */
			return reply_body(conn, 400, "Bad Request",
					  "text/plain",
					  std::string("unknown fail point or "
						      "invalid action\n"));
		}

		/* fault_arm() treats "off" as a disarm, and accepts it for any
		 * name (mirroring DELETE), so do not claim we armed something.
		 */
		LogEvent(COMPONENT_INIT, "fault injection: set '%s' = '%s'",
			 name, action);
		return reply_status(conn, 200, "OK");
	}

	/* DELETE /debug/failpoints[/<name>] -> clear */
	if (std::strcmp(method, "DELETE") == 0) {
		if (name[0] == '\0')
			fault_clear_all();
		else
			fault_clear(name);

		LogEvent(COMPONENT_INIT, "fault injection: cleared '%s'",
			 name[0] != '\0' ? name : "(all)");
		return reply_status(conn, 200, "OK");
	}

	return reply_status(conn, 405, "Method Not Allowed");
}

}  /* anonymous namespace */

namespace ganesha_monitoring {

/*
 * Start our own civetweb on 0.0.0.0:<port> serving /metrics AND
 * /debug/failpoints, and remember the shared Registry so /metrics can
 * serialise it.  Called from monitoring_init() in test builds instead of
 * constructing a prometheus::Exposer.
 */
void fault_control_start(uint16_t port,
			 std::shared_ptr<prometheus::Registry> registry)
{
	char listen_spec[32];
	const char *lib_version = mg_version();
	const int lib_id = civetweb_version_id(lib_version);
	const int hdr_id = CIVETWEB_VERSION_MAJOR * 100 + CIVETWEB_VERSION_MINOR;

	/*
	 * Compile-time we checked the header; check the library we actually
	 * linked.  A skew here means struct mg_request_info is laid out
	 * differently than this TU believes, so refuse to run rather than
	 * misread it on the first request.
	 *
	 * major.minor must match exactly -- not merely be ">= 1.15".  The 1.15
	 * local_uri_raw insertion is itself the proof that a *minor* bump can
	 * reorder mg_request_info, so "new enough" is not the same claim as
	 * "same layout".  Only the patch/suffix component is treated as ABI
	 * neutral (see civetweb_version_id()).
	 */
	if (lib_id < 0) {
		LogFatal(COMPONENT_INIT,
			 "fault injection: cannot parse civetweb library version '%s'; refusing to guess the struct mg_request_info layout",
			 lib_version != nullptr ? lib_version : "(null)");
	}

	if (lib_id != hdr_id) {
		LogFatal(COMPONENT_INIT,
			 "fault injection: civetweb ABI mismatch: built against %s, linked %s -- struct mg_request_info layout is not stable across releases",
			 CIVETWEB_VERSION, lib_version);
	}

	/* Registry works even without the HTTP front end (unit tests). */
	fault_registry_init();
	fault_registry = registry;

	if (port == 0) {
		LogEvent(COMPONENT_INIT,
			 "fault injection: HTTP control disabled (port 0)");
		return;
	}

	if (fault_ctx != nullptr)
		return;			/* already started */

	std::snprintf(listen_spec, sizeof(listen_spec), "0.0.0.0:%u",
		      static_cast<unsigned int>(port));

	const char *options[] = {
		"listening_ports", listen_spec,
		"num_threads", "2",
		"request_timeout_ms", "5000",
		nullptr
	};

	fault_ctx = mg_start(nullptr, nullptr, options);
	if (fault_ctx == nullptr) {
		LogWarn(COMPONENT_INIT,
			"fault injection: failed to start control server on %s",
			listen_spec);
		return;
	}

	mg_set_request_handler(fault_ctx, "/metrics", metrics_handler, nullptr);
	mg_set_request_handler(fault_ctx, kFailPrefix, failpoints_handler,
			       nullptr);

	LogEvent(COMPONENT_INIT,
		 "fault injection: serving /metrics + /debug/failpoints on %s",
		 listen_spec);
}

}  /* namespace ganesha_monitoring */

#endif /* ENABLE_FAULT_INJECTION */
