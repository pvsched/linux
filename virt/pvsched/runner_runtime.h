/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_RUNNER_RUNTIME_H
#define _VIRT_PVSCHED_RUNNER_RUNTIME_H

#include <linux/bits.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <uapi/linux/pvsched.h>

#include "default_policy.h"
#include "runtime_accounting.h"
#include "attachment.h"

enum pvsched_reconcile_event {
	/* RUN_ENTER: IRQs enabled, preemptible, setter permitted. */
	PVSCHED_RECONCILE_RUN_ENTER,
	/* IRQ-on VMEXIT: preemption disabled, setter permitted. */
	PVSCHED_RECONCILE_VMEXIT,
	/* VMENTRY_CANCEL: IRQs enabled, preemption disabled, setter permitted. */
	PVSCHED_RECONCILE_CANCEL,
	/* RUN_LEAVE: IRQs enabled, preemptible, setter permitted. */
	PVSCHED_RECONCILE_RUN_LEAVE,
	/* HALT: immediately before blocking, setter permitted. */
	PVSCHED_RECONCILE_HALT,
	/* UNHALT: after schedule(), balances sleep without classifying it. */
	PVSCHED_RECONCILE_UNHALT,
	/* INJECT: accepted producer fact in an audited source context. */
	PVSCHED_RECONCILE_INJECT,
};

enum pvsched_runner_reason {
	PVSCHED_RUNNER_REASON_HALT = BIT(0),
	PVSCHED_RUNNER_REASON_INJECT = BIT(1),
};

struct pvsched_runner_event_input {
	const struct pvsched_default_guest_area *guest;
	u32 mode_flags;
};

struct pvsched_runner_vmentry_input {
	struct pvsched_default_guest_area guest;
	u32 mode_flags;
	bool interrupt_ready;
};

/* Owned by the persistent runner, rather than an attachment runtime. */
struct pvsched_ticket_owner {
	u64 last_ticket;
};

struct pvsched_runner_runtime {
	/* Common attachment identity, admission, position and state lock. */
	struct pvsched_attachment attachment;
	/* ATTACH scheduling state: restore target and ownership reference. */
	struct sched_task_state baseline;
	/* Last successfully applied canonical tuple, published at VMENTRY. */
	struct pvsched_prio_desc applied;
	/* Actual fair nice, retained while FIFO or IDLE leaves it latent. */
	int expected_nice;
	/* Budgets, anchors, charge domain, and current HOST/GUEST phase. */
	struct pvsched_runtime_accounting accounting;
	/* Validated built-in default-policy priorities. */
	struct pvsched_default_policy_config config;
	/* Last selection snapshot, reused while the vCPU runs nested. */
	struct pvsched_default_guest_area last_guest_area;
	/* Persistent owner state that prevents ticket reuse across attachments. */
	struct pvsched_ticket_owner *ticket_owner;
	/* Pinned hard cap timer; its callback only forces a nonfast exit. */
	struct hrtimer cutoff_timer;
	/* Latest owned-fault errno; external ownership does not set it. */
	int last_fault;
	/*
	 * Nonzero while a binding or late-entry failure owes one setter-safe
	 * baseline restore: the errno reported when it is paid.
	 */
	int restore_owed_error;
	/* Destruction has closed admission and is draining callbacks. */
	bool closing;
	/* An owned restore failed; ordinary checkpoints must not retry it. */
	bool restore_failed;
	/* KVM currently reports nested L2 execution. */
	bool nested_l2;
	/* last_guest_area contains a snapshot supplied while not nested. */
	bool last_guest_area_valid;
	/* Pending HLT and accepted-injection assistance. */
	unsigned long reasons;
	/* An unsupported late mode fact owes safe baseline restoration. */
	bool revoke_pending;
	/* Publish the cooperative deboost kick hint in applied feedback. */
	bool deboost_notify;
	/* Current opaque nonzero handoff token, or zero when none is live. */
	u64 interrupt_ticket;
	/* Longest a boost is kept for an idle guest heading to a halt; 0: off. */
	u64 idle_hold_ns;
	/*
	 * End of this idle episode's grace, or zero when none has started.
	 * Once passed, the grace is spent until the guest leaves idle.
	 */
	u64 idle_hold_end_ns;
	/* The applied tuple is held for an idle guest; only then cap cutoff. */
	bool idle_holding;
#if IS_ENABLED(CONFIG_KUNIT)
	/* Avoid task-local static-stub lookup while state_lock is held. */
	int (*test_setattr)(struct task_struct *task,
			    const struct sched_attr *attr, void *data);
	void *test_setattr_data;
#endif
};

enum pvsched_runner_restore_disposition {
	PVSCHED_RESTORE_RESTORED,
	PVSCHED_RESTORE_EXITED,
	PVSCHED_RESTORE_EXTERNAL_OWNER,
	PVSCHED_RESTORE_OWNED_FAILURE,
};

/*
 * Currently defined subset of the per-vCPU call order for activation wiring:
 *
 * ATTACH_SHM             -> prepare, then one final publish
 * RUN_ENTER              -> pvsched_runner_reconcile(RUN_ENTER, facts)
 * late VMENTRY, IRQ-off  -> pvsched_runner_vmentry()
 * fast re-entry          -> pvsched_runner_vmentry()
 * IRQ-off VMEXIT         -> pvsched_runner_guest_exit_irqoff()
 * IRQ-on VMEXIT          -> pvsched_runner_reconcile(VMEXIT)
 * VMENTRY_CANCEL         -> pvsched_runner_reconcile(CANCEL)
 * RUN_LEAVE              -> pvsched_runner_reconcile(RUN_LEAVE)
 * DETACH / final close   -> split disable/drain/disposition/unhash/release
 *
 * HALT, UNHALT, and INJECT are private events here; their KVM
 * adapters, remote lookup, and source-mode deferral are not wired yet.
 */

