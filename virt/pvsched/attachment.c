// SPDX-License-Identifier: GPL-2.0-only

/*
 * An attachment holds the common identity, admission and target position for
 * one vCPU thread.  Target-local hooks and task exit find it by exact task;
 * remote injection finds it by an opaque vCPU pointer.  These private hashes
 * avoid numeric PID lookup and KVM's pid_lock in IRQ-off hook contexts.
 *
 * The lifetime order is init, publish, visit, disable, unhash, drain and
 * release.  Disabling makes existing readers fail ordinary service admission
 * while preserving cleanup visits.  Unhashing stops new readers, and the RCU
 * drain completes old readers before nodes or identity references are freed.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/hashtable.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "attachment.h"

#define PVSCHED_ATTACHMENT_HASH_BITS 8

static DEFINE_HASHTABLE(pvsched_task_hash, PVSCHED_ATTACHMENT_HASH_BITS);
static DEFINE_HASHTABLE(pvsched_vcpu_hash, PVSCHED_ATTACHMENT_HASH_BITS);
static DEFINE_RAW_SPINLOCK(pvsched_task_hash_lock);
static DEFINE_RAW_SPINLOCK(pvsched_vcpu_hash_lock);

VISIBLE_IF_KUNIT void
pvsched_attachment_owner_snapshot(struct task_struct *task,
				  struct mm_struct **mm,
				  struct pid **tgid,
				  bool *exiting)
{
	KUNIT_STATIC_STUB_REDIRECT(pvsched_attachment_owner_snapshot, task, mm,
				   tgid, exiting);
	task_lock(task);
	*mm = task->mm;
	*tgid = task_tgid(task);
	*exiting = task->flags & PF_EXITING;
	task_unlock(task);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_owner_snapshot);

int pvsched_attachment_init(struct pvsched_attachment *attachment,
			    struct task_struct *task)
{
	struct mm_struct *owner_mm;
	struct pid *owner_tgid;
	bool exiting;

	/* Prepare and pin identity privately; hooks cannot see it before publish. */
	if (!task)
		return -EINVAL;

	/* Pin identity before exec/exit can drop the task's mm or TGID. */
	task_lock(task);
	owner_mm = task->mm;
	if (owner_mm)
		mmgrab(owner_mm);
	owner_tgid = get_pid(task_tgid(task));
	exiting = task->flags & PF_EXITING;
	task_unlock(task);
	if (!owner_tgid || exiting) {
		if (owner_mm)
			mmdrop(owner_mm);
		put_pid(owner_tgid);
		return -ESRCH;
	}

	memset(attachment, 0, sizeof(*attachment));
	raw_spin_lock_init(&attachment->state_lock);
	get_task_struct(task);
	attachment->task = task;
	attachment->owner_mm = owner_mm;
	attachment->owner_tgid = owner_tgid;
	attachment->position = PVSCHED_RUNNER_QEMU;
	attachment->vcpu.attachment = attachment;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_init);

int pvsched_attachment_publish(struct pvsched_attachment *attachment,
			       pvsched_attachment_commit_fn commit, void *data)
{
	struct pvsched_attachment *other;
	struct mm_struct *mm;
	struct pid *tgid;
	unsigned long flags;
	bool exiting;
	int ret = 0;

	/*
	 * Insertion makes the task visible: every fallible enclosing-runner setup
	 * must already be complete because RCU visitors can reach it afterwards.
	 */
	raw_spin_lock_irqsave(&pvsched_task_hash_lock, flags);
	hash_for_each_possible(pvsched_task_hash, other, task_node,
			       (unsigned long)attachment->task) {
		if (other->task != attachment->task)
			continue;
		ret = -EEXIST;
		goto unlock_hash;
	}
	hash_add_rcu(pvsched_task_hash, &attachment->task_node,
		     (unsigned long)attachment->task);
	attachment->task_hashed = true;
unlock_hash:
	raw_spin_unlock_irqrestore(&pvsched_task_hash_lock, flags);
	if (ret)
		return ret;

	/* Never nest task_lock or a foreign attachment lock under a hash lock. */
	pvsched_attachment_owner_snapshot(attachment->task, &mm, &tgid,
					  &exiting);

	/*
	 * The task was inserted before this final check, so exit cannot fall
	 * between admission and visibility: either this check or the exit lookup
	 * closes it.
	 */
	raw_spin_lock_irqsave(&attachment->state_lock, flags);
	if (attachment->exited || exiting ||
	    (READ_ONCE(attachment->task->flags) & PF_EXITING) ||
	    mm != attachment->owner_mm ||
	    tgid != attachment->owner_tgid) {
		raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
		pvsched_attachment_unhash(attachment);
		pvsched_attachment_drain();
		return -ESRCH;
	}
	attachment->active = true;
	if (commit)
		commit(attachment, data);
	raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_publish);

