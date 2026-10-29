// SPDX-License-Identifier: GPL-2.0-only

/*
 * External policies: pvsched keeps the attachment, its lookups and checks,
 * and calls the policy's ops with the vCPU's context.  It never reads or
 * writes the policy's part of the page itself.
 */

#include <linux/build_bug.h>
#include <linux/export.h>
#include <linux/printk.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include <kunit/visibility.h>

#include "external.h"
#include "internal.h"
#include "lifecycle.h"

void pvsched_external_prepare(struct pvsched_runner_runtime *runtime,
			      struct pvsched_policy_entry *entry)
{
	pvsched_policy_entry_get(entry);
	runtime->entry = entry;
	runtime->ext_ops = entry->ext_ops;
	runtime->attachment.external = true;
}

int pvsched_external_attach(struct pvsched_runner_runtime *runtime,
			    struct pvsched_vcpu_runner *runner)
{
	struct pvsched_attachment *attachment = &runtime->attachment;
	struct pvsched_vcpu *v = &runtime->ext_vcpu;
	int ret;

	lockdep_assert_held(&runner->session->lock);
	*v = (struct pvsched_vcpu) {
		.task = attachment->task,
		.vcpu_id = -1,
		.position = attachment->position,
	};
	if (runtime->ext_ops->attach) {
		ret = runtime->ext_ops->attach(v);
		/*
		 * The ioctl returns this.  EPROTO would read as a rejected
		 * negotiation, and a positive value as success.
		 */
		if (WARN_ON_ONCE(ret > 0) || ret == -EPROTO)
			ret = -EINVAL;
		if (ret)
			return ret;
	}
	runtime->ext_attached = true;
	return 0;
}

void pvsched_external_detach(struct pvsched_runner_runtime *runtime)
{
	struct pvsched_attachment *attachment = &runtime->attachment;
	struct pvsched_vcpu *v = &runtime->ext_vcpu;
	unsigned long flags;
	bool exited;

	raw_spin_lock_irqsave(&attachment->state_lock, flags);
	if (runtime->ext_attached && runtime->ext_ops->detach) {
		/* As for a managed restore: an exiting task is left alone. */
		exited = attachment->exited ||
			 (READ_ONCE(attachment->task->flags) & PF_EXITING);
		v->kvm_mode = 0;
		v->position = attachment->position;
		/* The page stays pinned until the runtime is freed. */
		runtime->ext_writable = true;
		runtime->ext_ops->detach(v, exited);
		runtime->ext_writable = false;
	}
	runtime->ext_attached = false;
	raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
}

/* As for a managed policy: stop, tell the VMM through QUERY, tear down. */
static void pvsched_external_revoke_locked(struct pvsched_runner_runtime *runtime)
{
	pr_warn_ratelimited("pvsched: runner %d: unsupported guest execution mode, revoking the attachment\n",
			    task_pid_nr(runtime->attachment.task));
	runtime->last_fault = -EOPNOTSUPP;
	pvsched_attachment_disable_locked(&runtime->attachment);
	pvsched_runner_request_cleanup_locked(&runtime->attachment);
}

/* The fields every event op sees fresh; the rest is fixed at attach. */
static struct pvsched_vcpu *
pvsched_external_vcpu(struct pvsched_runner_runtime *runtime, u32 kvm_mode)
{
	struct pvsched_vcpu *v = &runtime->ext_vcpu;

	v->kvm_mode = kvm_mode;
	v->position = runtime->attachment.position;
	return v;
}

