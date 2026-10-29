/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pvsched external policies.
 *
 * An external policy runs no pvsched engine.  pvsched negotiates the shared
 * page, pins it, keeps the attachment lifetime and its KVM hook checks, and
 * calls the policy's ops at each event with the vCPU's context.  It owns
 * only the 64-byte header of the page; the rest belongs to the policy and
 * its guest side, and pvsched never reads or writes it.  A VMM
 * selects an external policy by name like any other, and a guest
 * negotiates for it with the CUSTOM protocol and POLICY mode.
 */
#ifndef _LINUX_PVSCHED_EXTERNAL_H
#define _LINUX_PVSCHED_EXTERNAL_H

#include <linux/types.h>
#include <uapi/linux/pvsched.h>

struct module;
struct task_struct;

/* Where the vCPU thread is, as of the event. */
enum pvsched_vcpu_position {
	/* In the kernel, outside guest mode; possibly preempted. */
	PVSCHED_VCPU_HOST,
	/* Halted in KVM. */
	PVSCHED_VCPU_BLOCKED,
	/* Running guest code, or about to. */
	PVSCHED_VCPU_GUEST,
	/* Back in the VMM, between KVM_RUN calls. */
	PVSCHED_VCPU_VMM,
};

/* The vCPU an op is called for.  Valid only during the call. */
struct pvsched_vcpu {
	/* The vCPU thread. */
	struct task_struct *task;
	/*
	 * The policy's own, NULL until attach sets it.  pvsched only carries
	 * it from call to call, and never allocates or frees it.
	 */
	void *priv;
	/* KVM's vcpu_id, or -1 before the vCPU's first KVM event. */
	s32 vcpu_id;
	/*
	 * KVM_PVSCHED_MODE_* of this event; for inject, the target's mode at
	 * injection.  run_leave, vmexit_irqoff and detach carry none.
	 */
	u32 kvm_mode;
	/* enum pvsched_vcpu_position */
	u32 position;
};

/**
 * struct pvsched_external_ops - an external host policy
 * @attach: a vCPU attached; a nonzero return fails the ATTACH
 * @detach: the vCPU is detached; the last call for it.  @exited: its
 *	task is exiting or gone, so its scheduling must not be touched
 * @run_enter: KVM_RUN starts
 * @run_leave: KVM_RUN returns @ret to the VMM
 * @vmentry: the guest is about to be entered, on every entry, including
 *	a fast re-entry after an exit KVM handled with IRQs still off
 * @vmentry_cancel: an entry was cancelled before reaching the guest
 * @vmexit_irqoff: the guest exited to KVM's run loop; IRQs are still off.
 *	Fast re-entries are not reported, so several @vmentry calls may
 *	precede one @vmexit_irqoff
 * @vmexit: a slow exit, before KVM handles it
 * @halt: the vCPU is about to block in a halt
 * @unhalt: the vCPU is runnable again after a halt
 * @inject: an interrupt or NMI is injected into the vCPU
 * @version: with @name, what the guest negotiates for
 * @name: nonempty, NUL-terminated and zero-padded
 * @owner: set by pvsched_register_external_policy()
 *
 * Every op is optional.  @attach runs in process context with the session
 * mutex held, after the guest's negotiation succeeded and before the vCPU
 * is visible to any event, so it may sleep; it must not call back into
 * pvsched for the same session.  It returns 0 or a negative errno, which
 * fails the ATTACH.  If @attach succeeds, @detach is called exactly once,
 * in process context: when the attachment is torn down, after pvsched has
 * told the guest it is disabled and no other op can run any more, or, if
 * publishing the attachment fails, before the guest was told anything.
 * State that @attach set in v->priv is freed in @detach; as @detach holds
 * a raw spinlock, on PREEMPT_RT the free must be deferred, such as with
 * kfree_rcu().
 *
 * Every op but @attach runs with the attachment's raw state lock held and
 * IRQs off, so the ops of one vCPU never run concurrently and none runs
 * after @detach.  They must not sleep, and may take only locks that order after
 * the task's pi_lock and runqueue lock.  @inject runs on the injecting CPU,
 * or, when the source was in guest mode and the target is not running
 * guest code, from hard irq_work on the target's CPU; @source_guest_mode
 * is set only in the first case with the source still in guest mode.
 * Injections deferred to the target's CPU before that call runs are merged
 * into it, with the mode of the latest, so @inject does not count interrupts.
 *
 * The area, the policy's part of the page, is reached only through the
 * pvsched_area_*() helpers.  Any op may read it.  Writes are allowed only
 * in @vmexit and @vmentry, between which the vCPU runs no guest code, so
 * the guest sees them whole when it resumes, and in @detach, to scrub
 * them.  @inject may not write: it may run while the guest does.
 * Everything the guest wrote is untrusted input.
 * A policy may change the scheduling of v->task, and only of that task,
 * with sched_setattr_nocheck_nopi(), but not in @vmentry or
 * @vmexit_irqoff, nor in an @inject with @source_guest_mode set.  @detach
 * is where a policy restores what it changed, unless @exited.
 */
