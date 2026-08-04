// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Offline correctness for attrs_set/get/overwrite/clear.
 * Does NOT run against a live ganesha.nfsd.
 */

#include <stdio.h>
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

int main(void)
{
	struct req_op_context fake = { 0 };
	struct fsal_obj_handle obj = { 0 };
	struct fsal_obj_handle obj2 = { 0 };
	struct fsal_attrlist in = { 0 };
	struct fsal_attrlist out = { 0 };
	int rc = 0;

	test_init_obj(&obj, 42);
	test_init_obj(&obj2, 42);

	in.valid_mask = ATTR_SIZE | ATTR_CHANGE;
	in.filesize = 100;
	in.change = 7;
	attrs_set(&fake, &obj, &in);

	out.request_mask = ATTR_SIZE | ATTR_CHANGE;
	if (!attrs_get(&fake, &obj, &out) || out.filesize != 100 ||
	    out.change != 7) {
		rc = 1;
		goto out;
	}

	/* Different handle instance, same handle_to_key. */
	memset(&out, 0, sizeof(out));
	out.request_mask = ATTR_SIZE | ATTR_CHANGE;
	if (!attrs_get(&fake, &obj2, &out) || out.filesize != 100 ||
	    out.change != 7) {
		rc = 2;
		goto out;
	}

	in.filesize = 200;
	attrs_set(&fake, &obj, &in);
	memset(&out, 0, sizeof(out));
	out.request_mask = ATTR_SIZE;
	if (!attrs_get(&fake, &obj2, &out) || out.filesize != 200) {
		rc = 3;
		goto out;
	}

	memset(&out, 0, sizeof(out));
	out.request_mask = ATTR_ACL;
	if (attrs_get(&fake, &obj, &out)) {
		rc = 4;
		goto out;
	}

	attrs_clear(&fake);
	memset(&out, 0, sizeof(out));
	out.request_mask = ATTR_SIZE;
	if (attrs_get(&fake, &obj, &out)) {
		rc = 5;
	}

out:
	attrs_clear(&fake);
	printf("test_attrs rc=%d\n", rc);
	return rc == 0 ? 0 : 1;
}