static bool
pvsched_attachment_check_or_bind_vcpu_locked(struct pvsched_attachment *attachment,
					     const void *vcpu_key)
{
	unsigned long flags;

	/* Bind once; afterwards accept only the same non-NULL opaque key. */
	if (!vcpu_key)
		return false;
	if (attachment->vcpu.key)
		return attachment->vcpu.key == vcpu_key;

	attachment->vcpu.key = vcpu_key;
	raw_spin_lock_irqsave(&pvsched_vcpu_hash_lock, flags);
	hash_add_rcu(pvsched_vcpu_hash, &attachment->vcpu.node,
		     (unsigned long)vcpu_key);
	attachment->vcpu_hashed = true;
	raw_spin_unlock_irqrestore(&pvsched_vcpu_hash_lock, flags);
	return true;
}

bool pvsched_attachment_local_visit(const void *vcpu_key,
				    enum pvsched_runner_position position,
				    u32 mode_flags,
				    pvsched_attachment_visit_fn visit,
				    void *data)
{
	struct pvsched_attachment *attachment;
	struct task_struct *task = current;
	struct mm_struct *mm;
	struct pid *tgid;
	unsigned long flags;
	bool exiting;
	bool found = false;

	/* This is current: it cannot exec or replace its mm in this hook. */
	mm = task->mm;
	tgid = task_tgid(task);
	exiting = READ_ONCE(task->flags) & PF_EXITING;
	rcu_read_lock();
	hash_for_each_possible_rcu(pvsched_task_hash, attachment, task_node,
				   (unsigned long)task) {
		if (attachment->task != task)
			continue;
		raw_spin_lock_irqsave(&attachment->state_lock, flags);
		if (attachment->exited || exiting || mm != attachment->owner_mm ||
		    tgid != attachment->owner_tgid) {
			raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
			continue;
		}
		attachment->position = position;
		attachment->target_mode_flags = mode_flags;
		attachment->target_mode_valid = true;
		if (!attachment->active) {
			if (visit)
				visit(attachment, PVSCHED_ATTACHMENT_VISIT_CLEANUP, data);
		} else if (!pvsched_attachment_check_or_bind_vcpu_locked(attachment,
								       vcpu_key)) {
			/* A setter-safe cleanup checkpoint must restore the baseline. */
			pvsched_attachment_disable_locked(attachment);
			attachment->binding_failed = true;
			attachment->restore_owed = true;
			if (visit)
				visit(attachment,
				      PVSCHED_ATTACHMENT_VISIT_BINDING_FAILED, data);
		} else if (visit) {
			visit(attachment, PVSCHED_ATTACHMENT_VISIT_ORDINARY, data);
		}
		raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
		found = true;
		break;
	}
	rcu_read_unlock();
	return found;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_local_visit);

bool pvsched_attachment_mark_exited(struct task_struct *task)
{
	struct pvsched_attachment *attachment;
	unsigned long flags;
	bool found = false;

	rcu_read_lock();
	hash_for_each_possible_rcu(pvsched_task_hash, attachment, task_node,
				   (unsigned long)task) {
		if (attachment->task != task)
			continue;
		raw_spin_lock_irqsave(&attachment->state_lock, flags);
		attachment->exited = true;
		pvsched_attachment_disable_locked(attachment);
		raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
		found = true;
	}
	rcu_read_unlock();
	return found;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_mark_exited);

