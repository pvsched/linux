// SPDX-License-Identifier: GPL-2.0-only
/*
 * Drive the public ATTACH_SHM/DETACH_SHM ioctls through the real control
 * entry point: an anonymous-inode file on pvsched_fops, a KUnit address
 * space standing in for the VMM's, and a real anonymous guest page.
 */

#include <kunit/test.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <uapi/linux/pvsched.h>

#include "default_policy.h"
#include "internal.h"
#include "policy.h"

static const struct file_operations pvsched_core_test_no_fops;

struct pvsched_core_test_ctx {
	struct file *file;
	/* Ioctl argument buffer, then the guest page, in user memory. */
	unsigned long args;
	unsigned long page;
	u64 runner_id;
	/* The policy the session selected and the guest asks for. */
	const char *policy;
	u32 version;
};

static int pvsched_core_test_captures;

/* The default policy under another name, counting its baseline captures. */
static int pvsched_core_test_capture(struct pvsched_policy_ctx *ctx,
				     void *baseline)
{
	pvsched_core_test_captures++;
	return pvsched_default_policy_ops.capture_baseline(ctx, baseline);
}

static struct pvsched_policy_ops pvsched_core_test_ops = {
	.name = "kunit-control",
	.version = 2,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = sizeof(struct pvsched_default_params),
	.priv_size = sizeof(struct pvsched_default_priv),
	.capture_baseline = pvsched_core_test_capture,
};

static int pvsched_core_suite_init(struct kunit_suite *suite)
{
	pvsched_core_test_ops.map = pvsched_default_policy_ops.map;
	pvsched_core_test_ops.apply = pvsched_default_policy_ops.apply;
	pvsched_core_test_ops.owned = pvsched_default_policy_ops.owned;
	return pvsched_register_policy(&pvsched_core_test_ops);
}

static void pvsched_core_suite_exit(struct kunit_suite *suite)
{
	pvsched_unregister_policy(&pvsched_core_test_ops);
}

static void pvsched_core_test_close(void *data)
{
	struct pvsched_core_test_ctx *ctx = data;

	if (ctx->file)
		__fput_sync(ctx->file);
	ctx->file = NULL;
}

/* Close the session now, running final close on this thread. */
static void pvsched_core_test_final_close(struct kunit *test,
					  struct pvsched_core_test_ctx *ctx)
{
	kunit_release_action(test, pvsched_core_test_close, ctx);
}

static void pvsched_core_test_write_request(struct kunit *test,
					    struct pvsched_core_test_ctx *ctx,
					    u32 requested_mode)
{
	struct pvsched_header header = {
		.abi_version = cpu_to_le32(PVSCHED_ABI_VERSION),
		.policy_version = cpu_to_le32(ctx->version),
		.protocol_id = cpu_to_le32(PVSCHED_PROTOCOL_DEFAULT),
		.requested_mode = cpu_to_le32(requested_mode),
	};

	strscpy(header.policy_name, ctx->policy, sizeof(header.policy_name));
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)ctx->page, &header,
					   sizeof(header)), 0);
}

static u32 pvsched_core_test_page_status(struct kunit *test,
					 struct pvsched_core_test_ctx *ctx)
{
	struct pvsched_header header;

	KUNIT_EXPECT_EQ(test, copy_from_user(&header,
					     (void __user *)ctx->page,
					     sizeof(header)), 0);
	return le32_to_cpu(header.status);
}

/* Copy @arg in, issue @cmd on the session, and copy the result back. */
static long pvsched_core_test_ioctl(struct kunit *test,
				    struct pvsched_core_test_ctx *ctx,
				    unsigned int cmd, void *arg, size_t size)
{
	void __user *uarg = (void __user *)ctx->args;
	long ret;

	KUNIT_ASSERT_EQ(test, copy_to_user(uarg, arg, size), 0);
	ret = ctx->file->f_op->unlocked_ioctl(ctx->file, cmd,
					      (unsigned long)uarg);
	KUNIT_ASSERT_EQ(test, copy_from_user(arg, uarg, size), 0);
	return ret;
}

static long pvsched_core_test_attach(struct kunit *test,
				     struct pvsched_core_test_ctx *ctx,
				     struct pvsched_attach_shm *attach)
{
	*attach = (struct pvsched_attach_shm) {
		.runner_id = ctx->runner_id,
		.user_addr = ctx->page,
		.size = PVSCHED_VCPU_STRIDE,
	};
	return pvsched_core_test_ioctl(test, ctx, PVSCHED_ATTACH_SHM, attach,
				       sizeof(*attach));
}

