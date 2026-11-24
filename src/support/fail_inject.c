// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright SmartX, 2026
 *  Contributor: Storage Team
 *
 * --------------------------
 *
 * Native fault-injection registry.
 *
 * A small, self-contained name -> action table protected by a single rwlock.
 * The table is a fixed static array (no dynamic allocation on the hot path);
 * lookups take the read lock, arm/clear take the write lock.  When no point is
 * armed, fault_should_fail() bails out after one lock-free atomic read, so a
 * fault-enabled build that has nothing armed pays almost nothing per seam.
 *
 * Action DSL (fail-rs subset):   [<prob>%][<count>*]<task>
 *   task  := off | return(<errno>) | sleep(<ms>) | panic
 */

#include "config.h"

#ifdef ENABLE_FAULT_INJECTION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "fail_inject.h"
#include "log.h"
#include "common_utils.h"
#include "abstract_atomic.h"

#define FAULT_MAX_POINTS	256
#define FAULT_NAME_MAX		96
#define FAULT_SPEC_MAX		64
#define PROB_SCALE		1000000	/* probability resolution (ppm) */

enum fault_op {
	FAULT_OFF = 0,
	FAULT_RETURN,	/* fail with an errno */
	FAULT_SLEEP,	/* delay, then continue */
	FAULT_PANIC	/* abort() the process */
};

struct fault_point {
	bool in_use;
	enum fault_op op;
	int arg;		/* errno for RETURN, milliseconds for SLEEP */
	uint32_t prob_ppm;	/* firing probability, parts-per-million */
	bool unlimited;		/* true => fire forever; else honour @count */
	int32_t count;		/* remaining firings when !unlimited */
	uint64_t hits;		/* times the action actually fired */
	char name[FAULT_NAME_MAX];
	char spec[FAULT_SPEC_MAX];	/* verbatim action text, for listing */
};

static struct fault_point registry[FAULT_MAX_POINTS];
static pthread_rwlock_t registry_lock;

/*
 * Two distinct counters, because "occupies a slot" and "can still fire" are
 * different questions and conflating them costs one of them:
 *
 *   @used_slots  -- slots with in_use set.  Bounds the find_point_locked()
 *                   scan so a lookup miss does not strcmp() its way through
 *                   all FAULT_MAX_POINTS entries.
 *
 *   @armed_count -- slots with op != FAULT_OFF, i.e. points that would still
 *                   do something.  This is the lock-free hot-path gate: when
 *                   it is zero, fault_should_fail() returns after one atomic
 *                   read and never takes the rwlock.
 *
 * A counted point that has spent its last firing ("3*return(28)" after three
 * hits) is *disarmed* (op = FAULT_OFF, armed_count--) but keeps its slot, its
 * spec and its hit count, so fault_list() can still report
 *
 *     nfs3.write=3*return(28) hits=3
 *
 * and a driver can tell "fired all three times" apart from "never armed".
 * Only fault_clear()/fault_clear_all() actually free a slot.  There is no
 * leak: a slot is only ever allocated for a name in known_seams[], so at most
 * ARRAY_SIZE(known_seams) slots can be occupied at once.
 */
static int32_t used_slots;
static int32_t armed_count;
static bool initialised;

/*
 * ---------------------------------------------------------------------------
 * Seam catalogue
 * ---------------------------------------------------------------------------
 *
 * The names that actually have a FAIL_POINT()/FAIL_POINT_RET() call site in
 * this build.  fault_arm() rejects anything else, because arming a name that
 * no seam ever evaluates would return "200 OK" and then quietly do nothing --
 * a typo in a test would read as a passing fault-injection run (false green).
 *
 * The seams live in conditionally-compiled translation units, so the catalogue
 * has to carry the same conditions.  Listing a name unconditionally would
 * re-introduce the exact false green this array exists to prevent: on a
 * -DUSE_NFS3=OFF build, arming "nfs3.write" would answer 200 while
 * Protocols/NFS/nfs3_write.c is not even in the link.
 *
 * Keep this list in sync with the call sites; grep for FAIL_POINT to audit:
 *   nfs3.write            Protocols/NFS/nfs3_write.c    [USE_NFS3]
 *   nfs3.read             Protocols/NFS/nfs3_read.c     [USE_NFS3]
 *   nfs3.getattr          Protocols/NFS/nfs3_getattr.c  [USE_NFS3]
 *   nfs4.open.reclaim     Protocols/NFS/nfs4_op_open.c  [unconditional]
 *
 * Deliberately NOT seamed, so nobody adds them back: the SFS cluster recovery
 * backend (SAL/recovery/recovery_sfs_cluster.c).  end_grace() and read_clids()
 * both run with the global grace_mutex held, so a sleep action there stalls
 * reclaim for every client at once; nfs_health() then reports Hung and the node
 * is fenced by the very failover the test meant to observe.  The per-client
 * paths are no better as seams, because a bare FAIL_POINT expands to
 * "if (should_fail) return;" and would silently skip the rest of a void
 * function -- SAL bookkeeping bypassed, nothing reported anywhere.  Fault the
 * protocol layer instead; that is what nfs4.open.reclaim is for.
 */
