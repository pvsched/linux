// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <kunit/static_stub.h>
#include <linux/gfp.h>
#include <linux/irq_work.h>
#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/string.h>

#include "external.h"
#include "internal.h"
#include "lifecycle.h"
#include "negotiation.h"
#include "shm.h"

#define KX_RUNNER_ID	7
#define KX_VCPU_ID	3
#define KX_KVM_MODE	0x10
/* Bytes 64.. as the guest left them; pvsched must never change them. */
#define KX_PATTERN	0xa5

enum kx_op {
	KX_ATTACH,
	KX_DETACH,
	KX_RUN_ENTER,
	KX_RUN_LEAVE,
	KX_VMENTRY,
	KX_VMENTRY_CANCEL,
	KX_VMEXIT_IRQOFF,
	KX_VMEXIT,
	KX_HALT,
	KX_UNHALT,
	KX_INJECT,
	KX_NR_OPS,
};

/* What the ops saw; they may run from irq_work, so not in test->priv. */
struct kx_state {
	unsigned int calls[KX_NR_OPS];
	enum kx_op last_op;
	struct pvsched_vcpu last;
	/* What attach sets v->priv to, and the v->priv it was called with. */
	u64 priv;
	void *priv_at_attach;
	int attach_ret;
	int run_ret;
	u32 exit_reason;
	bool hlt_exit;
	bool interrupt_ready;
	bool source_guest_mode;
	/* The page status detach saw: the guest must be told first. */
	u32 status_at_detach;
	/* What detach was told about the task. */
	bool exited;
	/* Words detach writes, to show it may scrub. */
	bool scrub;
	/* Words vmentry writes. */
	u64 vmentry_words[2];
	int area_write_ret;
	/* Writes from vmexit_irqoff and inject, which may not write. */
	int irqoff_write_ret;
	int irqoff_write_u64_ret;
	int inject_write_ret;
	/* Words vmexit writes, after its read. */
	int vmexit_write_ret;
	/* What attach and vmexit read, which they may. */
	int attach_read_ret;
	u64 attach_word;
	int vmexit_read_ret;
	u64 vmexit_words[2];
};

static struct kx_state kx;

/* The guest page the next attach pins. */
static union pvsched_vcpu_page *kx_pinned_page;

struct kx_ctx {
	struct pvsched_session session;
	struct pvsched_vcpu_runner runner;
	union pvsched_vcpu_page *page;
	/* The vCPU key KVM would pass; any stable pointer. */
	int key;
};

static void kx_record(enum kx_op op, struct pvsched_vcpu *v)
{
	kx.calls[op]++;
	kx.last_op = op;
	kx.last = *v;
}

static int kx_attach(struct pvsched_vcpu *v)
{
	kx.priv_at_attach = v->priv;
	v->priv = &kx.priv;
	kx_record(KX_ATTACH, v);
	kx.attach_read_ret = pvsched_area_read_u64(v, 0, &kx.attach_word);
	return kx.attach_ret;
}

static void kx_detach(struct pvsched_vcpu *v, bool exited)
{
	u64 zero[2] = { };

	kx_record(KX_DETACH, v);
	kx.exited = exited;
	kx.status_at_detach =
		le32_to_cpu(READ_ONCE(kx_pinned_page->header.status));
	if (kx.scrub)
		kx.area_write_ret = pvsched_area_write(v, 0, zero, sizeof(zero));
}

static void kx_run_enter(struct pvsched_vcpu *v)
{
	kx_record(KX_RUN_ENTER, v);
}

static void kx_run_leave(struct pvsched_vcpu *v, int ret, u32 exit_reason)
{
	kx_record(KX_RUN_LEAVE, v);
	kx.run_ret = ret;
	kx.exit_reason = exit_reason;
}

static void kx_vmentry(struct pvsched_vcpu *v, bool interrupt_ready)
{
	kx_record(KX_VMENTRY, v);
	kx.interrupt_ready = interrupt_ready;
	/* A block write and a word write. */
	kx.area_write_ret = pvsched_area_write(v, 0, kx.vmentry_words, 8) ?:
			    pvsched_area_write_u64(v, 8, kx.vmentry_words[1]);
}

static void kx_vmentry_cancel(struct pvsched_vcpu *v)
{
	kx_record(KX_VMENTRY_CANCEL, v);
}

static void kx_vmexit_irqoff(struct pvsched_vcpu *v)
{
	u64 word = ~0ULL;

	kx_record(KX_VMEXIT_IRQOFF, v);
	kx.irqoff_write_ret = pvsched_area_write(v, 0, &word, sizeof(word));
	kx.irqoff_write_u64_ret = pvsched_area_write_u64(v, 0, word);
}

