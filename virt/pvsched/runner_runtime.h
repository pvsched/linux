/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_RUNNER_RUNTIME_H
#define _VIRT_PVSCHED_RUNNER_RUNTIME_H

#include <linux/bits.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/irq_work.h>
#include <linux/spinlock.h>
#include <linux/pvsched_policy.h>
#include <uapi/linux/pvsched.h>

#include "policy.h"
#include "runtime_accounting.h"
#include "attachment.h"

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
	/* Persistent owner state that prevents ticket reuse across attachments. */
	struct pvsched_ticket_owner *ticket_owner;
	/* Negotiation decided when ATTACH prepared this runtime. */
	enum pvsched_status negotiation;
	/* Pinned hard cap timer; its callback only forces a nonfast exit. */
	struct hrtimer cutoff_timer;
	/* Source-guest-mode injection is applied after guest state is unloaded. */
	struct irq_work inject_work;
	/* Mode snapshot coalesced for the pending injection work. */
	u32 inject_mode_flags;
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
	/* The applied value is held for an idle guest; only then cap cutoff. */
	bool idle_holding;
};

enum pvsched_runner_restore_disposition {
	PVSCHED_RESTORE_RESTORED,
	PVSCHED_RESTORE_EXITED,
	PVSCHED_RESTORE_EXTERNAL_OWNER,
	PVSCHED_RESTORE_OWNED_FAILURE,
};

/*
 * Per-vCPU call order used by the production KVM adapters.  Every local
 * event goes through pvsched_runner_local_event(), which snapshots guest
 * input and publishes VMENTRY feedback only through the shared-page bridge:
 *
 * ATTACH_SHM             -> prepare, then one final publish
 * RUN_ENTER, IRQ-on VMEXIT, VMENTRY_CANCEL, HALT, UNHALT, RUN_LEAVE
 *                        -> pvsched_runner_local_event(event)
 * late VMENTRY, IRQ-off (including fast re-entry)
 *                        -> pvsched_runner_local_event(..., vmentry = true)
 * IRQ-off VMEXIT         -> pvsched_runner_local_guest_exit_irqoff()
 * INJECT                 -> pvsched_runner_remote_inject()
 * DETACH / final close   -> split disable/drain/disposition/unhash/release
 *
 * pvsched_runner_reconcile() and pvsched_runner_vmentry() drive one runtime
 * directly and exist for KUnit; they never see the shared page.
 */

/**
 * pvsched_runner_runtime_prepare() - prepare private runtime state
 * @runtime: uninitialized per-attachment state
 * @task: vCPU thread whose scheduling state is captured and referenced
 * @entry: registry entry of the policy to bind; the runtime takes a reference
 * @cs_budget_ns: critical-section budget limit
 * @generic_budget_ns: total elevated-runtime budget limit
 * @ticket_owner: persistent runner ticket-generation state
 * @deboost_notify: publish the cooperative guest deboost hint
 *
 * Allocates the policy's buffers and captures its baseline, without hash
 * visibility or scheduling mutation.
 * The caller performs every other fallible setup step before one final
 * pvsched_runner_runtime_publish().  A prepared runtime that is not published
 * must be paired with pvsched_runner_runtime_release().
 */
int pvsched_runner_runtime_prepare(struct pvsched_runner_runtime *runtime,
				   struct task_struct *task,
				   struct pvsched_policy_entry *entry,
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

/* Apply or defer one vCPU-keyed remote injection fact. */
unsigned int pvsched_runner_remote_inject(const void *vcpu_key,
					  u32 mode_flags,
					  bool source_guest_mode);
/* Target-local adapters that find the runtime by its vCPU key. */
int pvsched_runner_local_event(const void *vcpu_key,
			       enum pvsched_reconcile_event event,
			       u32 mode_flags, bool vmentry,
			       bool interrupt_ready);
void pvsched_runner_local_guest_exit_irqoff(const void *vcpu_key);

/**
 * pvsched_runner_vmentry() - account, hand off a ticket, and publish feedback
 * @runtime: initialized per-attachment state
 * @input: local acquire-ordered guest/ack snapshot and late KVM facts
 * @host: caller-owned staging output, never the shared page
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