static const char *const known_seams[] = {
#ifdef _USE_NFS3
	"nfs3.write",
	"nfs3.read",
	"nfs3.getattr",
#endif
	"nfs4.open.reclaim",
	NULL,	/* terminator for the seam_is_known() scan */
};

static bool seam_is_known(const char *name)
{
	size_t i;

	for (i = 0; known_seams[i] != NULL; i++) {
		if (strcmp(known_seams[i], name) == 0)
			return true;
	}
	return false;
}

/*
 * ---------------------------------------------------------------------------
 * DSL parser
 * ---------------------------------------------------------------------------
 */

/*
 * Parse a run of decimal digits, advancing *pp.  Returns false if there are none, or if the
 * value will not survive being stored.
 *
 * The range check is not defensive tidiness.  Every caller narrows the result to int32_t or
 * int, so without it "2147483648*return(28)" would wrap to a NEGATIVE count -- a point that
 * retires immediately and never fires, while fault_list() still reports it armed -- and
 * "return(4294967301)" would quietly become EIO.  Both are false greens, which is the one
 * failure mode this whole file exists to avoid.
 */
static bool parse_uint(const char **pp, unsigned long *out)
{
	const char *p = *pp;
	char *end = NULL;
	unsigned long v;

	if (!isdigit((unsigned char)*p))
		return false;

	errno = 0;
	v = strtoul(p, &end, 10);
	if (end == p || errno == ERANGE || v > INT32_MAX)
		return false;

	*out = v;
	*pp = end;
	return true;
}

/* True once only blanks are left, i.e. the task keyword really ended here. */
static bool at_spec_end(const char *p)
{
	while (isspace((unsigned char)*p))
		p++;
	return *p == '\0';
}

/*
 * Parse @spec into @out.  Returns 0 on success, -EINVAL on error.
 * Grammar:  [<prob>%][<count>*]<task>
 */
static int parse_action(const char *spec, struct fault_point *out)
{
	const char *p = spec;
	unsigned long n;

	out->op = FAULT_OFF;
	out->arg = 0;
	out->prob_ppm = PROB_SCALE;	/* always, unless a p% is given */
	out->unlimited = true;
	out->count = 0;

	/* skip leading blanks */
	while (isspace((unsigned char)*p))
		p++;

	/* optional probability modifier: "<n>%" */
	if (parse_uint(&p, &n)) {
		if (*p == '%') {
			if (n > 100)
				return -EINVAL;
			out->prob_ppm = (uint32_t)(n * (PROB_SCALE / 100));
			p++;
		} else if (*p == '*') {
			/* it was actually the count modifier, not a prob */
			if (n == 0)
				return -EINVAL;	/* see the count check below */
			out->unlimited = false;
			out->count = (int32_t)n;
			p++;
			goto parse_task;
		} else {
			/* a bare leading number is not valid */
			return -EINVAL;
		}
	}

	/* optional count modifier: "<n>*" */
	if (parse_uint(&p, &n)) {
		if (*p != '*')
			return -EINVAL;
		/*
		 * Zero is not "unlimited" -- unlimited is the ABSENCE of the modifier.
		 * A zero count can never fire: the gate at fire time wants count > 0.
		 * Accepting it armed a point that answered 200, listed as armed, and did
		 * nothing; hits stayed 0 forever.  That is the one shape a fault-injection
		 * suite is least able to notice, because "no fault observed" is what a
		 * passing run looks like.  The header has always said `count := positive
		 * integer`; this is the implementation finally saying it too.
		 */
		if (n == 0)
			return -EINVAL;
		out->unlimited = false;
		out->count = (int32_t)n;
		p++;
	}

parse_task:
	/*
	 * The task keyword.  Every arm ends by checking that nothing but blanks follows, because
	 * a prefix match is not a match: without it "panics" and "panic_after_3" both parse as
	 * PANIC, and PANIC abort()s the process -- so a typo in a spec would answer 200 and then
	 * take ganesha down on the next request.  Trailing junk after a complete task
	 * ("return(5)junk") is refused for the same reason: it means the caller believed
	 * something about the grammar that is not true, and guessing which half they meant is
	 * how a fault ends up not being the one they asked for.
	 */
	if (strncmp(p, "off", 3) == 0 && at_spec_end(p + 3)) {
		out->op = FAULT_OFF;
		return 0;
	}

	if (strncmp(p, "panic", 5) == 0 && at_spec_end(p + 5)) {
		out->op = FAULT_PANIC;
		return 0;
	}

	if (strncmp(p, "return(", 7) == 0) {
		p += 7;
		if (!parse_uint(&p, &n) || *p != ')' || !at_spec_end(p + 1))
			return -EINVAL;
		out->op = FAULT_RETURN;
		out->arg = (int)n;
		return 0;
	}

	if (strncmp(p, "sleep(", 6) == 0) {
		p += 6;
		if (!parse_uint(&p, &n) || *p != ')' || !at_spec_end(p + 1))
			return -EINVAL;
		out->op = FAULT_SLEEP;
		out->arg = (int)n;
		return 0;
	}

	return -EINVAL;
}