/**
 * pvsched_runner_runtime_prepare() - prepare private runtime state
 * @runtime: uninitialized per-attachment state
 * @task: vCPU thread whose scheduling state is captured and referenced
 * @config: validated default-policy configuration
 * @cs_budget_ns: critical-section budget limit
 * @generic_budget_ns: total elevated-runtime budget limit
 * @ticket_owner: persistent runner ticket-generation state
 * @deboost_notify: publish the cooperative guest deboost hint
 *
 * Captures private state without hash visibility or scheduling mutation.
 * The caller performs every other fallible setup step before one final
 * pvsched_runner_runtime_publish().  A prepared runtime that is not published
 * must be paired with pvsched_runner_runtime_release().
 */
int pvsched_runner_runtime_prepare(struct pvsched_runner_runtime *runtime,
				   struct task_struct *task,
				   const struct pvsched_default_policy_config *config,
				   u64 cs_budget_ns, u64 generic_budget_ns,
				   struct pvsched_ticket_owner *ticket_owner,
				   bool deboost_notify);
int pvsched_runner_runtime_publish(struct pvsched_runner_runtime *runtime,
				   pvsched_attachment_commit_fn commit, void *data);
/* Split close primitives permit one RCU drain for a batch of runtimes. */
void pvsched_runner_runtime_disable(struct pvsched_runner_runtime *runtime);
void pvsched_runner_runtime_drain_actions(struct pvsched_runner_runtime *runtime);
/*
 * Classify one baseline disposition after admission and actions are drained.
 * Automatic cleanup passes false so a prior owned restoration failure is not
 * retried; explicit detach and final close pass true for one last bounded
 * attempt.  The caller decides from the disposition whether unhash is safe.
 */
enum pvsched_runner_restore_disposition
pvsched_runner_runtime_finish_close(struct pvsched_runner_runtime *runtime,
				    bool final_attempt);
void pvsched_runner_runtime_unhash(struct pvsched_runner_runtime *runtime);
void pvsched_runner_runtime_release(struct pvsched_runner_runtime *runtime);

/**
 * pvsched_runner_reconcile() - reconcile one factual event
 * @runtime: initialized per-attachment state
 * @event: RUN_ENTER, IRQ-on VMEXIT, VMENTRY_CANCEL, or RUN_LEAVE
 * @input: required event facts; guest may be omitted only where unused
 *
 * Called from the named KVM checkpoint with IRQs enabled and no pi or rq lock
 * held.  VMEXIT and CANCEL still have preemption disabled; RUN_ENTER and
 * RUN_LEAVE may be preemptible.  It disables IRQs under state_lock and may call
 * the restricted atomic scheduler setter.  VMEXIT and RUN_LEAVE with an open
 * GUEST phase close that window; other events settle HOST before selection.
 * Non-nested selection retains the supplied snapshot under state_lock.
 * Returns 0, event-local -EINVAL for malformed input, -ESRCH, -ESHUTDOWN,
 * -EOWNERDEAD, a setter/internal error, or last_fault ?: -EIO for a restore
 * obligation/failure.  Unsupported mode facts revoke this attachment.
 */
int pvsched_runner_reconcile(struct pvsched_runner_runtime *runtime,
			     enum pvsched_reconcile_event event,
			     const struct pvsched_runner_event_input *input);

/* Target-local adapters that find the runtime by its vCPU key. */
int pvsched_runner_local_reconcile(const void *vcpu_key,
				   enum pvsched_reconcile_event event,
				   const struct pvsched_runner_event_input *input);

/**
 * pvsched_runner_vmentry() - account, hand off a ticket, and publish feedback
 * @runtime: initialized per-attachment state
 * @input: local acquire-ordered guest/ack snapshot and late KVM facts
 * @host: host-area output snapshot
 *
 * Called at late VMENTRY with IRQs and preemption disabled, after KVM's last
 * reschedule check.  It never calls the scheduler setter.  Returns 0,
 * -ESHUTDOWN, an accounting error, or last_fault ?: -EIO.  The window opened
 * here is closed by reconcile(VMEXIT), which settles before selecting; there
 * is intentionally no separate close API, preserving settle -> select order.
 */
int pvsched_runner_vmentry(struct pvsched_runner_runtime *runtime,
			   const struct pvsched_runner_vmentry_input *input,
			   struct pvsched_host_area *host);
int pvsched_runner_local_vmentry(const void *vcpu_key,
				 const struct pvsched_runner_vmentry_input *input,
				 struct pvsched_host_area *host);

/**
 * pvsched_runner_guest_exit_irqoff() - cancel the current GUEST cutoff timer
 * @runtime: initialized per-attachment state
 *
 * Called on same-CPU IRQ-off VMEXIT with preemption disabled.  It neither
 * accounts nor calls the setter; the matching IRQ-on reconcile(VMEXIT) settles
 * the GUEST window.
 */
void pvsched_runner_guest_exit_irqoff(struct pvsched_runner_runtime *runtime);

#endif /* _VIRT_PVSCHED_RUNNER_RUNTIME_H */
