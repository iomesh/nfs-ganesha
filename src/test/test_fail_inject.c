// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Tests for the fault-injection action parser AND for what fault_arm() refuses.
 *
 * Built only when ENABLE_FAULT_INJECTION is on, since that is the only build in which any of
 * this exists.  Run it as `make test_fail_inject && ./test/test_fail_inject`.
 *
 * fail_inject.c is #included rather than linked: its symbols are local to libganesha_nfsd and
 * cannot be reached from outside it.
 *
 * Two layers are covered, because two different things refuse.  parse_action() rejects a spec
 * it cannot mean; fault_arm() additionally rejects one it could parse but could not STORE --
 * slot->spec is a fixed field, and truncating to fit would list the arm back short.  A test
 * that stopped at the parser could not see that second check at all.
 *
 * Reaching fault_arm() needs the registry lock that fault_registry_init() creates, and that
 * function ends in LogEvent(COMPONENT_INIT, ...) -- which faults in a bare test binary, since
 * ganesha's logging is set up by a running server.  Silencing that one component first makes
 * the macro's level test short-circuit before it reaches the log path; the lock is still
 * created.  That is all the arm-layer cases need.
 *
 * Some REFUSED cases below are specs that used to be ACCEPTED -- those are the defects
 * this file now pins shut.  The rest pin shapes that were already refused, so that a
 * later parser change cannot quietly start accepting them.
 */

#include "config.h"

#ifndef ENABLE_FAULT_INJECTION

#include <stdio.h>

int main(void)
{
	printf("fault injection is not enabled in this build; nothing to test\n");
	return 0;
}

#else

#include "../support/fail_inject.c"

static int failures;

static void must_parse(const char *spec, enum fault_op want_op, int want_arg)
{
	struct fault_point got;
	int rc = parse_action(spec, &got);

	if (rc != 0) {
		printf("FAIL: %-28s should parse, got rc=%d\n", spec, rc);
		failures++;
		return;
	}
	if (got.op != want_op || (want_arg >= 0 && got.arg != want_arg)) {
		printf("FAIL: %-28s parsed as op=%d arg=%d, want op=%d arg=%d\n",
		       spec, (int)got.op, got.arg, (int)want_op, want_arg);
		failures++;
	}
}

static void must_refuse(const char *spec, const char *why)
{
	struct fault_point got;
	int rc = parse_action(spec, &got);

	if (rc == 0) {
		printf("FAIL: %-28s should have been refused (%s), parsed as op=%d arg=%d count=%d\n",
		       spec, why, (int)got.op, got.arg, got.count);
		failures++;
		return;
	}
	if (rc != -EINVAL) {
		printf("FAIL: %-28s refused with rc=%d, want -EINVAL\n", spec, rc);
		failures++;
	}
}

/*
 * The helpers above stop at parse_action().  The store-capacity check they cannot see
 * lives in fault_arm(), so the two below reach that layer instead.  fault_arm() takes the
 * registry lock, which only fault_registry_init() creates -- main() calls it before any
 * arm-level case runs.
 */
static void must_arm(const char *name, const char *spec, const char *why)
{
	int rc = fault_arm(name, spec);

	if (rc != 0) {
		printf("FAIL: %s: fault_arm(\"%s\") = %d, want 0\n", why, spec, rc);
		failures++;
		return;
	}
	fault_arm(name, "off");
}

static void must_refuse_arm(const char *name, const char *spec, const char *why)
{
	int rc = fault_arm(name, spec);

	if (rc == 0) {
		printf("FAIL: %s: fault_arm(\"%s\") was accepted\n", why, spec);
		failures++;
		fault_arm(name, "off");
		return;
	}
	/* The contract is -EINVAL specifically.  Accepting any non-zero would let a later
	 * change return, say, -ENOSPC and still pass -- a test that cannot fail the way it
	 * is meant to fail.
	 */
	if (rc != -EINVAL) {
		printf("FAIL: %s: fault_arm(\"%s\") = %d, want -EINVAL\n", why, spec, rc);
		failures++;
	}
}