static void kx_vmexit(struct pvsched_vcpu *v, bool hlt_exit)
{
	u64 word = ~0ULL;

	kx_record(KX_VMEXIT, v);
	kx.hlt_exit = hlt_exit;
	kx.vmexit_read_ret = pvsched_area_read(v, 0, kx.vmexit_words, 8) ?:
			    pvsched_area_read_u64(v, 8, &kx.vmexit_words[1]);
	/* A block write and a word write, past the words just read. */
	kx.vmexit_write_ret = pvsched_area_write(v, 16, &word, sizeof(word)) ?:
			      pvsched_area_write_u64(v, 24, word);
}

static void kx_halt(struct pvsched_vcpu *v)
{
	kx_record(KX_HALT, v);
}

static void kx_unhalt(struct pvsched_vcpu *v)
{
	kx_record(KX_UNHALT, v);
}

static void kx_inject(struct pvsched_vcpu *v, bool source_guest_mode)
{
	kx_record(KX_INJECT, v);
	kx.source_guest_mode = source_guest_mode;
	kx.inject_write_ret = pvsched_area_write_u64(v, 0, ~0ULL);
}

static struct pvsched_external_ops kx_ops = {
	.name = "kunit-external",
	.version = 3,
	.attach = kx_attach,
	.detach = kx_detach,
	.run_enter = kx_run_enter,
	.run_leave = kx_run_leave,
	.vmentry = kx_vmentry,
	.vmentry_cancel = kx_vmentry_cancel,
	.vmexit_irqoff = kx_vmexit_irqoff,
	.vmexit = kx_vmexit,
	.halt = kx_halt,
	.unhalt = kx_unhalt,
	.inject = kx_inject,
};

static struct pvsched_policy_entry *kx_entry;

static int kx_suite_init(struct kunit_suite *suite)
{
	int ret;

	ret = pvsched_register_external_policy(&kx_ops);
	if (ret)
		return ret;
	kx_entry = pvsched_policy_lookup(kx_ops.name, kx_ops.version);
	return kx_entry ? 0 : -ENOENT;
}

static void kx_suite_exit(struct kunit_suite *suite)
{
	pvsched_unregister_external_policy(&kx_ops);
	if (kx_entry)
		pvsched_policy_entry_put(kx_entry);
}

static int kx_test_init(struct kunit *test)
{
	memset(&kx, 0, sizeof(kx));
	return 0;
}

static int kx_shm_mapping(struct mm_struct *mm, unsigned long start,
			  unsigned long end)
{
	return 0;
}

static long kx_shm_pin(struct mm_struct *mm, unsigned long user_addr,
		       struct page **page)
{
	*page = virt_to_page(kx_pinned_page);
	return 1;
}

static void kx_shm_unpin(struct page *page, bool dirty)
{
}

/* A guest page asking for the test policy, and its untouched area. */
static void kx_guest_request(union pvsched_vcpu_page *page, u32 protocol,
			     u32 mode)
{
	struct pvsched_header *header = &page->header;

	memset(page->opaque, KX_PATTERN, sizeof(page->opaque));
	memset(header, 0, sizeof(*header));
	header->abi_version = cpu_to_le32(PVSCHED_ABI_VERSION);
	header->policy_version = cpu_to_le32(kx_ops.version);
	strscpy(header->policy_name, kx_ops.name, sizeof(header->policy_name));
	header->protocol_id = cpu_to_le32(protocol);
	header->requested_mode = cpu_to_le32(mode);
	/* A sentinel shows whether the host wrote a response. */
	header->status = cpu_to_le32(0x77);
}

static bool kx_area_untouched(const union pvsched_vcpu_page *page,
			      unsigned int from)
{
	return !memchr_inv(page->opaque + from, KX_PATTERN,
			   sizeof(page->opaque) - from);
}

/* Word @i of the policy's area, as the guest sees it. */
static u64 kx_word(const union pvsched_vcpu_page *page, unsigned int i)
{
	return READ_ONCE(((const u64 *)page->opaque)[i]);
}

static void kx_teardown(void *data)
{
	struct kx_ctx *ctx = data;

	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	cancel_work_sync(&ctx->runner.cleanup_work);
	put_pid(ctx->runner.pid);
	pvsched_runner_lifecycle_destroy(&ctx->runner);
	free_page((unsigned long)ctx->page);
}