static long pvsched_core_test_detach(struct kunit *test,
				     struct pvsched_core_test_ctx *ctx,
				     u64 runner_id)
{
	struct pvsched_detach_shm detach = { .runner_id = runner_id };

	return pvsched_core_test_ioctl(test, ctx, PVSCHED_DETACH_SHM, &detach,
				       sizeof(detach));
}

static u32 pvsched_core_test_state(struct kunit *test,
				   struct pvsched_core_test_ctx *ctx)
{
	struct pvsched_query_runner query = { .runner_id = ctx->runner_id };

	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_RUNNER, &query, sizeof(query)), 0);
	return query.state;
}

static long pvsched_core_test_set_policy(struct kunit *test,
					 struct pvsched_core_test_ctx *ctx,
					 const char *name, u32 version)
{
	struct pvsched_set_policy set = { .version = version };

	strscpy(set.name, name, sizeof(set.name));
	return pvsched_core_test_ioctl(test, ctx, PVSCHED_SET_POLICY, &set,
				       sizeof(set));
}

/* Open a session with no policy selected and no runner. */
static struct pvsched_core_test_ctx *pvsched_core_test_open(struct kunit *test)
{
	struct pvsched_core_test_ctx *ctx;
	struct file *file;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	/* The session owner is this thread and its (KUnit) address space. */
	ctx->args = kunit_vm_mmap(test, NULL, 0, 2 * PAGE_SIZE,
				  PROT_READ | PROT_WRITE,
				  MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(test, ctx->args, 0UL, "user memory unavailable");
	ctx->page = ctx->args + PAGE_SIZE;

	file = anon_inode_getfile("[pvsched-test]", &pvsched_fops, NULL,
				  O_RDWR);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	ret = pvsched_fops.open(file_inode(file), file);
	if (ret) {
		/* No session exists, so pvsched_release() must not run. */
		replace_fops(file, &pvsched_core_test_no_fops);
		fput(file);
		KUNIT_FAIL(test, "pvsched open failed: %d", ret);
		return NULL;
	}
	ctx->file = file;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			pvsched_core_test_close, ctx), 0);
	return ctx;
}

/* A session on @policy with the KUnit thread as its runner, as a VMM sets up. */
static struct pvsched_core_test_ctx *
pvsched_core_test_setup_policy(struct kunit *test, const char *policy,
			       u32 version)
{
	struct pvsched_core_test_ctx *ctx = pvsched_core_test_open(test);

	if (!ctx)
		return NULL;
	ctx->policy = policy;
	ctx->version = version;
	KUNIT_ASSERT_EQ(test, pvsched_core_test_set_policy(test, ctx, policy,
							   version), 0);
	KUNIT_ASSERT_EQ(test, pvsched_session_add_current_runner(
			ctx->file->private_data, &ctx->runner_id), 0);
	pvsched_core_test_write_request(test, ctx, PVSCHED_MODE_FRAMEWORK);
	return ctx;
}

static struct pvsched_core_test_ctx *pvsched_core_test_setup(struct kunit *test)
{
	return pvsched_core_test_setup_policy(test, PVSCHED_DEFAULT_POLICY_NAME,
					      PVSCHED_DEFAULT_POLICY_VERSION);
}

static void pvsched_core_attach_detach_test(struct kunit *test)
{
	struct pvsched_core_test_ctx *ctx = pvsched_core_test_setup(test);
	struct pvsched_attach_shm attach;

	if (!ctx)
		return;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach), 0);
	KUNIT_EXPECT_EQ(test, attach.negotiation_status,
			(u32)PVSCHED_STATUS_ENABLED);
	KUNIT_EXPECT_EQ(test, attach.runner_id, ctx->runner_id);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_page_status(test, ctx),
			(u32)PVSCHED_STATUS_ENABLED);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_state(test, ctx),
			(u32)PVSCHED_RUNNER_ACTIVE);

	/* A second ATTACH of an attached runner is refused. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach),
			-EEXIST);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_state(test, ctx),
			(u32)PVSCHED_RUNNER_ACTIVE);

	/* DETACH tells the guest and is idempotent. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_detach(test, ctx,
						       ctx->runner_id), 0);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_page_status(test, ctx),
			(u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_state(test, ctx),
			(u32)PVSCHED_RUNNER_INACTIVE);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_detach(test, ctx,
						       ctx->runner_id), 0);

	/* The same runner and page can be attached again. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach), 0);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_page_status(test, ctx),
			(u32)PVSCHED_STATUS_ENABLED);

	/* Final close of an attached session disables and releases it. */
	pvsched_core_test_final_close(test, ctx);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_page_status(test, ctx),
			(u32)PVSCHED_STATUS_DISABLED);
}

