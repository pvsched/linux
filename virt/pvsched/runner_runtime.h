/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_RUNNER_RUNTIME_H
#define _VIRT_PVSCHED_RUNNER_RUNTIME_H

#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/pvsched_policy.h>
#include <uapi/linux/pvsched.h>

#include "policy.h"
#include "runtime_accounting.h"

struct pvsched_runner_runtime {
	/* Serializes mutable runtime state and setter calls: state -> pi -> rq. */
	raw_spinlock_t state_lock;
	/* vCPU thread, referenced from successful init until destroy. */
	struct task_struct *task;
	/* Counted registry entry of the bound policy, and its ops. */
	struct pvsched_policy_entry *entry;
	const struct pvsched_policy_ops *ops;
	/* The policy's view of this vCPU; its buffers are allocated below. */
	struct pvsched_policy_ctx ctx;
	/*
	 * ATTACH capture of the policy's parameters: the restore target, which
	 * a reattach compares with the runner's saved baseline.
	 */
	void *baseline;
	/* Last value applied successfully, which the same-bytes skip compares. */
	void *applied;
	/* map() output, zeroed before each call. */
	void *out;
	/* Class of the applied value: charging, the forced-exit kick, feedback. */
	enum pvsched_boost_class applied_class;
	/* Budgets, anchors, charge domain, and current HOST/GUEST phase. */
	struct pvsched_runtime_accounting accounting;
	/* Last selection snapshot, reused while the vCPU runs nested. */
	struct pvsched_default_guest_area last_guest_area;
	/* Pinned hard cap timer; its callback only forces a nonfast exit. */
	struct hrtimer cutoff_timer;
	/* Attachment generation reserved for validating late callbacks. */
	u64 generation;
	/* Latest owned-fault errno; external ownership does not set it. */
	int last_fault;
	/*
	 * Nonzero while a late-entry failure owes one setter-safe baseline
	 * restore: the errno reported when it is paid.
	 */
	int restore_owed_error;
	/* Runtime service currently admits ordinary operations. */
	bool active;
	/* Destruction has closed admission and is draining callbacks. */
	bool closing;
	/* sched_process_exit observed; task restoration is no longer safe. */
	bool exited;
	/* An owned restore failed; ordinary checkpoints must not retry it. */
	bool restore_failed;
	/* KVM currently reports nested L2 execution. */
	bool nested_l2;
	/* last_guest_area contains a snapshot supplied while not nested. */
	bool last_guest_area_valid;
};

/*
 * Currently defined subset of the per-vCPU call order for activation wiring:
 *
 * ATTACH_SHM             -> pvsched_runner_runtime_init()
 * RUN_ENTER              -> pvsched_runner_reconcile(RUN_ENTER)
 * late VMENTRY, IRQ-off  -> pvsched_runner_guest_start()
 *                           pvsched_runner_publish_vmentry()
 * fast re-entry          -> pvsched_runner_guest_start()
 * IRQ-off VMEXIT         -> pvsched_runner_guest_exit_irqoff()
 * IRQ-on VMEXIT          -> pvsched_runner_reconcile(VMEXIT)
 * VMENTRY_CANCEL         -> pvsched_runner_reconcile(CANCEL)
 * RUN_LEAVE              -> pvsched_runner_reconcile(RUN_LEAVE)
 * DETACH / final close   -> pvsched_runner_runtime_destroy()
 *
 * HALT, UNHALT, and remote-injection entry points are not wired yet.
 */

/**
 * pvsched_runner_runtime_init() - create one attachment's runtime state
 * @runtime: uninitialized per-attachment state
 * @task: vCPU thread whose scheduling state is captured and referenced
 * @generation: attachment generation for future late-callback validation
 * @entry: registry entry of the policy to bind; the runtime takes a reference
 * @cs_budget_ns: critical-section budget limit
 * @generic_budget_ns: total elevated-runtime budget limit
 *
 * Called by ATTACH_SHM in sleepable task context before publishing the
 * attachment.  It allocates the policy's buffers and captures its baseline,
 * and does not call the scheduler setter.  Returns 0, -ENOMEM, an
 * accounting error, or -EOPNOTSUPP for an unsupported baseline.
 * A successful call must be paired with pvsched_runner_runtime_destroy().
 */