/* A session pinned to the test policy and one runner for this thread. */
static struct kx_ctx *kx_setup(struct kunit *test, u32 protocol, u32 mode)
{
	struct kx_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	/* ATTACH pins in the vCPU thread's own mm; a KUnit kthread needs one. */
	KUNIT_ASSERT_EQ(test, kunit_attach_mm(), 0);
	ctx->page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NOT_NULL(test, ctx->page);
	kx_pinned_page = ctx->page;
	kx_guest_request(ctx->page, protocol, mode);
	mutex_init(&ctx->session.lock);
	INIT_LIST_HEAD(&ctx->session.runners);
	ctx->session.owner_mm = current->mm;
	ctx->session.owner_tgid = task_tgid(current);
	ctx->session.entry = kx_entry;
	ctx->runner.pid = get_task_pid(current, PIDTYPE_PID);
	ctx->runner.runner_id = KX_RUNNER_ID;
	KUNIT_ASSERT_EQ(test, pvsched_runner_lifecycle_init(&ctx->runner,
				&ctx->session, pvsched_policy_params_size(kx_entry)), 0);
	list_add(&ctx->runner.session_node, &ctx->session.runners);
	kunit_activate_static_stub(test, pvsched_shm_mapping_admit,
				   kx_shm_mapping);
	kunit_activate_static_stub(test, pvsched_shm_pin_page, kx_shm_pin);
	kunit_activate_static_stub(test, pvsched_shm_unpin_page, kx_shm_unpin);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, kx_teardown, ctx),
			0);
	return ctx;
}

static int kx_prepare(struct kx_ctx *ctx, struct pvsched_runner_runtime **runtime,
		      enum pvsched_status *status)
{
	struct pvsched_attach_params params = {
		.user_addr = 0x100000,
		.entry = kx_entry,
		.cs_budget_ns = NSEC_PER_SEC,
		.generic_budget_ns = NSEC_PER_SEC,
		.cap_ipc_lock = true,
	};

	return pvsched_runner_prepare_attach_locked(&ctx->runner, current,
						    &params, runtime, status);
}

/* ATTACH as the ioctl does it: prepare, then publish or reject. */
static int kx_attach_ioctl(struct kx_ctx *ctx, enum pvsched_status *status)
{
	struct pvsched_runner_runtime *runtime;
	int ret;

	mutex_lock(&ctx->session.lock);
	ret = kx_prepare(ctx, &runtime, status);
	if (ret)
		goto unlock;
	if (*status != PVSCHED_STATUS_ENABLED) {
		pvsched_runner_reject_prepared_locked(runtime);
		ret = -EPROTO;
		goto unlock;
	}
	ret = pvsched_runner_publish_prepared_locked(&ctx->runner, runtime);
	if (ret)
		pvsched_runner_discard_prepared_locked(runtime);
unlock:
	mutex_unlock(&ctx->session.lock);
	return ret;
}

static struct kx_ctx *kx_attached(struct kunit *test)
{
	struct kx_ctx *ctx = kx_setup(test, PVSCHED_PROTOCOL_CUSTOM,
				      PVSCHED_MODE_POLICY);
	enum pvsched_status status;

	KUNIT_ASSERT_EQ(test, kx_attach_ioctl(ctx, &status), 0);
	return ctx;
}

static void kx_detach_ioctl(struct kx_ctx *ctx)
{
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
}

static int kx_event(struct kx_ctx *ctx, enum pvsched_reconcile_event event,
		    u32 mode, bool vmentry)
{
	struct pvsched_runner_kvm_info info = {
		.kvm_mode = KX_KVM_MODE,
		.vcpu_id = KX_VCPU_ID,
		.run_ret = -EINTR,
		.exit_reason = 9,
	};

	return pvsched_runner_local_event(&ctx->key, event, mode, vmentry,
					  true, &info);
}