static void pvsched_core_reject_test(struct kunit *test)
{
	struct pvsched_core_test_ctx *ctx = pvsched_core_test_setup(test);
	struct pvsched_attach_shm attach;

	if (!ctx)
		return;
	pvsched_core_test_write_request(test, ctx, PVSCHED_MODE_POLICY);
	/* A completed rejection is the one error with valid output. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, attach.negotiation_status,
			(u32)PVSCHED_STATUS_MODE_MISMATCH);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_page_status(test, ctx),
			(u32)PVSCHED_STATUS_MODE_MISMATCH);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_state(test, ctx),
			(u32)PVSCHED_RUNNER_INACTIVE);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_detach(test, ctx,
						       ctx->runner_id), 0);

	/* No attachment remains: a corrected request then succeeds. */
	pvsched_core_test_write_request(test, ctx, PVSCHED_MODE_FRAMEWORK);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach), 0);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_detach(test, ctx,
						       ctx->runner_id), 0);
}

static void pvsched_core_input_errors_test(struct kunit *test)
{
	struct pvsched_core_test_ctx *ctx = pvsched_core_test_setup(test);
	struct pvsched_attach_shm attach, valid = { };
	struct pvsched_detach_shm detach = { };
	long ret;

	if (!ctx)
		return;
	valid = (struct pvsched_attach_shm) {
		.runner_id = ctx->runner_id,
		.user_addr = ctx->page,
		.size = PVSCHED_VCPU_STRIDE,
	};

	attach = valid;
	attach.runner_id = 0;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -EINVAL);
	attach = valid;
	attach.flags = 1;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -EINVAL);
	attach = valid;
	attach.negotiation_status = PVSCHED_STATUS_ENABLED;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -EINVAL);
	attach = valid;
	attach.size = PAGE_SIZE / 2;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -EINVAL);
	attach = valid;
	attach.user_addr += 8;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -EINVAL);
	attach = valid;
	attach.user_addr = U64_MAX & PAGE_MASK;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -EINVAL);

	/* Shape errors precede lookup; an unknown runner is ENOENT. */
	attach = valid;
	attach.runner_id = ctx->runner_id + 1;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_ATTACH_SHM, &attach, sizeof(attach)), -ENOENT);
	detach.runner_id = ctx->runner_id + 1;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_DETACH_SHM, &detach, sizeof(detach)), -ENOENT);
	detach = (struct pvsched_detach_shm) { };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_DETACH_SHM, &detach, sizeof(detach)), -EINVAL);
	detach.runner_id = ctx->runner_id;
	detach.reserved[2] = 1;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_DETACH_SHM, &detach, sizeof(detach)), -EINVAL);

	/* An unmapped guest address is a fault and leaves nothing attached. */
	attach = valid;
	attach.user_addr = ctx->args + 16 * PAGE_SIZE;
	ret = pvsched_core_test_ioctl(test, ctx, PVSCHED_ATTACH_SHM, &attach,
				      sizeof(attach));
	KUNIT_EXPECT_EQ(test, ret, -EFAULT);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_state(test, ctx),
			(u32)PVSCHED_RUNNER_INACTIVE);
}

/* Whether QUERY_POLICY lists @name at @version. */
static bool pvsched_core_test_listed(struct kunit *test,
				     struct pvsched_core_test_ctx *ctx,
				     const char *name, u32 version)
{
	struct pvsched_policy_info info;
	u32 index;

	for (index = 0; index < 64; index++) {
		info = (struct pvsched_policy_info) { .index = index };
		if (pvsched_core_test_ioctl(test, ctx, PVSCHED_QUERY_POLICY,
					    &info, sizeof(info)))
			return false;
		if (!strcmp(info.name, name) && info.version == version)
			return true;
	}
	return false;
}