struct pvsched_external_ops {
	int (*attach)(struct pvsched_vcpu *v);
	void (*detach)(struct pvsched_vcpu *v, bool exited);

	void (*run_enter)(struct pvsched_vcpu *v);
	void (*run_leave)(struct pvsched_vcpu *v, int ret, u32 exit_reason);
	void (*vmentry)(struct pvsched_vcpu *v, bool interrupt_ready);
	void (*vmentry_cancel)(struct pvsched_vcpu *v);
	void (*vmexit_irqoff)(struct pvsched_vcpu *v);
	void (*vmexit)(struct pvsched_vcpu *v, bool hlt_exit);
	void (*halt)(struct pvsched_vcpu *v);
	void (*unhalt)(struct pvsched_vcpu *v);
	void (*inject)(struct pvsched_vcpu *v, bool source_guest_mode);

	u32 version;
	char name[PVSCHED_NAME_MAX];
	struct module *owner;
};

/*
 * Register last in module init and unregister first in module exit.  As
 * for other policies, unregistering only unlists the policy: a session that
 * selected it keeps it, and a reference on its module, until it closes.
 */
int __pvsched_register_external_policy(struct pvsched_external_ops *ops,
				       struct module *owner);
#define pvsched_register_external_policy(ops) \
	__pvsched_register_external_policy(ops, THIS_MODULE)
void pvsched_unregister_external_policy(struct pvsched_external_ops *ops);

/*
 * Copy @size bytes from @buf into @v's area at @off, as whole aligned
 * words, each single-copy atomic for the guest.  @off and @size are
 * multiples of 8 within PVSCHED_OPAQUE_SIZE.  Only from the vmexit,
 * vmentry or detach op, with the @v it was called with.  Returns 0,
 * -EINVAL for a bad range, or -EPERM from any other op or for a vCPU
 * without a mapped page.
 */
int pvsched_area_write(struct pvsched_vcpu *v, u32 off, const void *buf,
		       u32 size);

/* As pvsched_area_write(), for the one word at @off. */
int pvsched_area_write_u64(struct pvsched_vcpu *v, u32 off, u64 val);

/*
 * Copy @size bytes of @v's area at @off into @buf, from any op, with the
 * @v it was called with.  Each word is read whole; nothing is consistent
 * across words.  @off and @size as for pvsched_area_write().  Returns 0,
 * or -EINVAL for a bad range or -ENOENT for a vCPU without a mapped page,
 * with @buf zeroed.
 */
int pvsched_area_read(struct pvsched_vcpu *v, u32 off, void *buf, u32 size);

/* As pvsched_area_read(), for the one word at @off. */
int pvsched_area_read_u64(struct pvsched_vcpu *v, u32 off, u64 *val);

#endif /* _LINUX_PVSCHED_EXTERNAL_H */