static void kx_registry_test(struct kunit *test)
{
	struct pvsched_external_ops bad = kx_ops;
	struct pvsched_policy_info info = { };
	struct pvsched_policy_entry *entry;
	int keys = static_key_count(&pvsched_external_policies.key);
	u32 i;

	/* A name and at least one op. */
	bad.name[0] = '\0';
	KUNIT_EXPECT_EQ(test, pvsched_register_external_policy(&bad), -EINVAL);
	memset(&bad, 0, sizeof(bad));
	strscpy(bad.name, "kunit-no-ops", sizeof(bad.name));
	KUNIT_EXPECT_EQ(test, pvsched_register_external_policy(&bad), -EINVAL);
	/* One namespace with managed policies. */
	bad = kx_ops;
	strscpy_pad(bad.name, PVSCHED_DEFAULT_POLICY_NAME, sizeof(bad.name));
	bad.version = PVSCHED_DEFAULT_POLICY_VERSION;
	KUNIT_EXPECT_EQ(test, pvsched_register_external_policy(&bad), -EEXIST);
	KUNIT_EXPECT_EQ(test, static_key_count(&pvsched_external_policies.key),
			keys);

	/* QUERY lists it with the CUSTOM protocol and no parameters. */
	for (i = 0; !pvsched_policy_query(i, &info); i++) {
		if (!strcmp(info.name, kx_ops.name))
			break;
		memset(&info, 0, sizeof(info));
	}
	KUNIT_EXPECT_STREQ(test, info.name, kx_ops.name);
	KUNIT_EXPECT_EQ(test, info.flags, 0U);
	KUNIT_EXPECT_EQ(test, info.version, kx_ops.version);
	KUNIT_EXPECT_EQ(test, info.protocol, (u32)PVSCHED_PROTOCOL_CUSTOM);
	KUNIT_EXPECT_EQ(test, info.params_size, 0U);
	KUNIT_EXPECT_EQ(test, kx_entry->kind, PVSCHED_POLICY_EXTERNAL);
	KUNIT_EXPECT_PTR_EQ(test, kx_entry->ext_ops,
			    (const struct pvsched_external_ops *)&kx_ops);
	KUNIT_EXPECT_PTR_EQ(test, kx_ops.owner, THIS_MODULE);

	/* The key stays on while an entry is still referenced. */
	bad = kx_ops;
	strscpy_pad(bad.name, "kunit-external-2", sizeof(bad.name));
	KUNIT_ASSERT_EQ(test, pvsched_register_external_policy(&bad), 0);
	KUNIT_EXPECT_EQ(test, static_key_count(&pvsched_external_policies.key),
			keys + 1);
	entry = pvsched_policy_lookup(bad.name, bad.version);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	pvsched_unregister_external_policy(&bad);
	KUNIT_EXPECT_NULL(test, pvsched_policy_lookup(bad.name, bad.version));
	KUNIT_EXPECT_EQ(test, static_key_count(&pvsched_external_policies.key),
			keys + 1);
	pvsched_policy_entry_put(entry);
	KUNIT_EXPECT_EQ(test, static_key_count(&pvsched_external_policies.key),
			keys);
}

static void kx_negotiation_test(struct kunit *test)
{
	struct pvsched_negotiation_request request = {
		.abi_version = PVSCHED_ABI_VERSION,
		.policy_version = kx_ops.version,
		.protocol_id = PVSCHED_PROTOCOL_CUSTOM,
		.requested_mode = PVSCHED_MODE_POLICY,
	};

	strscpy(request.policy_name, kx_ops.name, sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate_external(&request, kx_entry),
			PVSCHED_STATUS_ENABLED);
	request.requested_mode = PVSCHED_MODE_FRAMEWORK;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate_external(&request, kx_entry),
			PVSCHED_STATUS_MODE_MISMATCH);
	/* The protocol is checked before the mode. */
	request.protocol_id = PVSCHED_PROTOCOL_DEFAULT;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate_external(&request, kx_entry),
			PVSCHED_STATUS_PROTOCOL_MISMATCH);
	request.policy_version++;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate_external(&request, kx_entry),
			PVSCHED_STATUS_POLICY_VERSION_MISMATCH);
	strscpy_pad(request.policy_name, PVSCHED_DEFAULT_POLICY_NAME,
		    sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate_external(&request, kx_entry),
			PVSCHED_STATUS_UNKNOWN_POLICY);
	request.abi_version++;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate_external(&request, kx_entry),
			PVSCHED_STATUS_ABI_MISMATCH);
}

static void kx_attach_detach_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	union pvsched_vcpu_page *page = ctx->page;
	struct pvsched_runner_runtime *runtime = ctx->runner.runtime;

	KUNIT_ASSERT_NOT_NULL(test, runtime);
	KUNIT_EXPECT_TRUE(test, runtime->attachment.external);
	/* The header alone answers; the policy's bytes are left as they were. */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_ENABLED);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.accepted_mode),
			(u32)PVSCHED_MODE_POLICY);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version),
			(u32)PVSCHED_ABI_VERSION);
	KUNIT_EXPECT_TRUE(test, kx_area_untouched(page, 0));

	/* attach saw the vCPU before any event. */
	KUNIT_EXPECT_EQ(test, kx.calls[KX_ATTACH], 1U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 0U);
	KUNIT_EXPECT_PTR_EQ(test, kx.last.task, current);
	/* attach may read the area. */
	KUNIT_EXPECT_EQ(test, kx.attach_read_ret, 0);
	KUNIT_EXPECT_EQ(test, kx.attach_word, 0xa5a5a5a5a5a5a5a5ULL);
	/* priv is the policy's own: NULL until attach sets it. */
	KUNIT_EXPECT_NULL(test, kx.priv_at_attach);
	KUNIT_EXPECT_EQ(test, kx.last.vcpu_id, -1);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_VMM);

	/* DETACH: the guest is told first, then detach runs once. */
	kx.scrub = true;
	kx_detach_ioctl(ctx);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	KUNIT_EXPECT_EQ(test, kx.status_at_detach, (u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_FALSE(test, kx.exited);
	KUNIT_EXPECT_PTR_EQ(test, kx.last.priv, (void *)&kx.priv);
	KUNIT_EXPECT_EQ(test, kx.area_write_ret, 0);
	/* Only what the policy scrubbed changed. */
	KUNIT_EXPECT_EQ(test, page->opaque[0], 0);
	KUNIT_EXPECT_TRUE(test, kx_area_untouched(page, 16));
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	/* Detach is final: no further op, even a second detach. */
	kx_detach_ioctl(ctx);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
}

/* A new attachment is a new attach, and priv starts NULL again. */
static void kx_reattach_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	enum pvsched_status status;

	kx_detach_ioctl(ctx);
	kx_guest_request(ctx->page, PVSCHED_PROTOCOL_CUSTOM,
			 PVSCHED_MODE_POLICY);
	KUNIT_ASSERT_EQ(test, kx_attach_ioctl(ctx, &status), 0);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_ATTACH], 2U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	KUNIT_EXPECT_NULL(test, kx.priv_at_attach);
}

