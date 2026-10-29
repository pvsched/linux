/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_EXTERNAL_H
#define _VIRT_PVSCHED_EXTERNAL_H

#include <linux/pvsched_policy.h>
#include <linux/types.h>

#include "attachment.h"

struct pvsched_policy_entry;
struct pvsched_runner_kvm_info;
struct pvsched_runner_runtime;
struct pvsched_vcpu_runner;

/*
 * The target-local events an external policy is called for: the reconcile
 * events it shares with managed policies, and two that only it sees.
 */
enum pvsched_external_reconcile_event {
	PVSCHED_EXTERNAL_RECONCILE_RUN_ENTER = PVSCHED_RECONCILE_RUN_ENTER,
	PVSCHED_EXTERNAL_RECONCILE_VMEXIT = PVSCHED_RECONCILE_VMEXIT,
	PVSCHED_EXTERNAL_RECONCILE_CANCEL = PVSCHED_RECONCILE_CANCEL,
	PVSCHED_EXTERNAL_RECONCILE_RUN_LEAVE = PVSCHED_RECONCILE_RUN_LEAVE,
	PVSCHED_EXTERNAL_RECONCILE_HALT = PVSCHED_RECONCILE_HALT,
	PVSCHED_EXTERNAL_RECONCILE_UNHALT = PVSCHED_RECONCILE_UNHALT,
	/* KVM reports a late VMENTRY as CANCEL with vmentry set. */
	PVSCHED_EXTERNAL_RECONCILE_VMENTRY = PVSCHED_RECONCILE_INJECT + 1,
	PVSCHED_EXTERNAL_RECONCILE_VMEXIT_IRQOFF,
};

/* One target-local event, as the runner's local visitor received it. */
struct pvsched_external_event {
	enum pvsched_external_reconcile_event event;
	/* pvsched's own mode flags: unsupported, and a VMEXIT's HLT exit. */
	u32 mode_flags;
	bool interrupt_ready;
	/* NULL where the caller has none, as KUnit callers do. */
	const struct pvsched_runner_kvm_info *kvm;
};

/*
 * The external policy side of a runner runtime.  The runtime is only its
 * container: an external attachment runs none of the managed engine, and
 * these calls are all pvsched does for it besides the attachment's own
 * lifetime and hook checks.
 */

/* Runtime prepare for an external @entry: take a reference on it. */
void pvsched_external_prepare(struct pvsched_runner_runtime *runtime,
			      struct pvsched_policy_entry *entry);
/*
 * Fill in the vCPU context and call the attach op, once negotiation is
 * enabled and before publication.  Session mutex held; may sleep.
 */
int pvsched_external_attach(struct pvsched_runner_runtime *runtime,
			    struct pvsched_vcpu_runner *runner);
/*
 * Call the detach op if attach succeeded and detach is still owed: at
 * teardown, once the attachment is disabled and drained, or when
 * publication fails.  Process context; takes the state lock.
 */
void pvsched_external_detach(struct pvsched_runner_runtime *runtime);
/* A target-local event under state_lock: call the op it maps to. */
void pvsched_external_local_event_locked(struct pvsched_runner_runtime *runtime,
					 enum pvsched_attachment_visit_kind kind,
					 const struct pvsched_external_event *event);
/* Whether the policy has an inject op, which saves deferring for nothing. */
bool pvsched_external_wants_inject(const struct pvsched_runner_runtime *runtime);
/* An admitted injection, in place or from the target's inject work. */
void pvsched_external_inject_locked(struct pvsched_runner_runtime *runtime,
				    u32 kvm_mode, bool source_guest_mode);

#endif /* _VIRT_PVSCHED_EXTERNAL_H */
