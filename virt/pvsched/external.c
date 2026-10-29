// SPDX-License-Identifier: GPL-2.0-only

/*
 * External policies: pvsched keeps the attachment, its lookups and checks,
 * and calls the policy's ops with the vCPU's context.  It never reads or
 * writes the policy's part of the page itself.
 */

#include <linux/build_bug.h>
#include <linux/export.h>
#include <linux/printk.h>
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

void pvsched_external_local_event_locked(struct pvsched_runner_runtime *runtime,
					 enum pvsched_attachment_visit_kind kind,
					 u32 mode_flags)
{
	/* Cleanup and binding-failure visits have nothing to call. */
	if (kind != PVSCHED_ATTACHMENT_VISIT_ORDINARY)
		return;
	/* Revocation only, until the policy's event ops are plugged in. */
	if (mode_flags & PVSCHED_RUNNER_MODE_UNSUPPORTED)
		pvsched_external_revoke_locked(runtime);
}