/*
 * ---------------------------------------------------------------------------
 * Table helpers (caller holds the appropriate lock)
 * ---------------------------------------------------------------------------
 */

static struct fault_point *find_point_locked(const char *name)
{
	/* @used_slots is exactly the number of in_use slots, so the scan can
	 * stop as soon as it has seen them all instead of strcmp()ing its way
	 * through the whole 256-entry table on every miss.  (Not @armed_count:
	 * a disarmed-but-retained slot still has to be findable, otherwise
	 * fault_list() and a re-arm would both miss it.)
	 */
	int32_t remaining = atomic_fetch_int32_t(&used_slots);
	int i;

	for (i = 0; i < FAULT_MAX_POINTS && remaining > 0; i++) {
		if (!registry[i].in_use)
			continue;
		remaining--;
		if (strcmp(registry[i].name, name) == 0)
			return &registry[i];
	}
	return NULL;
}

static struct fault_point *find_free_slot_locked(void)
{
	int i;

	for (i = 0; i < FAULT_MAX_POINTS; i++) {
		if (!registry[i].in_use)
			return &registry[i];
	}
	return NULL;
}

/*
 * ---------------------------------------------------------------------------
 * Public: lifecycle
 * ---------------------------------------------------------------------------
 */

void fault_registry_init(void)
{
	if (initialised)
		return;

	PTHREAD_RWLOCK_init(&registry_lock, NULL);
	memset(registry, 0, sizeof(registry));
	atomic_store_int32_t(&used_slots, 0);
	atomic_store_int32_t(&armed_count, 0);

	/* Seed the PRNG used for probability modifiers. */
	srandom((unsigned int)(time(NULL) ^ (long)getpid()));

	initialised = true;
	LogEvent(COMPONENT_INIT, "fault injection: registry initialised");
}

void fault_registry_destroy(void)
{
	if (!initialised)
		return;

	PTHREAD_RWLOCK_destroy(&registry_lock);
	initialised = false;
}

/*
 * ---------------------------------------------------------------------------
 * Public: arm / clear / list
 * ---------------------------------------------------------------------------
 */