void pvsched_external_local_event_locked(struct pvsched_runner_runtime *runtime,
					 enum pvsched_attachment_visit_kind kind,
					 const struct pvsched_external_event *event)
{
	const struct pvsched_external_ops *ops = runtime->ext_ops;
	const struct pvsched_runner_kvm_info *info = event->kvm;
	struct pvsched_vcpu *v;

	/* Cleanup and binding-failure visits have nothing to call. */
	if (kind != PVSCHED_ATTACHMENT_VISIT_ORDINARY)
		return;
	if (event->mode_flags & PVSCHED_RUNNER_MODE_UNSUPPORTED) {
		pvsched_external_revoke_locked(runtime);
		return;
	}
	v = pvsched_external_vcpu(runtime, info ? info->kvm_mode : 0);
	/* An ordinary visit has bound the vCPU key, which never changes. */
	if (info && v->vcpu_id < 0)
		v->vcpu_id = info->vcpu_id;

	switch (event->event) {
	case PVSCHED_EXTERNAL_RECONCILE_RUN_ENTER:
		if (ops->run_enter)
			ops->run_enter(v);
		break;
	case PVSCHED_EXTERNAL_RECONCILE_RUN_LEAVE:
		if (ops->run_leave)
			ops->run_leave(v, info ? info->run_ret : 0,
				       info ? info->exit_reason : 0);
		break;
	case PVSCHED_EXTERNAL_RECONCILE_VMEXIT:
		if (ops->vmexit) {
			runtime->ext_writable = true;
			ops->vmexit(v, event->mode_flags &
				    PVSCHED_RUNNER_MODE_HLT_EXIT);
			runtime->ext_writable = false;
		}
		break;
	case PVSCHED_EXTERNAL_RECONCILE_VMENTRY:
		if (ops->vmentry) {
			runtime->ext_writable = true;
			ops->vmentry(v, event->interrupt_ready);
			runtime->ext_writable = false;
		}
		break;
	case PVSCHED_EXTERNAL_RECONCILE_CANCEL:
		if (ops->vmentry_cancel)
			ops->vmentry_cancel(v);
		break;
	case PVSCHED_EXTERNAL_RECONCILE_VMEXIT_IRQOFF:
		if (ops->vmexit_irqoff)
			ops->vmexit_irqoff(v);
		break;
	case PVSCHED_EXTERNAL_RECONCILE_HALT:
		if (ops->halt)
			ops->halt(v);
		break;
	case PVSCHED_EXTERNAL_RECONCILE_UNHALT:
		if (ops->unhalt)
			ops->unhalt(v);
		break;
	}
}

bool pvsched_external_wants_inject(const struct pvsched_runner_runtime *runtime)
{
	return runtime->ext_ops->inject;
}

void pvsched_external_inject_locked(struct pvsched_runner_runtime *runtime,
				    u32 kvm_mode, bool source_guest_mode)
{
	if (runtime->ext_ops->inject)
		runtime->ext_ops->inject(pvsched_external_vcpu(runtime, kvm_mode),
					 source_guest_mode);
}

/* The runtime behind the @v an op was called with. */
static struct pvsched_runner_runtime *
pvsched_area_runtime(struct pvsched_vcpu *v)
{
	return container_of(v, struct pvsched_runner_runtime, ext_vcpu);
}

static bool pvsched_area_range_ok(u32 off, u32 size)
{
	return IS_ALIGNED(off, sizeof(u64)) && IS_ALIGNED(size, sizeof(u64)) &&
	       size <= PVSCHED_OPAQUE_SIZE && off <= PVSCHED_OPAQUE_SIZE - size;
}

int pvsched_area_write(struct pvsched_vcpu *v, u32 off, const void *buf,
		       u32 size)
{
	struct pvsched_runner_runtime *runtime = pvsched_area_runtime(v);
	u64 *dst;
	u64 word;
	u32 i;

	lockdep_assert_held(&runtime->attachment.state_lock);
	/* Only vmexit, vmentry and detach may write, to a mapped page. */
	if (!runtime->ext_writable || !runtime->attachment.shm.addr)
		return -EPERM;
	if (!pvsched_area_range_ok(off, size))
		return -EINVAL;
	dst = runtime->attachment.shm.addr + PVSCHED_OPAQUE_OFFSET + off;
	/* Whole aligned words, each single-copy atomic for the guest. */
	for (i = 0; i < size / sizeof(u64); i++) {
		memcpy(&word, buf + i * sizeof(u64), sizeof(word));
		WRITE_ONCE(dst[i], word);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(pvsched_area_write);

int pvsched_area_write_u64(struct pvsched_vcpu *v, u32 off, u64 val)
{
	return pvsched_area_write(v, off, &val, sizeof(val));
}
EXPORT_SYMBOL_GPL(pvsched_area_write_u64);

/*
 * No lock is needed: the page stays mapped while ops can run, and attach
 * reads it under the session mutex instead of the state lock.
 */
int pvsched_area_read(struct pvsched_vcpu *v, u32 off, void *buf, u32 size)
{
	struct pvsched_runner_runtime *runtime = pvsched_area_runtime(v);
	const u64 *src;
	u64 word;
	u32 i;

	if (!pvsched_area_range_ok(off, size)) {
		memset(buf, 0, size);
		return -EINVAL;
	}
	if (!runtime->attachment.shm.addr) {
		memset(buf, 0, size);
		return -ENOENT;
	}
	src = runtime->attachment.shm.addr + PVSCHED_OPAQUE_OFFSET + off;
	for (i = 0; i < size / sizeof(u64); i++) {
		word = READ_ONCE(src[i]);
		memcpy(buf + i * sizeof(u64), &word, sizeof(word));
	}
	return 0;
}
EXPORT_SYMBOL_GPL(pvsched_area_read);

int pvsched_area_read_u64(struct pvsched_vcpu *v, u32 off, u64 *val)
{
	return pvsched_area_read(v, off, val, sizeof(*val));
}
EXPORT_SYMBOL_GPL(pvsched_area_read_u64);