static void pvsched_core_query_policy_test(struct kunit *test)
{
	struct pvsched_core_test_ctx *ctx = pvsched_core_test_open(test);
	struct pvsched_policy_info info = { };
	u32 index;

	if (!ctx)
		return;
	/* The built-in default is listed first. */
	KUNIT_ASSERT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), 0);
	KUNIT_EXPECT_STREQ(test, info.name, PVSCHED_DEFAULT_POLICY_NAME);
	KUNIT_EXPECT_EQ(test, info.version, PVSCHED_DEFAULT_POLICY_VERSION);
	KUNIT_EXPECT_EQ(test, info.protocol, (u32)PVSCHED_PROTOCOL_DEFAULT);
	KUNIT_EXPECT_EQ(test, info.params_size,
			(u32)sizeof(struct pvsched_default_params));
	KUNIT_EXPECT_EQ(test, info.flags, 0U);
	KUNIT_EXPECT_EQ(test, info.reserved, 0U);
	KUNIT_EXPECT_TRUE(test, pvsched_core_test_listed(test, ctx,
							 "kunit-control", 2));

	/* Every input field but the index must be zero. */
	info = (struct pvsched_policy_info) { .flags = 1 };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -EINVAL);
	info = (struct pvsched_policy_info) { .name[PVSCHED_NAME_MAX - 1] = 1 };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -EINVAL);
	info = (struct pvsched_policy_info) { .version = 1 };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -EINVAL);
	info = (struct pvsched_policy_info) { .protocol = 1 };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -EINVAL);
	info = (struct pvsched_policy_info) { .params_size = 1 };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -EINVAL);
	info = (struct pvsched_policy_info) { .reserved = 1 };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -EINVAL);

	/* The list ends with ENOENT. */
	for (index = 0; index < 64; index++) {
		info = (struct pvsched_policy_info) { .index = index };
		if (pvsched_core_test_ioctl(test, ctx, PVSCHED_QUERY_POLICY,
					    &info, sizeof(info)))
			break;
	}
	KUNIT_EXPECT_GE(test, index, 2U);
	KUNIT_EXPECT_LT(test, index, 64U);
	info = (struct pvsched_policy_info) { .index = U32_MAX };
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_QUERY_POLICY, &info, sizeof(info)), -ENOENT);
}

static void pvsched_core_set_policy_test(struct kunit *test)
{
	struct pvsched_core_test_ctx *ctx = pvsched_core_test_open(test);
	struct pvsched_create_runner create = { .tid = current->pid };
	struct pvsched_set_policy set = { };
	u64 runner_id;

	if (!ctx)
		return;
	/* No runner before the session has a policy. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_CREATE_RUNNER, &create, sizeof(create)), -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_session_add_current_runner(
			ctx->file->private_data, &runner_id), -EINVAL);

	/* Malformed requests. */
	set.version = 1;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_SET_POLICY, &set, sizeof(set)), -EINVAL);
	memset(set.name, 'a', sizeof(set.name));
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_SET_POLICY, &set, sizeof(set)), -EINVAL);
	memset(set.name, 0, sizeof(set.name));
	strscpy(set.name, PVSCHED_DEFAULT_POLICY_NAME, sizeof(set.name));
	set.name[sizeof(set.name) - 1] = 'x';
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_SET_POLICY, &set, sizeof(set)), -EINVAL);
	set.name[sizeof(set.name) - 1] = 0;
	set.flags = 1;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_ioctl(test, ctx,
			PVSCHED_SET_POLICY, &set, sizeof(set)), -EINVAL);

	/* Unknown names and versions. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, ctx,
			"no-such-policy", 1), -ENOENT);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, ctx,
			PVSCHED_DEFAULT_POLICY_NAME, 2), -ENOENT);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, ctx,
			"kunit-control", 1), -ENOENT);

	/* Exactly once. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, ctx,
			"kunit-control", 2), 0);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, ctx,
			"kunit-control", 2), -EBUSY);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, ctx,
			PVSCHED_DEFAULT_POLICY_NAME,
			PVSCHED_DEFAULT_POLICY_VERSION), -EBUSY);
	KUNIT_EXPECT_EQ(test, pvsched_session_add_current_runner(
			ctx->file->private_data, &runner_id), 0);
}

/* A session pins its policy; a guest may only negotiate for that one. */
static void pvsched_core_pin_test(struct kunit *test)
{
	struct pvsched_policy_entry *entry;
	struct pvsched_core_test_ctx *ctx;
	struct pvsched_attach_shm attach;
	unsigned int refs;

	entry = pvsched_policy_lookup("kunit-control", 2);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	refs = refcount_read(&entry->ref);
	ctx = pvsched_core_test_setup_policy(test, "kunit-control", 2);
	if (!ctx)
		goto out;
	KUNIT_EXPECT_EQ(test, refcount_read(&entry->ref), refs + 1);
	KUNIT_EXPECT_PTR_EQ(test, ((struct pvsched_session *)
			    ctx->file->private_data)->entry, entry);

	/* The default policy is not a fallback for this session. */
	ctx->policy = PVSCHED_DEFAULT_POLICY_NAME;
	ctx->version = PVSCHED_DEFAULT_POLICY_VERSION;
	pvsched_core_test_write_request(test, ctx, PVSCHED_MODE_FRAMEWORK);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, attach.negotiation_status,
			(u32)PVSCHED_STATUS_UNKNOWN_POLICY);

	ctx->policy = "kunit-control";
	ctx->version = 2;
	pvsched_core_test_write_request(test, ctx, PVSCHED_MODE_FRAMEWORK);
	pvsched_core_test_captures = 0;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach), 0);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_captures, 1);
	/* The attachment holds its own reference while it lives. */
	KUNIT_EXPECT_EQ(test, refcount_read(&entry->ref), refs + 2);

	/* Close releases every reference the session took. */
	pvsched_core_test_final_close(test, ctx);
	KUNIT_EXPECT_EQ(test, refcount_read(&entry->ref), refs);