int fault_arm(const char *name, const char *action_spec)
{
	struct fault_point parsed;
	struct fault_point *slot;
	int rc;

	if (name == NULL || name[0] == '\0' ||
	    strlen(name) >= FAULT_NAME_MAX)
		return -EINVAL;

	/*
	 * The spec is kept verbatim for listing, in a fixed FAULT_SPEC_MAX field, and the
	 * control server deliberately REFUSES an oversized body rather than truncating it
	 * (fault_control.cc: "Reject rather than silently truncate an oversized action",
	 * 413 above 255).  Truncating here would quietly undo that decision for everything
	 * in between: a spec of 64..255 bytes armed, answered 200, and listed back short --
	 * and the listing is what a driver compares against to prove the arm landed.
	 *
	 * Reachable today, not hypothetical: parse_action() skips leading blanks, so 54
	 * spaces plus "return(28)" is 64 bytes, parses, and lost its final ')' in the store.
	 * Refuse it here, exactly as an overlong name is refused above.
	 */
	if (action_spec != NULL && strlen(action_spec) >= FAULT_SPEC_MAX)
		return -EINVAL;

	rc = parse_action(action_spec ? action_spec : "off", &parsed);
	if (rc != 0)
		return rc;

	/* "off" == clear.  Deliberately handled before the seam check, so that
	 * disarming an unknown name is a no-op success exactly like
	 * DELETE /debug/failpoints/<unknown> is.  Tearing down a point that was
	 * never armed is what every test teardown does; it must not be an error
	 * on one verb and a 400 on the other.
	 */
	if (parsed.op == FAULT_OFF) {
		fault_clear(name);
		return 0;
	}

	/* Refuse to *arm* a name with no seam behind it: an accepted-but-inert
	 * point makes a broken test look like a passing one.
	 */
	if (!seam_is_known(name)) {
		LogWarn(COMPONENT_INIT,
			"fault injection: rejecting unknown fail point '%s'",
			name);
		return -EINVAL;
	}

	PTHREAD_RWLOCK_wrlock(&registry_lock);

	slot = find_point_locked(name);
	if (slot == NULL) {
		slot = find_free_slot_locked();
		if (slot == NULL) {
			PTHREAD_RWLOCK_unlock(&registry_lock);
			LogWarn(COMPONENT_INIT,
				"fault injection: registry full (%d points)",
				FAULT_MAX_POINTS);
			return -ENOSPC;
		}
		slot->in_use = true;
		atomic_inc_int32_t(&used_slots);
	}

	/* Re-arming a slot that is still armed must not double-count; re-arming
	 * one that retired itself (or was never armed) puts it back on the hot
	 * path.  parsed.op is never FAULT_OFF here -- that returned above.
	 */
	if (slot->op == FAULT_OFF)
		atomic_inc_int32_t(&armed_count);

	slot->op = parsed.op;
	slot->arg = parsed.arg;
	slot->prob_ppm = parsed.prob_ppm;
	slot->unlimited = parsed.unlimited;
	slot->count = parsed.count;
	slot->hits = 0;		/* re-arming restarts the hit accounting */
	strncpy(slot->name, name, sizeof(slot->name) - 1);
	slot->name[sizeof(slot->name) - 1] = '\0';
	strncpy(slot->spec, action_spec, sizeof(slot->spec) - 1);
	slot->spec[sizeof(slot->spec) - 1] = '\0';

	PTHREAD_RWLOCK_unlock(&registry_lock);
	return 0;
}

void fault_clear(const char *name)
{
	struct fault_point *slot;

	if (name == NULL || name[0] == '\0')
		return;

	PTHREAD_RWLOCK_wrlock(&registry_lock);
	slot = find_point_locked(name);
	if (slot != NULL) {
		/* An exhausted point is already off the hot path; only take it
		 * out of @armed_count if it was still armed.
		 */
		if (slot->op != FAULT_OFF)
			atomic_dec_int32_t(&armed_count);
		memset(slot, 0, sizeof(*slot));
		atomic_dec_int32_t(&used_slots);
	}
	PTHREAD_RWLOCK_unlock(&registry_lock);
}

void fault_clear_all(void)
{
	PTHREAD_RWLOCK_wrlock(&registry_lock);
	memset(registry, 0, sizeof(registry));
	atomic_store_int32_t(&used_slots, 0);
	atomic_store_int32_t(&armed_count, 0);
	PTHREAD_RWLOCK_unlock(&registry_lock);
}

size_t fault_list(char *buf, size_t buflen)
{
	size_t off = 0;
	int i;

	if (buflen > 0)
		buf[0] = '\0';

	PTHREAD_RWLOCK_rdlock(&registry_lock);
	for (i = 0; i < FAULT_MAX_POINTS; i++) {
		int n;

		if (!registry[i].in_use)
			continue;

		/* snprintf into the remaining space; keep accumulating the
		 * "would-be" length even past the end so callers can detect
		 * truncation.  The hit count lets a driver assert that the seam
		 * was really reached rather than merely armed. */
		n = snprintf(off < buflen ? buf + off : NULL,
			     off < buflen ? buflen - off : 0,
			     "%s=%s hits=%" PRIu64 "\n",
			     registry[i].name, registry[i].spec,
			     atomic_fetch_uint64_t(&registry[i].hits));
		if (n > 0)
			off += (size_t)n;
	}
	PTHREAD_RWLOCK_unlock(&registry_lock);

	return off;
}

/*
 * ---------------------------------------------------------------------------
 * Public: evaluation core (called by the FAIL_POINT* macros)
 * ---------------------------------------------------------------------------
 */