bool pvsched_attachment_cleanup_visit(struct task_struct *task,
				      pvsched_attachment_visit_fn visit,
				      void *data)
{
	struct pvsched_attachment *attachment;
	unsigned long flags;
	bool found = false;

	rcu_read_lock();
	hash_for_each_possible_rcu(pvsched_task_hash, attachment, task_node,
				   (unsigned long)task) {
		if (attachment->task != task)
			continue;
		raw_spin_lock_irqsave(&attachment->state_lock, flags);
		if (visit)
			visit(attachment, PVSCHED_ATTACHMENT_VISIT_CLEANUP, data);
		found = true;
		raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
	}
	rcu_read_unlock();
	return found;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_cleanup_visit);

unsigned int pvsched_attachment_remote_visit(const void *vcpu_key,
					     u32 target_mode_flags,
					      pvsched_attachment_visit_fn visit,
					      void *data)
{
	struct pvsched_vcpu_key *binding;
	struct pvsched_attachment *attachment;
	unsigned long flags;
	unsigned int visited = 0;

	if (!vcpu_key ||
	    (target_mode_flags & (PVSCHED_RUNNER_MODE_NESTED |
				  PVSCHED_RUNNER_MODE_UNSUPPORTED)))
		return 0;

	rcu_read_lock();
	/* Scan every equal key: an old RCU-visible node may precede a live one. */
	hash_for_each_possible_rcu(pvsched_vcpu_hash, binding, node,
				   (unsigned long)vcpu_key) {
		if (binding->key != vcpu_key)
			continue;
		attachment = binding->attachment;
		raw_spin_lock_irqsave(&attachment->state_lock, flags);
		if (attachment->active &&
		    attachment->target_mode_valid &&
		    !(attachment->target_mode_flags &
		      (PVSCHED_RUNNER_MODE_NESTED |
		       PVSCHED_RUNNER_MODE_UNSUPPORTED)) &&
		    (attachment->position == PVSCHED_RUNNER_HOST ||
		     attachment->position == PVSCHED_RUNNER_BLOCKED)) {
			if (visit)
				visit(attachment, PVSCHED_ATTACHMENT_VISIT_ORDINARY,
				      data);
			visited++;
		}
		raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
	}
	rcu_read_unlock();
	return visited;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_remote_visit);

void pvsched_attachment_disable_locked(struct pvsched_attachment *attachment)
{
	attachment->active = false;
}

void pvsched_attachment_disable(struct pvsched_attachment *attachment)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&attachment->state_lock, flags);
	pvsched_attachment_disable_locked(attachment);
	raw_spin_unlock_irqrestore(&attachment->state_lock, flags);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_disable);

void pvsched_attachment_unhash(struct pvsched_attachment *attachment)
{
	unsigned long flags;

	if (attachment->vcpu_hashed) {
		raw_spin_lock_irqsave(&pvsched_vcpu_hash_lock, flags);
		hash_del_rcu(&attachment->vcpu.node);
		attachment->vcpu_hashed = false;
		raw_spin_unlock_irqrestore(&pvsched_vcpu_hash_lock, flags);
	}
	if (attachment->task_hashed) {
		raw_spin_lock_irqsave(&pvsched_task_hash_lock, flags);
		hash_del_rcu(&attachment->task_node);
		attachment->task_hashed = false;
		raw_spin_unlock_irqrestore(&pvsched_task_hash_lock, flags);
	}
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_unhash);

void pvsched_attachment_drain(void)
{
	KUNIT_STATIC_STUB_REDIRECT(pvsched_attachment_drain);
	synchronize_rcu();
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_drain);

void pvsched_attachment_release(struct pvsched_attachment *attachment)
{
	WARN_ON_ONCE(attachment->task_hashed || attachment->vcpu_hashed);
	if (attachment->owner_mm)
		mmdrop(attachment->owner_mm);
	put_pid(attachment->owner_tgid);
	put_task_struct(attachment->task);
	attachment->owner_mm = NULL;
	attachment->owner_tgid = NULL;
	attachment->task = NULL;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_attachment_release);