int pvsched_runner_runtime_init(struct pvsched_runner_runtime *runtime,
				struct task_struct *task, u64 generation,
				struct pvsched_policy_entry *entry,
				u64 cs_budget_ns, u64 generic_budget_ns);
/**
 * pvsched_runner_runtime_destroy() - end one attachment's runtime lifetime
 * @runtime: initialized per-attachment state
 *
 * Called by DETACH or final close in sleepable task context, after lookup
 * removal and tracepoint/irq_work draining.  It may call the scheduler setter
 * for the final baseline restore.  Per-attachment state, including debt, dies
 * here.  Activation wiring must retain sticky fault state in the persistent
 * runner, including a restore failure first discovered during this call.
 *
 * The owner must first remove the runner from every lookup/binding and drain
 * tracepoint/irq_work callbacks.  This function closes admission before it
 * synchronously cancels the timer and drops the task reference.
 */
void pvsched_runner_runtime_destroy(struct pvsched_runner_runtime *runtime);

/**
 * pvsched_runner_reconcile() - settle, select, apply, and commit at a safe hook
 * @runtime: initialized per-attachment state
 * @event: RUN_ENTER, IRQ-on VMEXIT, VMENTRY_CANCEL, or RUN_LEAVE
 * @guest: coherent local guest-area snapshot, not live guest-owned SHM;
 *         ignored for RUN_LEAVE and while nested
 *
 * Called from the named KVM checkpoint with IRQs enabled and no pi or rq lock
 * held.  VMEXIT and CANCEL still have preemption disabled; RUN_ENTER and
 * RUN_LEAVE may be preemptible.  It disables IRQs under state_lock and may call
 * the restricted atomic scheduler setter.  VMEXIT and RUN_LEAVE with an open
 * GUEST phase close that window; other events settle HOST before selection.
 * Non-nested selection retains the supplied snapshot under state_lock.
 * Returns 0, event-local -EINVAL for malformed input, -ESRCH, -ESHUTDOWN,
 * -EOWNERDEAD, a setter/internal error, or last_fault ?: -EIO for a restore
 * obligation/failure.  It pairs entry-side selection with guest_start() and
 * closes the window opened there at IRQ-on VMEXIT.
 */
int pvsched_runner_reconcile(struct pvsched_runner_runtime *runtime,
			     enum pvsched_reconcile_event event,
			     const struct pvsched_default_guest_area *guest);

/**
 * pvsched_runner_guest_start() - settle HOST and open the GUEST window
 * @runtime: initialized per-attachment state
 *
 * Called at late VMENTRY with IRQs and preemption disabled, after KVM's last
 * reschedule check.  It never calls the scheduler setter.  Returns 0,
 * -ESHUTDOWN, an accounting error, or last_fault ?: -EIO.  The window opened
 * here is closed by reconcile(VMEXIT), which settles before selecting; there
 * is intentionally no separate close API, preserving settle -> select order.
 */
int pvsched_runner_guest_start(struct pvsched_runner_runtime *runtime);

/**
 * pvsched_runner_guest_exit_irqoff() - cancel the current GUEST cutoff timer
 * @runtime: initialized per-attachment state
 *
 * Called on same-CPU IRQ-off VMEXIT with preemption disabled.  It neither
 * accounts nor calls the setter; the matching IRQ-on reconcile(VMEXIT) settles
 * the GUEST window.
 */
void pvsched_runner_guest_exit_irqoff(struct pvsched_runner_runtime *runtime);

/**
 * pvsched_runner_publish_vmentry() - publish the applied scheduling snapshot
 * @runtime: initialized per-attachment state
 * @host: host-owned shared area to update
 *
 * Called at late VMENTRY with IRQs and preemption disabled, after
 * guest_start().  It takes only state_lock, cannot call the scheduler setter,
 * and has no return value.
 */
void pvsched_runner_publish_vmentry(struct pvsched_runner_runtime *runtime,
				    struct pvsched_host_area *host);

#endif /* _VIRT_PVSCHED_RUNNER_RUNTIME_H */