/* Probability + count gate.  Caller holds the read lock. */
static bool point_gate_passes(struct fault_point *p)
{
	if (p->prob_ppm < PROB_SCALE) {
		uint32_t roll = (uint32_t)(random() % PROB_SCALE);

		if (roll >= p->prob_ppm)
			return false;
	}

	if (!p->unlimited) {
		/* Already exhausted?  Avoid unbounded negative drift. */
		if (atomic_fetch_int32_t(&p->count) <= 0)
			return false;
		/* Claim a token; a lost race yields a negative value. */
		if (atomic_dec_int32_t(&p->count) < 0)
			return false;
	}

	return true;
}

/*
 * Disarm a counted point that has spent its last token.
 *
 * Without this an exhausted "3*return(5)" keeps armed_count above zero
 * forever, so every seam in the process -- nfs3.read and nfs3.write included
 * -- keeps paying the rdlock plus a table walk on every single call long
 * after the fault is over.  Clearing @op restores the lock-free "nothing
 * armed" fast path.
 *
 * The slot itself is deliberately *not* released: @spec and @hits stay
 * readable so fault_list() keeps reporting "nfs3.write=3*return(28) hits=3".
 * Freeing it here would make "the point fired its full count" and "the point
 * was never armed" indistinguishable to a test driver -- which is precisely
 * the observation the hit counter was added for.  fault_clear() /
 * fault_clear_all() are the only things that free a slot.
 *
 * The condition is re-tested under the write lock, so a concurrent re-arm
 * (which resets @op, @count and @unlimited) is not clobbered.  The op check
 * also makes a double retire -- two threads spending the last two tokens
 * concurrently -- idempotent rather than a double decrement.
 */
static void retire_exhausted(const char *name)
{
	struct fault_point *slot;

	PTHREAD_RWLOCK_wrlock(&registry_lock);
	slot = find_point_locked(name);
	if (slot != NULL && slot->op != FAULT_OFF &&
	    !slot->unlimited && slot->count <= 0) {
		slot->op = FAULT_OFF;
		atomic_dec_int32_t(&armed_count);
	}
	PTHREAD_RWLOCK_unlock(&registry_lock);
}

static void fault_do_sleep(int ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

bool fault_should_fail(const char *name, int *out_errno)
{
	enum fault_op op = FAULT_OFF;
	int arg = 0;
	bool exhausted = false;
	struct fault_point *p;

	/* Hot path: nothing armed -> single atomic read, no locking. */
	if (atomic_fetch_int32_t(&armed_count) == 0)
		return false;

	PTHREAD_RWLOCK_rdlock(&registry_lock);
	p = find_point_locked(name);
	if (p != NULL && p->op != FAULT_OFF) {
		if (point_gate_passes(p)) {
			op = p->op;
			arg = p->arg;
			atomic_inc_uint64_t(&p->hits);
		}
		/* Last token spent (or lost to a race): disarm it once the read
		 * lock is gone.  Testing op != FAULT_OFF above matters: an
		 * already-retired slot stays in the table (to keep its hits
		 * visible), and without the guard every later call through this
		 * seam would take the write lock to retire it again.
		 */
		if (!p->unlimited && atomic_fetch_int32_t(&p->count) <= 0)
			exhausted = true;
	}
	PTHREAD_RWLOCK_unlock(&registry_lock);

	if (exhausted)
		retire_exhausted(name);

	switch (op) {
	case FAULT_SLEEP:
		/* Sleep is done outside the lock to avoid stalling arm/clear. */
		fault_do_sleep(arg);
		return false;
	case FAULT_PANIC:
		/* abort() directly, NOT LogFatal(): LogFatal() calls Fatal()
		 * (log/log_functions.c), which runs Cleanup() and exits on
		 * whatever thread happened to trip the seam.  That destroys
		 * global state out from under its holders and turns the crash a
		 * test asked for into an orderly shutdown -- no signal is ever
		 * raised, so no core is written.
		 *
		 * abort() raises SIGABRT instead.  Whether that leaves a core
		 * file is up to the host (core_pattern, RLIMIT_CORE), not up to
		 * ganesha.  If ganesha was started with -C, init_crash_handlers()
		 * (MainNFSD/nfs_init.c) has a SIGABRT handler installed that
		 * additionally logs a backtrace and re-raises under SA_RESETHAND
		 * so the default disposition still applies; without -C, SIGABRT
		 * simply takes its default disposition.
		 */
		LogCrit(COMPONENT_INIT,
			"fault injection: panic action at fail point '%s', aborting",
			name);
		abort();
		/* not reached */
		return false;
	case FAULT_RETURN:
		*out_errno = arg;
		return true;
	case FAULT_OFF:
	default:
		return false;
	}
}

#endif /* ENABLE_FAULT_INJECTION */