static void kx_reject_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_setup(test, PVSCHED_PROTOCOL_DEFAULT,
				      PVSCHED_MODE_FRAMEWORK);
	enum pvsched_status status;

	KUNIT_EXPECT_EQ(test, kx_attach_ioctl(ctx, &status), -EPROTO);
	KUNIT_EXPECT_EQ(test, status, PVSCHED_STATUS_PROTOCOL_MISMATCH);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctx->page->header.status),
			(u32)PVSCHED_STATUS_PROTOCOL_MISMATCH);
	KUNIT_EXPECT_TRUE(test, kx_area_untouched(ctx->page, 0));
	/* A rejected attachment never reaches the policy. */
	KUNIT_EXPECT_EQ(test, kx.calls[KX_ATTACH], 0U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 0U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void kx_attach_fails_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_setup(test, PVSCHED_PROTOCOL_CUSTOM,
				      PVSCHED_MODE_POLICY);
	enum pvsched_status status;

	kx.attach_ret = -EBUSY;
	KUNIT_EXPECT_EQ(test, kx_attach_ioctl(ctx, &status), -EBUSY);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_ATTACH], 1U);
	/* A failed attach is not owed a detach, and nothing was published. */
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 0U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctx->page->header.status), 0x77U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void kx_owner_exiting(struct task_struct *task, struct mm_struct **mm,
			     struct pid **tgid, bool *exiting)
{
	*mm = task->mm;
	*tgid = task_tgid(task);
	*exiting = true;
}

/* Publication failing after attach still gets the policy its detach. */
static void kx_publish_fails_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_setup(test, PVSCHED_PROTOCOL_CUSTOM,
				      PVSCHED_MODE_POLICY);
	enum pvsched_status status;

	kunit_activate_static_stub(test, pvsched_attachment_owner_snapshot,
				   kx_owner_exiting);
	KUNIT_EXPECT_EQ(test, kx_attach_ioctl(ctx, &status), -ESRCH);
	kunit_deactivate_static_stub(test, pvsched_attachment_owner_snapshot);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_ATTACH], 1U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	/* Nothing was published, so the guest was never told anything. */
	KUNIT_EXPECT_EQ(test, kx.status_at_detach, 0x77U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void kx_local_events_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	union pvsched_vcpu_page *page = ctx->page;

	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_ENTER, 0,
				       false), 0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_RUN_ENTER);
	/* The first local event binds the vCPU and its KVM id. */
	KUNIT_EXPECT_EQ(test, kx.last.vcpu_id, KX_VCPU_ID);
	KUNIT_EXPECT_EQ(test, kx.last.kvm_mode, (u32)KX_KVM_MODE);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_HOST);

	/* VM entry is the write point; the guest sees whole words. */
	kx.vmentry_words[0] = 0x1122334455667788ULL;
	kx.vmentry_words[1] = 0x99;
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_CANCEL, 0, true),
			0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_VMENTRY);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_VMENTRY_CANCEL], 0U);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_GUEST);
	KUNIT_EXPECT_TRUE(test, kx.interrupt_ready);
	KUNIT_EXPECT_PTR_EQ(test, kx.last.priv, (void *)&kx.priv);
	KUNIT_EXPECT_EQ(test, kx.area_write_ret, 0);
	KUNIT_EXPECT_EQ(test, kx_word(page, 0), 0x1122334455667788ULL);
	KUNIT_EXPECT_EQ(test, kx_word(page, 1), 0x99ULL);
	KUNIT_EXPECT_TRUE(test, kx_area_untouched(page, 16));

	pvsched_runner_local_guest_exit_irqoff(&ctx->key);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_VMEXIT_IRQOFF);
	KUNIT_EXPECT_EQ(test, kx.last.kvm_mode, 0U);
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_VMEXIT,
				       PVSCHED_RUNNER_MODE_HLT_EXIT, false), 0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_VMEXIT);
	KUNIT_EXPECT_TRUE(test, kx.hlt_exit);
	/* Any op may read the area; vmexit may write it, vmexit_irqoff not. */
	KUNIT_EXPECT_EQ(test, kx.irqoff_write_ret, -EPERM);
	KUNIT_EXPECT_EQ(test, kx.irqoff_write_u64_ret, -EPERM);
	KUNIT_EXPECT_EQ(test, kx.vmexit_read_ret, 0);
	KUNIT_EXPECT_EQ(test, kx.vmexit_words[0], 0x1122334455667788ULL);
	KUNIT_EXPECT_EQ(test, kx.vmexit_words[1], 0x99ULL);
	KUNIT_EXPECT_EQ(test, kx.vmexit_write_ret, 0);
	KUNIT_EXPECT_EQ(test, kx_word(page, 0), 0x1122334455667788ULL);
	KUNIT_EXPECT_EQ(test, kx_word(page, 2), ~0ULL);
	KUNIT_EXPECT_EQ(test, kx_word(page, 3), ~0ULL);
	KUNIT_EXPECT_TRUE(test, kx_area_untouched(page, 32));
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_CANCEL, 0, false),
			0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_VMENTRY_CANCEL);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_VMENTRY], 1U);
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_HALT, 0, false), 0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_HALT);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_BLOCKED);
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_UNHALT, 0, false),
			0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_UNHALT);
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_LEAVE, 0,
				       false), 0);
	KUNIT_EXPECT_EQ(test, kx.last_op, KX_RUN_LEAVE);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_VMM);
	KUNIT_EXPECT_EQ(test, kx.run_ret, -EINTR);
	KUNIT_EXPECT_EQ(test, kx.exit_reason, 9U);
	/* The managed engine never ran: no budget, timer or ticket. */
	KUNIT_EXPECT_FALSE(test, hrtimer_active(&ctx->runner.runtime->cutoff_timer));
	KUNIT_EXPECT_EQ(test, ctx->runner.runtime->interrupt_ticket, 0ULL);
}