out:
	pvsched_policy_entry_put(entry);
}

static int pvsched_core_test_gone_captures;

static int pvsched_core_test_gone_capture(struct pvsched_policy_ctx *ctx,
					  void *baseline)
{
	pvsched_core_test_gone_captures++;
	return pvsched_default_policy_ops.capture_baseline(ctx, baseline);
}

/* Unregistering unlists a policy; its sessions keep it until they close. */
static void pvsched_core_unregister_test(struct kunit *test)
{
	/* Static: sessions closed by test cleanup may still call it. */
	static struct pvsched_policy_ops ops;
	struct pvsched_core_test_ctx *ctx, *other;
	struct pvsched_policy_entry *entry;
	struct pvsched_attach_shm attach;

	ops = pvsched_core_test_ops;
	strscpy_pad(ops.name, "kunit-control-gone", sizeof(ops.name));
	ops.version = 1;
	ops.capture_baseline = pvsched_core_test_gone_capture;
	KUNIT_ASSERT_EQ(test, pvsched_register_policy(&ops), 0);
	entry = pvsched_policy_lookup("kunit-control-gone", 1);
	if (!entry) {
		pvsched_unregister_policy(&ops);
		KUNIT_FAIL(test, "registered policy not found");
		return;
	}
	ctx = pvsched_core_test_setup_policy(test, "kunit-control-gone", 1);
	if (!ctx)
		goto out;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach), 0);
	pvsched_unregister_policy(&ops);

	/* No longer listed or selectable... */
	KUNIT_EXPECT_FALSE(test, pvsched_core_test_listed(test, ctx,
			   "kunit-control-gone", 1));
	other = pvsched_core_test_open(test);
	if (other)
		KUNIT_EXPECT_EQ(test, pvsched_core_test_set_policy(test, other,
				"kunit-control-gone", 1), -ENOENT);

	/* ...but the session that selected it keeps calling it. */
	KUNIT_EXPECT_EQ(test, pvsched_core_test_detach(test, ctx,
						       ctx->runner_id), 0);
	pvsched_core_test_gone_captures = 0;
	KUNIT_EXPECT_EQ(test, pvsched_core_test_attach(test, ctx, &attach), 0);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_gone_captures, 1);
	KUNIT_EXPECT_EQ(test, pvsched_core_test_state(test, ctx),
			(u32)PVSCHED_RUNNER_ACTIVE);

	/* The last session's close drops the last reference but ours. */
	pvsched_core_test_final_close(test, ctx);
	KUNIT_EXPECT_EQ(test, refcount_read(&entry->ref), 1U);
	KUNIT_EXPECT_TRUE(test, entry->dead);
out:
	pvsched_unregister_policy(&ops);
	pvsched_policy_entry_put(entry);
}

static struct kunit_case pvsched_core_test_cases[] = {
	KUNIT_CASE(pvsched_core_attach_detach_test),
	KUNIT_CASE(pvsched_core_reject_test),
	KUNIT_CASE(pvsched_core_input_errors_test),
	KUNIT_CASE(pvsched_core_query_policy_test),
	KUNIT_CASE(pvsched_core_set_policy_test),
	KUNIT_CASE(pvsched_core_pin_test),
	KUNIT_CASE(pvsched_core_unregister_test),
	{}
};

static struct kunit_suite pvsched_core_test_suite = {
	.name = "pvsched-control",
	.suite_init = pvsched_core_suite_init,
	.suite_exit = pvsched_core_suite_exit,
	.test_cases = pvsched_core_test_cases,
};

kunit_test_suite(pvsched_core_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched control ioctl dispatch");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