int main(void)
{
	char overlong[80];
	int i;

	/* The grammar, as documented: [<prob>%][<count>*]<task> */
	must_parse("off", FAULT_OFF, -1);
	must_parse("panic", FAULT_PANIC, -1);
	must_parse("return(28)", FAULT_RETURN, 28);
	must_parse("return(10033)", FAULT_RETURN, 10033);
	must_parse("sleep(100)", FAULT_SLEEP, 100);
	must_parse("3*return(28)", FAULT_RETURN, 28);
	must_parse("50%return(28)", FAULT_RETURN, 28);
	must_parse("50%3*sleep(10)", FAULT_SLEEP, 10);
	must_parse("  return(28)  ", FAULT_RETURN, 28);
	/* A zero count can never fire: the gate at fire time wants count > 0.  This line
	 * used to be a must_parse -- the test pinned the defect as the contract.
	 */
	must_refuse("0*panic", "a zero count can never fire");
	/* The count is parsed in TWO places: bare "<n>*" above, and "<p>%<n>*" where the
	 * first number turned out to be the probability.  Both had to learn to refuse a
	 * zero, so both need a case -- with only the first, the second check could be
	 * deleted and every test would still pass.
	 */
	must_refuse("50%0*panic", "a zero count can never fire after a probability");
	must_parse("1*panic", FAULT_PANIC, -1);	/* the smallest count that can */
	must_parse("50%1*panic", FAULT_PANIC, -1);
	must_parse("2147483647*panic", FAULT_PANIC, -1);

	/* A keyword has to END where it ends.  "panic" was matched as a PREFIX, so every one of
	 * these parsed as PANIC — and PANIC abort()s the process, so a typo in a spec answered
	 * 200 and then took ganesha down on the next request it served.
	 */
	must_refuse("panics", "panic matched as a prefix");
	must_refuse("panic_after_3", "panic matched as a prefix");
	must_refuse("panicX", "panic matched as a prefix");
	must_refuse("offset", "off matched as a prefix");

	/* Trailing text means the caller believed something about the grammar that is not true.
	 * Guessing which half they meant is how a run injects a fault nobody asked for.
	 */
	must_refuse("return(28)junk", "trailing text after a complete task");
	must_refuse("sleep(10)xyz", "trailing text after a complete task");
	must_refuse("panic junk", "trailing text after a complete task");
	must_refuse("off nonsense", "trailing text after a complete task");

	/* Every count and errno is narrowed to a 32-bit signed int on the way in.  Unchecked,
	 * this count wrapped NEGATIVE: the point retired before its first evaluation, never
	 * fired, and still listed as armed.  The errno silently became something else.
	 */
	must_refuse("2147483648*return(28)", "count overflows int32");
	must_refuse("99999999999999999999*panic", "count overflows unsigned long");
	must_refuse("return(4294967301)", "errno overflows int32");

	/* Malformed shapes that were already refused; pinned so a parser change cannot quietly
	 * start accepting them.
	 */
	must_refuse("", "empty spec");
	must_refuse("return", "return without a value");
	must_refuse("return(", "unterminated return");
	must_refuse("return(28", "unterminated return");
	must_refuse("return()", "return with no digits");
	must_refuse("101%panic", "probability above 100");
	must_refuse("nonsense", "not a task word");
	must_refuse("3*", "count with no task");
	must_refuse("7", "a bare number is not a task");

	/*
	 * Arm-layer cases.  Everything above stops at parse_action(); the store-capacity
	 * check does not live there.
	 *
	 * fault_registry_init() ends in LogEvent(COMPONENT_INIT, ...), and a bare test
	 * binary has no logging set up -- that call segfaults.  Silencing the component
	 * short-circuits the macro's level test before it reaches the log path.  The lock
	 * the registry needs is still created; only the log line is skipped.
	 */
	component_log_level[COMPONENT_INIT] = NIV_NULL;
	fault_registry_init();

	/* nfs4.open.reclaim, not nfs3.*: the NFSv3 seams are behind _USE_NFS3, and nothing
	 * about spec length is specific to NFSv3.
	 */
	for (i = 0; i < 54; i++)
		overlong[i] = ' ';
	strcpy(overlong + 54, "return(28)");
	must_refuse_arm("nfs4.open.reclaim", overlong,
			"a 64-byte spec would be stored short");

	/* The same shape one byte under the cap still arms: the check added for the case
	 * above must not have turned leading blanks into a parse error.
	 */
	must_arm("nfs4.open.reclaim", overlong + 1,
		 "63 bytes is within the store");
	must_arm("nfs4.open.reclaim", "  return(28)  ",
		 "leading and trailing blanks stay legal");

	fault_clear_all();
	fault_registry_destroy();

	if (failures) {
		printf("\n%d failure(s)\n", failures);
		return 1;
	}
	printf("all action-parser and arm cases behaved as specified\n");
	return 0;
}

#endif /* ENABLE_FAULT_INJECTION */