static void kx_area_bounds_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	struct pvsched_runner_runtime *runtime = ctx->runner.runtime;
	struct pvsched_vcpu *v = &runtime->ext_vcpu;
	unsigned long flags;
	const u32 last = PVSCHED_OPAQUE_SIZE - 8;
	u64 words[2] = { 1, 2 };
	void *addr;
	u64 word;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	/* Outside the ops that may write, even a valid range is refused. */
	KUNIT_EXPECT_EQ(test, pvsched_area_write(v, 0, words, 8), -EPERM);
	KUNIT_EXPECT_EQ(test, pvsched_area_write_u64(v, 0, 1), -EPERM);
	/* As inside them: outside the area, or unaligned, is refused. */
	runtime->ext_writable = true;
	KUNIT_EXPECT_EQ(test, pvsched_area_write(v, 4, words, 8), -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_area_write(v, 0, words, 12), -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_area_write(v, PVSCHED_OPAQUE_SIZE - 8,
						 words, 16), -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_area_write(v, PVSCHED_OPAQUE_SIZE,
						 words, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_area_write(v, PVSCHED_OPAQUE_SIZE - 8,
						 words, 8), 0);
	KUNIT_EXPECT_EQ(test, pvsched_area_write_u64(v, 4, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_area_write_u64(v, PVSCHED_OPAQUE_SIZE, 2),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_area_write_u64(v, last - 8, 2), 0);
	runtime->ext_writable = false;

	/* Reads take the same ranges, and zero the buffer on a refusal. */
	KUNIT_EXPECT_EQ(test, pvsched_area_read(v, PVSCHED_OPAQUE_SIZE - 16,
						words, 16), 0);
	KUNIT_EXPECT_EQ(test, words[0], 2ULL);
	KUNIT_EXPECT_EQ(test, words[1], 1ULL);
	KUNIT_EXPECT_EQ(test, pvsched_area_read_u64(v, 0, &word), 0);
	KUNIT_EXPECT_EQ(test, word, 0xa5a5a5a5a5a5a5a5ULL);
	KUNIT_EXPECT_EQ(test, pvsched_area_read(v, 4, words, 8), -EINVAL);
	KUNIT_EXPECT_EQ(test, words[0], 0ULL);
	KUNIT_EXPECT_EQ(test, pvsched_area_read(v, 0, words, 12), -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_area_read_u64(v, PVSCHED_OPAQUE_SIZE,
						    &word), -EINVAL);

	/* Without a mapped page, reads find nothing and writes are refused. */
	addr = runtime->attachment.shm.addr;
	runtime->attachment.shm.addr = NULL;
	runtime->ext_writable = true;
	word = 1;
	KUNIT_EXPECT_EQ(test, pvsched_area_read_u64(v, 0, &word), -ENOENT);
	KUNIT_EXPECT_EQ(test, word, 0ULL);
	KUNIT_EXPECT_EQ(test, pvsched_area_write_u64(v, 0, 1), -EPERM);
	runtime->ext_writable = false;
	runtime->attachment.shm.addr = addr;
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);

	KUNIT_EXPECT_EQ(test, kx_word(ctx->page, PVSCHED_OPAQUE_SIZE / 8 - 1),
			1ULL);
	KUNIT_EXPECT_EQ(test, kx_word(ctx->page, PVSCHED_OPAQUE_SIZE / 8 - 2),
			2ULL);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(ctx->page->opaque, KX_PATTERN,
					    PVSCHED_OPAQUE_SIZE - 16));
}

