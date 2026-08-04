// SPDX-License-Identifier: LGPL-3.0-or-later
/* Offline correctness for request-scoped opaque values and their cleanup. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fsal.h"
#include "fsal_api.h"

/* Key identity off fileid so two handles can share one logical object. */
static void test_handle_to_key(struct fsal_obj_handle *obj_hdl,
			       struct gsh_buffdesc *fh_desc)
{
	fh_desc->addr = &obj_hdl->fileid;
	fh_desc->len = sizeof(obj_hdl->fileid);
}

static struct fsal_obj_ops test_ops = {
	.handle_to_key = test_handle_to_key,
};

static void test_init_obj(struct fsal_obj_handle *obj, uint64_t fileid)
{
	memset(obj, 0, sizeof(*obj));
	obj->obj_ops = &test_ops;
	obj->fileid = fileid;
}

struct test_value {
	uint64_t number;
	unsigned int *releases;
};

static struct test_value *new_value(uint64_t number, unsigned int *releases)
{
	struct test_value *value = malloc(sizeof(*value));

	if (value == NULL) {
		exit(EXIT_FAILURE);
	}
	value->number = number;
	value->releases = releases;
	return value;
}

static void free_value(void *data)
{
	struct test_value *value = data;

	(*value->releases)++;
	free(value);
}

int main(void)
{
	struct req_op_context fake = { 0 };
	struct fsal_obj_handle obj;
	struct fsal_obj_handle alias;
	struct test_value borrowed = { .number = 400 };
	struct test_value *value;
	unsigned int releases = 0;
	unsigned int before;
	unsigned int i;
	int rc = 0;

	test_init_obj(&obj, 42);
	test_init_obj(&alias, 42);
	value = new_value(100, &releases);
	attrs_set(&fake, &obj, value, free_value);
	if (attrs_get(&fake, &obj) != value || releases != 0) {
		rc = 1;
		goto out;
	}
	if (attrs_get(&fake, &alias) != value) {
		rc = 2;
		goto out;
	}

	/* The value is retained, not copied; putting it again must not free it.
	 */
	value->number = 200;
	attrs_set(&fake, &alias, value, free_value);
	if (attrs_get(&fake, &obj) != value || value->number != 200 ||
	    releases != 0) {
		rc = 3;
		goto out;
	}

	value = new_value(300, &releases);
	attrs_set(&fake, &alias, value, free_value);
	if (attrs_get(&fake, &obj) != value || releases != 1) {
		rc = 4;
		goto out;
	}
	attrs_invalidate(&fake, &alias);
	if (attrs_get(&fake, &obj) != NULL ||
	    attrs_get(&fake, &alias) != NULL || releases != 2) {
		rc = 5;
		goto out;
	}

	attrs_set(&fake, &obj, new_value(400, &releases), free_value);
	attrs_clear(&fake);
	attrs_clear(&fake);
	if (attrs_get(&fake, &obj) != NULL || releases != 3) {
		rc = 6;
		goto out;
	}

	/* Values that cannot be cached still have their cleanup callback run.
	 */
	attrs_set(NULL, &obj, new_value(500, &releases), free_value);
	attrs_set(&fake, NULL, new_value(500, &releases), free_value);
	if (releases != 5) {
		rc = 7;
		goto out;
	}

	attrs_set(&fake, &obj, &borrowed, NULL);
	if (attrs_get(&fake, &alias) != &borrowed) {
		rc = 8;
		goto out;
	}
	attrs_clear(&fake);

	/* Filling beyond eight objects evicts the oldest and keeps other keys.
	 */
	before = releases;
	attrs_set(&fake, &obj, new_value(600, &releases), free_value);
	for (i = 0; i < 8; i++) {
		test_init_obj(&alias, 100 + i);
		attrs_set(&fake, &alias, new_value(100 + i, &releases),
			      free_value);
	}
	if (attrs_get(&fake, &obj) != NULL || releases != before + 1) {
		rc = 9;
		goto out;
	}
	test_init_obj(&alias, 103);
	attrs_invalidate(&fake, &alias);
	test_init_obj(&alias, 107);
	value = attrs_get(&fake, &alias);
	if (value == NULL || value->number != 107 || releases != before + 2) {
		rc = 10;
		goto out;
	}
	attrs_clear(&fake);
	if (releases != before + 9) {
		rc = 11;
	}

out:
	attrs_clear(&fake);
	printf("test_attrs rc=%d\n", rc);
	return rc == 0 ? 0 : 1;
}