static void kx_inject_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	struct pvsched_runner_runtime *runtime = ctx->runner.runtime;

	/* Not yet bound: KVM's key finds nothing. */
	KUNIT_EXPECT_EQ(test, pvsched_runner_remote_inject(&ctx->key, 0, 0,
							   false), 0U);
	KUNIT_ASSERT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_LEAVE, 0,
				       false), 0);

	/* In any position, the VMM's included, with the mode at injection. */
	KUNIT_EXPECT_EQ(test, pvsched_runner_remote_inject(&ctx->key, 0,
							   KX_KVM_MODE, false),
			1U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_INJECT], 1U);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_VMM);
	KUNIT_EXPECT_EQ(test, kx.last.kvm_mode, (u32)KX_KVM_MODE);
	KUNIT_EXPECT_FALSE(test, kx.source_guest_mode);
	/* inject may not write: it may run while the guest does. */
	KUNIT_EXPECT_EQ(test, kx.inject_write_ret, -EPERM);
	KUNIT_EXPECT_TRUE(test, kx_area_untouched(ctx->page, 0));

	/* Nested is the policy's call; an unsupported mode never arrives. */
	KUNIT_EXPECT_EQ(test, pvsched_runner_remote_inject(&ctx->key,
			PVSCHED_RUNNER_MODE_NESTED, 1, false), 1U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_INJECT], 2U);
	KUNIT_EXPECT_EQ(test, kx.last.kvm_mode, 1U);
	KUNIT_EXPECT_EQ(test, pvsched_runner_remote_inject(&ctx->key,
			PVSCHED_RUNNER_MODE_UNSUPPORTED, 2, false), 0U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_INJECT], 2U);

	/* From a source in guest mode: run on the target's CPU later... */
	KUNIT_ASSERT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_ENTER, 0,
				       false), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_remote_inject(&ctx->key, 0,
							   KX_KVM_MODE, true),
			1U);
	irq_work_sync(&runtime->inject_work);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_INJECT], 3U);
	/* By then the source has left guest mode: a setter is allowed. */
	KUNIT_EXPECT_FALSE(test, kx.source_guest_mode);
	KUNIT_EXPECT_EQ(test, kx.last.kvm_mode, (u32)KX_KVM_MODE);

	/* ...but in place into a target running guest code. */
	KUNIT_ASSERT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_CANCEL, 0, true),
			0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_remote_inject(&ctx->key, 0, 0,
							   true), 1U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_INJECT], 4U);
	KUNIT_EXPECT_EQ(test, kx.last.position, (u32)PVSCHED_VCPU_GUEST);
	KUNIT_EXPECT_TRUE(test, kx.source_guest_mode);
	KUNIT_EXPECT_FALSE(test, irq_work_is_pending(&runtime->inject_work));
}

/*
 * With an external policy registered, the injection lookup no longer
 * refuses nested and unsupported targets up front; an attachment of any
 * other policy must still refuse them itself.
 */
static void kx_managed_inject_test(struct kunit *test)
{
	struct pvsched_attachment *attachment;
	int key;

	KUNIT_ASSERT_TRUE(test,
			  static_branch_unlikely(&pvsched_external_policies));
	attachment = kunit_kzalloc(test, sizeof(*attachment), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, attachment);
	KUNIT_ASSERT_EQ(test, kunit_attach_mm(), 0);
	KUNIT_ASSERT_EQ(test, pvsched_attachment_init(attachment, current), 0);
	KUNIT_ASSERT_EQ(test, pvsched_attachment_publish(attachment, NULL, NULL),
			0);
	/* Bind the key in the host, where a managed policy takes injections. */
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_local_visit(&key,
			  PVSCHED_RUNNER_HOST, 0, NULL, NULL));
	KUNIT_EXPECT_EQ(test, pvsched_attachment_remote_visit(&key, 0, NULL,
							      NULL), 1U);
	KUNIT_EXPECT_EQ(test, pvsched_attachment_remote_visit(&key,
			PVSCHED_RUNNER_MODE_NESTED, NULL, NULL), 0U);
	KUNIT_EXPECT_EQ(test, pvsched_attachment_remote_visit(&key,
			PVSCHED_RUNNER_MODE_UNSUPPORTED, NULL, NULL), 0U);
	/* Nor in the VMM. */
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_local_visit(&key,
			  PVSCHED_RUNNER_VMM, 0, NULL, NULL));
	KUNIT_EXPECT_EQ(test, pvsched_attachment_remote_visit(&key, 0, NULL,
							      NULL), 0U);
	pvsched_attachment_disable(attachment);
	pvsched_attachment_unhash(attachment);
	pvsched_attachment_drain();
	pvsched_attachment_release(attachment);
}

static void kx_unsupported_mode_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	struct pvsched_runner_runtime *runtime = ctx->runner.runtime;
	u32 state, flags;
	int fault;

	/* Keep the cleanup work from freeing the runtime under the checks. */
	mutex_lock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_ENTER,
				       PVSCHED_RUNNER_MODE_UNSUPPORTED, false),
			0);
	/* Revoked: no event op, the guest told; detach waits for teardown. */
	KUNIT_EXPECT_EQ(test, kx.calls[KX_RUN_ENTER], 0U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 0U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctx->page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_FALSE(test, runtime->attachment.active);
	KUNIT_EXPECT_TRUE(test, runtime->attachment.teardown_requested);
	/* QUERY serves the teardown, which calls detach, and reports why. */
	pvsched_runner_query_locked(&ctx->runner, &state, &flags, &fault);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	KUNIT_EXPECT_EQ(test, kx.status_at_detach, (u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_FALSE(test, kx.exited);
	KUNIT_EXPECT_EQ(test, state, (u32)PVSCHED_RUNNER_INACTIVE);
	KUNIT_EXPECT_EQ(test, flags, (u32)PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID);
	KUNIT_EXPECT_EQ(test, fault, -EOPNOTSUPP);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

/*
 * Paths that queue the runner's cleanup work hold the session mutex until
 * this thread has torn down: the work runs outside the page-pin stubs.
 */
static void kx_exit_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);

	mutex_lock(&ctx->session.lock);
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_mark_exited(current, NULL,
							       NULL));
	/* At task exit, detach waits for teardown. */
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 0U);
	/* Events of an exited vCPU never reach the policy. */
	KUNIT_EXPECT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_ENTER, 0,
				       false), -ENOENT);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_RUN_ENTER], 0U);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	/* Told the task exited, so the policy leaves its scheduling alone. */
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	KUNIT_EXPECT_TRUE(test, kx.exited);
}

static void kx_binding_failure_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);
	int other_key;

	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, kx_event(ctx, PVSCHED_RECONCILE_RUN_ENTER, 0,
				       false), 0);
	/* Another vCPU key on the same thread fails the binding for good. */
	pvsched_runner_local_event(&other_key, PVSCHED_RECONCILE_VMEXIT, 0,
				   false, false, NULL);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_VMEXIT], 0U);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 0U);
	KUNIT_EXPECT_TRUE(test, ctx->runner.runtime->attachment.binding_failed);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	KUNIT_EXPECT_EQ(test, kx.status_at_detach, (u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_FALSE(test, kx.exited);
}

/* Final session close disables every runtime first, then detaches each. */
static void kx_session_close_test(struct kunit *test)
{
	struct kx_ctx *ctx = kx_attached(test);

	ctx->session.closing = true;
	pvsched_session_release_runtimes(&ctx->session);
	KUNIT_EXPECT_EQ(test, kx.calls[KX_DETACH], 1U);
	KUNIT_EXPECT_EQ(test, kx.status_at_detach, (u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static struct kunit_case kx_test_cases[] = {
	KUNIT_CASE(kx_registry_test),
	KUNIT_CASE(kx_negotiation_test),
	KUNIT_CASE(kx_attach_detach_test),
	KUNIT_CASE(kx_reattach_test),
	KUNIT_CASE(kx_reject_test),
	KUNIT_CASE(kx_attach_fails_test),
	KUNIT_CASE(kx_publish_fails_test),
	KUNIT_CASE(kx_local_events_test),
	KUNIT_CASE(kx_area_bounds_test),
	KUNIT_CASE(kx_inject_test),
	KUNIT_CASE(kx_managed_inject_test),
	KUNIT_CASE(kx_unsupported_mode_test),
	KUNIT_CASE(kx_exit_test),
	KUNIT_CASE(kx_binding_failure_test),
	KUNIT_CASE(kx_session_close_test),
	{}
};

static struct kunit_suite kx_test_suite = {
	.name = "pvsched-external",
	.suite_init = kx_suite_init,
	.suite_exit = kx_suite_exit,
	.init = kx_test_init,
	.test_cases = kx_test_cases,
};
kunit_test_suite(kx_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched external policies");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
