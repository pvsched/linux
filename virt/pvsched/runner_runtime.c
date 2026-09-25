// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <linux/err.h>
#include <linux/sched/cputime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>
#include "runner_runtime.h"

static enum hrtimer_restart pvsched_cutoff_timer(struct hrtimer *timer)
{
	/* The interrupt's only job is to force a prompt nonfast KVM exit. */
	return HRTIMER_NORESTART;
}

static bool pvsched_mode_supported(u32 mode_flags)
{
	return !(mode_flags & PVSCHED_RUNNER_MODE_UNSUPPORTED);
}

static bool pvsched_mode_nested(u32 mode_flags)
{
	return mode_flags & PVSCHED_RUNNER_MODE_NESTED;
}

static void
pvsched_reset_host_initiated_boost_locked(struct pvsched_runner_runtime *runtime)
{
	runtime->interrupt_ticket = 0;
	runtime->reasons = 0;
}

static u64 pvsched_budget_remaining(const struct pvsched_budget *budget)
{
	return budget->throttled || budget->debt_ns >= budget->limit_ns ? 0 :
	       budget->limit_ns - budget->debt_ns;
}

static void pvsched_arm_cutoff_locked(struct pvsched_runner_runtime *runtime)
{
	struct pvsched_runtime_accounting *accounting = &runtime->accounting;
	u64 remaining;

	switch (accounting->domain) {
	case PVSCHED_BUDGET_DRAIN_CS_GENERIC:
		remaining = min(pvsched_budget_remaining(&accounting->accounting.cs),
				pvsched_budget_remaining(&accounting->accounting.generic));
		break;
	case PVSCHED_BUDGET_DRAIN_GENERIC:
		remaining = pvsched_budget_remaining(&accounting->accounting.generic);
		break;
	default:
		return;
	}

	/*
	 * A held boost ends at its grace deadline even with budget left.  Only
	 * a boost that is held right now is capped: a stale deadline must never
	 * force an exit, or the guest could not run to change what it asks for.
	 */
	if (runtime->idle_holding) {
		u64 now = ktime_get_ns();

		remaining = min(remaining, runtime->idle_hold_end_ns > now ?
				runtime->idle_hold_end_ns - now : 0);
	}

	/* A latched budget yields zero: request an immediate nonfast exit. */
	hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(remaining),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

/*
 * A failure of the built-in default policy is a kernel bug and warns.  A
 * custom policy's failure is only logged: a WARN would panic a host that
 * sets panic_on_warn over an error in a loadable policy.
 */
#define pvsched_policy_bug_on(runtime, cond)				\
	(pvsched_policy_builtin((runtime)->ops) ? WARN_ON_ONCE(cond) : !!(cond))

#define pvsched_policy_log(runtime, fmt, ...)				\
do {									\
	if (pvsched_policy_builtin((runtime)->ops))			\
		pr_err_ratelimited("pvsched: runner %d policy %s: " fmt,	\
				   task_pid_nr((runtime)->attachment.task),	\
				   (runtime)->ops->name, ##__VA_ARGS__);	\
	else								\
		pr_warn_ratelimited("pvsched: runner %d policy %s: " fmt,	\
				    task_pid_nr((runtime)->attachment.task),	\
				    (runtime)->ops->name, ##__VA_ARGS__);	\
} while (0)

/*
 * A custom policy's map() or apply() error reaches reconcile returns,
 * last_fault and QUERY.  Pass on only a plain errno without a meaning of its
 * own there: a positive value, or one that pvsched or its ABI gives a meaning
 * (an external owner, exit, shutdown, a stale binding, a revocation, a
 * rejected negotiation or a refused reattach), becomes -EINVAL.  The
 * built-in default keeps its codes.
 */
static int pvsched_policy_errno(const struct pvsched_runner_runtime *runtime,
				int ret)
{
	if (pvsched_policy_builtin(runtime->ops))
		return ret;
	switch (ret) {
	case -EOWNERDEAD:
	case -ESRCH:
	case -ESHUTDOWN:
	case -ESTALE:
	case -EOPNOTSUPP:
	case -EPROTO:
	case -EBUSY:
		return -EINVAL;
	default:
		return ret < 0 && ret >= -MAX_ERRNO ? ret : -EINVAL;
	}
}

/* Without an ownership check, the policy cannot see an external owner. */
static bool pvsched_policy_owned(struct pvsched_runner_runtime *runtime)
{
	return !runtime->ops->owned || runtime->ops->owned(&runtime->ctx);
}

static bool pvsched_baseline_applied(const struct pvsched_runner_runtime *runtime)
{
	return !memcmp(runtime->applied, runtime->baseline,
		       runtime->ops->params_size);
}

/*
 * Apply @params, labelled @class, through the policy.  Ownership is
 * rechecked under the runner lock before the same-bytes skip.
 */
static int pvsched_apply(struct pvsched_runner_runtime *runtime,
			 const void *params, enum pvsched_boost_class class)
{
	u32 size = runtime->ops->params_size;
	int ret;

	if (!pvsched_policy_owned(runtime))
		return -EOWNERDEAD;
	if (memcmp(runtime->applied, params, size)) {
		ret = runtime->ops->apply(&runtime->ctx, params);
		if (ret)
			return pvsched_policy_errno(runtime, ret);
		memcpy(runtime->applied, params, size);
	}
	runtime->applied_class = class;
	return 0;
}

static void pvsched_idle_hold_reset(struct pvsched_runner_runtime *runtime)
{
	runtime->idle_hold_end_ns = 0;
	runtime->idle_holding = false;
}

/*
 * An idle guest is heading to a halt, where the HLT exit and HALT keep it
 * boosted until it blocks, or has just woken and still runs its idle task.
 * A policy may keep its boost meanwhile rather than let a host task take
 * the CPU; pvsched bounds that to one short grace per idle episode and
 * charges it to the CS budget.  An episode ends when the guest leaves
 * idle, at a halt or wake, or on a return to the VMM.  Returns whether the
 * hold may be used now.
 */
static bool pvsched_idle_hold_offered(struct pvsched_runner_runtime *runtime,
				      enum pvsched_reconcile_event event,
				      const struct pvsched_default_guest_area *input,
				      bool throttled, u64 now)
{
	u64 end = runtime->idle_hold_end_ns;

	if (!(input->task_intent.flags & PVSCHED_INTENT_FLAG_IDLE)) {
		pvsched_idle_hold_reset(runtime);
		return false;
	}
	/* A grace that has passed stays spent until the guest leaves idle. */
	return (event == PVSCHED_RECONCILE_VMEXIT ||
		event == PVSCHED_RECONCILE_CANCEL) &&
	       runtime->idle_hold_ns && !throttled && !(end && now >= end);
}

/*
 * Ask the policy for the parameters and class to apply next.  The class is
 * trusted, but it must be one the budgets allow now, and a hold must keep
 * the elevated value applied, and only when offered.
 */
static int pvsched_map_locked(struct pvsched_runner_runtime *runtime,
			      const struct pvsched_map_input *in,
			      enum pvsched_boost_class *class, bool *held)
{
	u32 size = runtime->ops->params_size;
	u32 flags = 0;
	int ret;

	memset(runtime->out, 0, size);
	*class = PVSCHED_CLASS_BASELINE;
	ret = runtime->ops->map(&runtime->ctx, in, runtime->out, class, &flags);
	if (ret)
		return pvsched_policy_errno(runtime, ret);
	if (flags & ~PVSCHED_MAP_HELD)
		return -EINVAL;
	*held = flags & PVSCHED_MAP_HELD;
	if (*held) {
		if (!in->hold_offered ||
		    runtime->applied_class == PVSCHED_CLASS_BASELINE ||
		    memcmp(runtime->out, runtime->applied, size))
			return -EINVAL;
		*class = runtime->applied_class;
	}
	switch (*class) {
	case PVSCHED_CLASS_BASELINE:
		return 0;
	case PVSCHED_CLASS_TASK:
		return in->generic_throttled ? -EINVAL : 0;
	case PVSCHED_CLASS_CS:
		return in->cs_throttled || in->generic_throttled ? -EINVAL : 0;
	default:
		return -EINVAL;
	}
}

static enum pvsched_budget_account
pvsched_class_account(enum pvsched_boost_class class)
{
	switch (class) {
	case PVSCHED_CLASS_CS:
		return PVSCHED_BUDGET_DRAIN_CS_GENERIC;
	case PVSCHED_CLASS_TASK:
		return PVSCHED_BUDGET_DRAIN_GENERIC;
	default:
		return PVSCHED_BUDGET_REFILL;
	}
}

static struct pvsched_runtime_sample
pvsched_sample(struct pvsched_runner_runtime *runtime, bool force_runtime)
{
	bool drain = runtime->accounting.domain != PVSCHED_BUDGET_REFILL;

	/* Avoid the accurate runtime accessor on the unboosted REFILL hot path. */
	return (struct pvsched_runtime_sample) {
		.wall_ns = ktime_get_ns(),
		.runtime_ns = drain || force_runtime ?
			      task_sched_runtime(runtime->attachment.task) : 0,
		.runtime_valid = drain || force_runtime,
	};
}

/* pvsched_apply() refuses with -EOWNERDEAD once another owner took over. */
static int pvsched_restore_locked(struct pvsched_runner_runtime *runtime)
{
	return pvsched_apply(runtime, runtime->baseline, PVSCHED_CLASS_BASELINE);
}

/* Stop service: close admission, drop the cutoff and host-initiated boost. */
static void pvsched_deactivate_locked(struct pvsched_runner_runtime *runtime)
{
	pvsched_attachment_disable_locked(&runtime->attachment);
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
	pvsched_reset_host_initiated_boost_locked(runtime);
}

static int pvsched_fail_locked(struct pvsched_runner_runtime *runtime, int ret)
{
	int restore_ret;

	/* External ownership is neither our fault nor ours to restore. */
	if (ret == -EOWNERDEAD) {
		pvsched_deactivate_locked(runtime);
		return ret;
	}
	runtime->last_fault = ret;
	/* One bounded restore attempt; failure is terminal until teardown. */
	pvsched_deactivate_locked(runtime);
	restore_ret = pvsched_restore_locked(runtime);
	if (restore_ret == -EOWNERDEAD)
		return ret;
	if (restore_ret) {
		pvsched_policy_bug_on(runtime, true);
		runtime->restore_failed = true;
		runtime->last_fault = restore_ret;
	}
	return ret;
}

/*
 * Bind the policy: take an entry reference, allocate its buffers and
 * capture its baseline.  One zeroed allocation holds the baseline, the
 * applied value, the map() output and the private state, in that order.
 */
static int pvsched_runner_policy_bind(struct pvsched_runner_runtime *runtime,
				      struct pvsched_policy_entry *entry)
{
	const struct pvsched_policy_ops *ops = entry->ops;
	size_t params = ALIGN(ops->params_size, sizeof(u64));
	void *buffers;
	int ret;

	pvsched_policy_entry_get(entry);
	runtime->entry = entry;
	runtime->ops = ops;
	buffers = kzalloc(3 * params + ops->priv_size, GFP_KERNEL_ACCOUNT);
	if (!buffers)
		return -ENOMEM;
	runtime->baseline = buffers;
	runtime->applied = buffers + params;
	runtime->out = buffers + 2 * params;
	runtime->ctx = (struct pvsched_policy_ctx) {
		.task = runtime->attachment.task,
		.priv = ops->priv_size ? buffers + 3 * params : NULL,
		.baseline = runtime->baseline,
		.applied = runtime->applied,
	};
	/* Any refusal reads as the default's: this state is not supported. */
	ret = ops->capture_baseline(&runtime->ctx, runtime->baseline);
	if (ret)
		return -EOPNOTSUPP;
	memcpy(runtime->applied, runtime->baseline, ops->params_size);
	runtime->applied_class = PVSCHED_CLASS_BASELINE;
	return 0;
}

int pvsched_runner_runtime_init(struct pvsched_runner_runtime *runtime,
				struct task_struct *task, u64 generation,
				struct pvsched_policy_entry *entry,
				u64 cs_budget_ns, u64 generic_budget_ns,
				struct pvsched_ticket_owner *ticket_owner,
				bool deboost_notify)
{
	int ret;

	if (!ticket_owner || !entry)
		return -EINVAL;
	memset(runtime, 0, sizeof(*runtime));
	hrtimer_setup(&runtime->cutoff_timer, pvsched_cutoff_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	ret = pvsched_attachment_init(&runtime->attachment, task);
	if (ret)
		return ret;
	runtime->ticket_owner = ticket_owner;
	runtime->deboost_notify = deboost_notify;
	ret = pvsched_runner_policy_bind(runtime, entry);
	if (ret) {
		/* Nothing is applied or visible yet: release, do not close. */
		pvsched_runner_runtime_release(runtime);
		return ret;
	}
	ret = pvsched_runtime_accounting_init(&runtime->accounting, cs_budget_ns,
					      generic_budget_ns, ktime_get_ns());
	if (ret)
		goto err_put;
	ret = pvsched_attachment_publish(&runtime->attachment, NULL, NULL);
	if (!ret)
		return 0;

err_put:
	pvsched_runner_runtime_abort(runtime);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_init);

void pvsched_runner_runtime_gate(struct pvsched_runner_runtime *runtime)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	runtime->closing = true;
	pvsched_attachment_disable_locked(&runtime->attachment);
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_gate);

void pvsched_runner_runtime_finish_close(struct pvsched_runner_runtime *runtime)
{
	unsigned long flags;
	int ret;

	/* Admission must be disabled before synchronously waiting for callbacks. */
	WARN_ON_ONCE(!runtime->closing);
	hrtimer_cancel(&runtime->cutoff_timer);

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	/* sched_process_exit makes scheduler restoration both unsafe and moot. */
	if (!runtime->attachment.exited) {
		runtime->attachment.restore_owed = false;
		if (!pvsched_baseline_applied(runtime)) {
			ret = pvsched_restore_locked(runtime);
			if (ret && ret != -EOWNERDEAD) {
				/* Activation wiring must copy this fault to persistent state. */
				runtime->last_fault = ret;
				pvsched_policy_log(runtime,
						   "failed to restore during teardown: %d\n",
						   ret);
			}
		}
	}
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	pvsched_attachment_unhash(&runtime->attachment);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_finish_close);

void pvsched_runner_runtime_release(struct pvsched_runner_runtime *runtime)
{
	pvsched_attachment_release(&runtime->attachment);
	kfree(runtime->baseline);
	runtime->baseline = NULL;
	runtime->applied = NULL;
	runtime->out = NULL;
	if (runtime->entry) {
		pvsched_policy_entry_put(runtime->entry);
		runtime->entry = NULL;
	}
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_release);

void pvsched_runner_runtime_destroy(struct pvsched_runner_runtime *runtime)
{
	pvsched_runner_runtime_abort(runtime);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_destroy);

void pvsched_runner_runtime_abort(struct pvsched_runner_runtime *runtime)
{
	bool hashed = runtime->attachment.task_hashed ||
		      runtime->attachment.vcpu_hashed;

	pvsched_runner_runtime_gate(runtime);
	pvsched_runner_runtime_finish_close(runtime);
	if (hashed)
		pvsched_attachment_drain();
	pvsched_runner_runtime_release(runtime);
}

/* A guest ack of the live ticket retires it. */
static void
pvsched_ack_ticket_locked(struct pvsched_runner_runtime *runtime,
			  const struct pvsched_default_guest_area *guest)
{
	if (runtime->interrupt_ticket &&
	    READ_ONCE(guest->interrupt_ack) == runtime->interrupt_ticket)
		runtime->interrupt_ticket = 0;
}

static int
pvsched_runner_reconcile_locked(struct pvsched_runner_runtime *runtime,
				enum pvsched_reconcile_event event,
				const struct pvsched_runner_event_input *event_input)
{
	enum pvsched_boost_class class = PVSCHED_CLASS_BASELINE;
	struct pvsched_runtime_sample sample;
	const struct pvsched_default_guest_area *input;
	struct pvsched_map_input map_input;
	const void *target;
	enum pvsched_budget_account domain;
	bool old_cs_throttled, old_generic_throttled, throttled;
	bool fresh_snapshot;
	bool hold_offered = false;
	bool idle_hold = false;
	u64 runtime_ns, *runtime_ptr = NULL;
	int deferred_fault;
	int event_ret = 0;
	int ret;

	/* Only target-local hooks advance the runner-owned position. */
	switch (event) {
	case PVSCHED_RECONCILE_RUN_ENTER:
	case PVSCHED_RECONCILE_UNHALT:
	case PVSCHED_RECONCILE_VMEXIT:
	case PVSCHED_RECONCILE_CANCEL:
		runtime->attachment.position = PVSCHED_RUNNER_HOST;
		break;
	case PVSCHED_RECONCILE_HALT:
		runtime->attachment.position = PVSCHED_RUNNER_BLOCKED;
		break;
	case PVSCHED_RECONCILE_RUN_LEAVE:
		runtime->attachment.position = PVSCHED_RUNNER_QEMU;
		break;
	default:
		break;
	}
	/* Mandatory cleanup precedes admission and policy selection. */
	if (runtime->attachment.exited) {
		ret = -ESRCH;
		goto out;
	}
	if (runtime->restore_owed_error) {
		/* Pay the single restore owed by a late-VMENTRY fault before work. */
		deferred_fault = runtime->restore_owed_error;
		runtime->restore_owed_error = 0;
		hrtimer_try_to_cancel(&runtime->cutoff_timer);
		ret = pvsched_restore_locked(runtime);
		if (pvsched_policy_bug_on(runtime, ret && ret != -EOWNERDEAD)) {
			runtime->restore_failed = true;
			runtime->last_fault = ret;
		}
		ret = deferred_fault;
		goto out;
	}
	if (runtime->closing) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (runtime->restore_failed) {
		ret = runtime->last_fault ?: -EIO;
		goto out;
	}
	if (!runtime->attachment.active) {
		ret = -ESHUTDOWN;
		goto out;
	}
	switch (event) {
	case PVSCHED_RECONCILE_RUN_ENTER:
	case PVSCHED_RECONCILE_VMEXIT:
	case PVSCHED_RECONCILE_CANCEL:
	case PVSCHED_RECONCILE_RUN_LEAVE:
	case PVSCHED_RECONCILE_HALT:
	case PVSCHED_RECONCILE_UNHALT:
	case PVSCHED_RECONCILE_INJECT:
		break;
	default:
		ret = -EINVAL;
		goto out;
	}
	if (!event_input) {
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		goto out;
	}
	input = event_input->guest;
	if (event == PVSCHED_RECONCILE_INJECT &&
	    !pvsched_mode_supported(event_input->mode_flags)) {
		/*
		 * Remote mode facts are only hints.  Ignore an unsupported INJECT
		 * and let the target's own hooks, which see fresh facts, start
		 * revocation.
		 */
		ret = 0;
		goto out;
	}
	/*
	 * An unsupported mode revokes this attachment: stop new assistance and
	 * mark revocation owed.  Local checkpoints then restore the baseline
	 * below.
	 */
	if (!pvsched_mode_supported(event_input->mode_flags)) {
		pvsched_reset_host_initiated_boost_locked(runtime);
		runtime->revoke_pending = true;
	}
	if (runtime->revoke_pending) {
		if (event == PVSCHED_RECONCILE_INJECT) {
			ret = 0;
			goto out;
		}
		pr_warn_ratelimited("pvsched: runner %d: unsupported guest execution mode, revoking the attachment\n",
				    task_pid_nr(runtime->attachment.task));
		pvsched_deactivate_locked(runtime);
		ret = pvsched_restore_locked(runtime);
		runtime->revoke_pending = false;
		if (pvsched_policy_bug_on(runtime, ret && ret != -EOWNERDEAD)) {
			runtime->restore_failed = true;
			runtime->last_fault = ret;
		}
		if (!ret)
			ret = -EOPNOTSUPP;
		goto out;
	}
	/* Ineligible producer facts must not touch the target's open interval. */
	if (event == PVSCHED_RECONCILE_INJECT &&
	    (runtime->nested_l2 ||
	     pvsched_mode_nested(event_input->mode_flags) ||
	     (runtime->attachment.position != PVSCHED_RUNNER_HOST &&
	      runtime->attachment.position != PVSCHED_RUNNER_BLOCKED) ||
	     runtime->accounting.phase != PVSCHED_RUNTIME_HOST ||
	     !runtime->last_guest_area_valid)) {
		ret = 0;
		goto out;
	}
	sample = pvsched_sample(runtime, false);
	if (event == PVSCHED_RECONCILE_VMEXIT ||
	    (event == PVSCHED_RECONCILE_RUN_LEAVE &&
	     runtime->accounting.phase == PVSCHED_RUNTIME_GUEST))
		ret = pvsched_runtime_guest_close(&runtime->accounting, sample);
	else
		ret = pvsched_runtime_settle(&runtime->accounting, sample);
	if (ret)
		goto internal_fault;
	old_cs_throttled = runtime->accounting.accounting.cs.throttled;
	old_generic_throttled =
		runtime->accounting.accounting.generic.throttled;
	throttled = old_cs_throttled || old_generic_throttled;
	if (throttled)
		pvsched_reset_host_initiated_boost_locked(runtime);
	if (event == PVSCHED_RECONCILE_UNHALT) {
		/*
		 * Balance sleep; do not manufacture a fresh post-wake boost.  A
		 * wake starts a new idle episode: until the guest switches to
		 * the woken task, the idle hold may keep the halt boost.
		 */
		pvsched_idle_hold_reset(runtime);
		if (!throttled) {
			target = runtime->applied;
			class = runtime->applied_class;
		} else {
			target = runtime->baseline;
		}
		goto selected;
	}

	if (event == PVSCHED_RECONCILE_RUN_LEAVE) {
		pvsched_idle_hold_reset(runtime);
		/* GUEST may remain open on a terminal path lacking IRQ-on VMEXIT. */
		hrtimer_try_to_cancel(&runtime->cutoff_timer);
		runtime->reasons = 0;
		target = runtime->baseline;
		goto selected;
	}

	if (event != PVSCHED_RECONCILE_INJECT)
		runtime->nested_l2 = pvsched_mode_nested(event_input->mode_flags);
	/* Only a non-nested local event supplies a fresh guest snapshot. */
	fresh_snapshot = !runtime->nested_l2 && event != PVSCHED_RECONCILE_INJECT;
	if (fresh_snapshot && input)
		pvsched_ack_ticket_locked(runtime, input);
	if (!runtime->nested_l2) {
		switch (event) {
		case PVSCHED_RECONCILE_VMEXIT:
			/*
			 * Boost a halting vCPU from its HLT exit, not only at
			 * HALT: a deboost here would let a host task preempt it
			 * before it blocks, and a guest timer that expires while
			 * it waits raises no host event to boost it again.
			 */
			if (!(event_input->mode_flags & PVSCHED_RUNNER_MODE_HLT_EXIT))
				break;
			fallthrough;
		case PVSCHED_RECONCILE_HALT:
			/* The halt ends this idle episode's grace. */
			pvsched_idle_hold_reset(runtime);
			if (!throttled)
				runtime->reasons |= PVSCHED_RUNNER_REASON_HALT;
			break;
		case PVSCHED_RECONCILE_INJECT:
			if (!throttled)
				runtime->reasons |= PVSCHED_RUNNER_REASON_INJECT;
			input = &runtime->last_guest_area;
			if (runtime->attachment.position == PVSCHED_RUNNER_BLOCKED) {
				ret = 0;
				goto out;
			}
			break;
		default:
			break;
		}
	}

	if (runtime->nested_l2) {
		/*
		 * The shared CS word describes the preempt-disabled L2 entry path,
		 * not guest intent throughout L2.  Reuse the last non-nested
		 * selection snapshot rather than manufacturing a CS boost.
		 */
		if (!runtime->last_guest_area_valid) {
			/* Mid-L2 attachment has no snapshot, so use baseline. */
			target = runtime->baseline;
			event_ret = -EINVAL;
			goto selected;
		}
		input = &runtime->last_guest_area;
	}
	if (!input) {
		WARN_ON_ONCE(1);
		runtime->last_guest_area_valid = false;
		target = runtime->baseline;
		event_ret = -EINVAL;
		goto selected;
	}
	if (fresh_snapshot) {
		runtime->last_guest_area = *input;
		runtime->last_guest_area_valid = pvsched_default_guest_valid(input);
		if (!runtime->last_guest_area_valid) {
			/* Malformed guest data is event-local, not a runner fault. */
			target = runtime->baseline;
			event_ret = -EINVAL;
			goto selected;
		}
		hold_offered = pvsched_idle_hold_offered(runtime, event, input,
							 throttled,
							 sample.wall_ns);
	}
	map_input = (struct pvsched_map_input) {
		.event = event,
		.guest = input,
		.reasons = runtime->reasons,
		.ticket_live = runtime->interrupt_ticket,
		.cs_throttled = old_cs_throttled,
		.generic_throttled = old_generic_throttled,
		.hold_offered = hold_offered,
	};
	ret = pvsched_map_locked(runtime, &map_input, &class, &idle_hold);
	if (ret)
		goto apply_fault;
	target = runtime->out;
	if (idle_hold && !runtime->idle_hold_end_ns)
		runtime->idle_hold_end_ns = sample.wall_ns + runtime->idle_hold_ns;

selected:
	domain = idle_hold ? PVSCHED_BUDGET_DRAIN_CS_GENERIC :
		 pvsched_class_account(class);
	/*
	 * Commit can fail only outside HOST, and the settle or close above
	 * leaves HOST; check before the setter rather than after it.
	 */
	if (WARN_ON_ONCE(runtime->accounting.phase != PVSCHED_RUNTIME_HOST)) {
		ret = -EINVAL;
		goto internal_fault;
	}
	if (runtime->accounting.domain == PVSCHED_BUDGET_REFILL &&
	    domain != PVSCHED_BUDGET_REFILL) {
		runtime_ns = task_sched_runtime(runtime->attachment.task);
		runtime_ptr = &runtime_ns;
	}
	ret = pvsched_apply(runtime, target, class);
	if (ret)
		goto apply_fault;
	/* Commit is infallible after the prevalidation above and a valid setter. */
	WARN_ON_ONCE(pvsched_runtime_commit(&runtime->accounting, domain,
					    runtime_ptr));
	runtime->idle_holding = idle_hold;
	ret = event_ret;
	goto out;

apply_fault:
	pvsched_idle_hold_reset(runtime);
	ret = pvsched_fail_locked(runtime, ret);
	goto out;
internal_fault:
	pvsched_idle_hold_reset(runtime);
	/* An internal failure must not leave an owned elevated value applied. */
	ret = pvsched_fail_locked(runtime, ret);
out:
	return ret;
}

int pvsched_runner_reconcile(struct pvsched_runner_runtime *runtime,
			     enum pvsched_reconcile_event event,
			     const struct pvsched_runner_event_input *event_input)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	ret = pvsched_runner_reconcile_locked(runtime, event, event_input);
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_reconcile);

/*
 * Whether the guest's CS word (nonzero, reserved bits clear) and the budget
 * latches allow a CS boost.  Validation checks the rest of the guest area at
 * the next checkpoint; the armed cutoff timer bounds the boost until then
 * either way.
 */
static bool
pvsched_guest_cs_selects_cs(const struct pvsched_runner_runtime *runtime,
			      const struct pvsched_runner_vmentry_input *input,
			      bool guest_valid)
{
	u64 cs;

	if (!guest_valid || runtime->accounting.accounting.cs.throttled ||
	    runtime->accounting.accounting.generic.throttled)
		return false;
	cs = le64_to_cpu(input->guest.cs_state);
	return cs && !(cs & PVSCHED_CS_RESERVED_MASK);
}

/*
 * Stage the applied-state feedback and the live ticket for the page.
 * @hint adds the cooperative deboost kick hint when it is enabled.
 */
static void
pvsched_publish_applied_locked(const struct pvsched_runner_runtime *runtime,
			       struct pvsched_host_area *host, bool hint)
{
	union pvsched_applied_state state = { };

	state.boost = runtime->applied_class;
	if (hint && runtime->deboost_notify)
		state.hints |= PVSCHED_HINT_KICK_DEBOOST;
	if (runtime->accounting.accounting.cs.throttled)
		state.flags |= PVSCHED_APPLIED_CS_THROTTLED;
	if (runtime->accounting.accounting.generic.throttled)
		state.flags |= PVSCHED_APPLIED_TOTAL_THROTTLED;
	WRITE_ONCE(host->applied_state.raw, state.raw);
	WRITE_ONCE(host->default_area.interrupt_ticket,
		   runtime->interrupt_ticket);
}

static int
pvsched_runner_vmentry_locked(struct pvsched_runner_runtime *runtime,
			      const struct pvsched_runner_vmentry_input *input,
			      struct pvsched_host_area *host)
{
	struct pvsched_runtime_sample sample;
	bool had_host_reason, nested;
	bool was_host;
	int ret = -ESHUTDOWN;

	runtime->attachment.position = PVSCHED_RUNNER_GUEST;
	nested = pvsched_mode_nested(input->mode_flags);
	runtime->nested_l2 = nested;
	if (runtime->restore_owed_error && !runtime->closing &&
	    !runtime->attachment.exited) {
		/* Fast reentry cannot restore, but must re-arm the immediate kick. */
		hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(0),
			      HRTIMER_MODE_REL_PINNED_HARD);
		ret = runtime->restore_owed_error;
	} else if (runtime->attachment.active && !runtime->closing &&
		   !runtime->attachment.exited &&
		   !runtime->restore_failed) {
		was_host = runtime->accounting.phase == PVSCHED_RUNTIME_HOST;
		sample = pvsched_sample(runtime, false);
		ret = pvsched_runtime_guest_start(&runtime->accounting, sample);
		if (ret) {
			/*
			 * Late VMENTRY follows KVM's final reschedule check, so a setter
			 * cannot safely take effect before entry.  Defer restoration.
			 */
			pvsched_attachment_disable_locked(&runtime->attachment);
			runtime->last_fault = ret;
			runtime->restore_owed_error = ret;
			hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(0),
				      HRTIMER_MODE_REL_PINNED_HARD);
		} else if (was_host) {
			/* Arm only for the HOST -> GUEST transition, not fast reentry. */
			pvsched_arm_cutoff_locked(runtime);
		}
		if (ret)
			goto unlock;
		had_host_reason = runtime->reasons || runtime->interrupt_ticket;
		if (runtime->revoke_pending ||
		    !pvsched_mode_supported(input->mode_flags)) {
			/* Entry proceeds; a safe slow checkpoint performs restoration. */
			runtime->revoke_pending = true;
			/* This also clears the ticket, so zero is published. */
			pvsched_reset_host_initiated_boost_locked(runtime);
			hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(0),
				      HRTIMER_MODE_REL_PINNED_HARD);
			pvsched_publish_applied_locked(runtime, host, false);
			ret = 0;
			goto unlock;
		}
		if (!nested) {
			pvsched_ack_ticket_locked(runtime, &input->guest);
			if (input->interrupt_ready) {
				if (!runtime->interrupt_ticket &&
				    !runtime->accounting.accounting.cs.throttled &&
				    !runtime->accounting.accounting.generic.throttled &&
				    !(input->mode_flags & PVSCHED_RUNNER_MODE_NO_TICKET) &&
				    runtime->ticket_owner->last_ticket != U64_MAX) {
					runtime->interrupt_ticket =
						++runtime->ticket_owner->last_ticket;
				}
			}
			runtime->reasons = 0;
		}
		if (runtime->accounting.accounting.cs.throttled ||
		    runtime->accounting.accounting.generic.throttled) {
			pvsched_reset_host_initiated_boost_locked(runtime);
		}
		/*
		 * The last host-initiated reason just retired while a CS boost
		 * is applied: force an exit so a checkpoint can deboost, unless
		 * the guest's own CS state still asks for that boost.
		 */
		if (had_host_reason && !runtime->reasons &&
		    !runtime->interrupt_ticket &&
		    runtime->applied_class == PVSCHED_CLASS_CS &&
		    !pvsched_guest_cs_selects_cs(runtime, input, !nested)) {
			hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(0),
				      HRTIMER_MODE_REL_PINNED_HARD);
		}
		if (!nested) {
			pvsched_publish_applied_locked(runtime, host, true);
		}
	}
unlock:
	return ret;
}

int pvsched_runner_vmentry(struct pvsched_runner_runtime *runtime,
			   const struct pvsched_runner_vmentry_input *input,
			   struct pvsched_host_area *host)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	ret = pvsched_runner_vmentry_locked(runtime, input, host);
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_vmentry);

struct pvsched_runner_local_reconcile_ctx {
	enum pvsched_reconcile_event event;
	const struct pvsched_runner_event_input *input;
	int ret;
};

static void
pvsched_runner_local_reconcile_visit(struct pvsched_attachment *attachment,
				     enum pvsched_attachment_visit_kind kind,
				     void *data)
{
	struct pvsched_runner_local_reconcile_ctx *ctx = data;
	struct pvsched_runner_runtime *runtime =
		container_of(attachment, struct pvsched_runner_runtime, attachment);

	/* A failed binding is reported, not recorded as the runner's fault. */
	if (kind == PVSCHED_ATTACHMENT_VISIT_BINDING_FAILED)
		runtime->restore_owed_error = -ESTALE;
	ctx->ret = pvsched_runner_reconcile_locked(runtime, ctx->event,
						   ctx->input);
}

int pvsched_runner_local_reconcile(const void *vcpu_key,
				   enum pvsched_reconcile_event event,
				   const struct pvsched_runner_event_input *input)
{
	struct pvsched_runner_local_reconcile_ctx ctx = {
		.event = event,
		.input = input,
		.ret = -ENOENT,
	};
	enum pvsched_runner_position position;

	switch (event) {
	case PVSCHED_RECONCILE_HALT:
		position = PVSCHED_RUNNER_BLOCKED;
		break;
	case PVSCHED_RECONCILE_RUN_LEAVE:
		position = PVSCHED_RUNNER_QEMU;
		break;
	default:
		position = PVSCHED_RUNNER_HOST;
		break;
	}
	pvsched_attachment_local_visit(vcpu_key, position,
		input ? input->mode_flags : 0,
		pvsched_runner_local_reconcile_visit, &ctx);
	return ctx.ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_local_reconcile);

int pvsched_runner_cleanup_reconcile(struct task_struct *task,
				     enum pvsched_reconcile_event event,
				     const struct pvsched_runner_event_input *input)
{
	struct pvsched_runner_local_reconcile_ctx ctx = {
		.event = event,
		.input = input,
		.ret = -ENOENT,
	};

	pvsched_attachment_cleanup_visit(task,
		pvsched_runner_local_reconcile_visit, &ctx);
	return ctx.ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_cleanup_reconcile);

struct pvsched_runner_local_vmentry_ctx {
	const struct pvsched_runner_vmentry_input *input;
	struct pvsched_host_area *host;
	int ret;
};

static void
pvsched_runner_local_vmentry_visit(struct pvsched_attachment *attachment,
				   enum pvsched_attachment_visit_kind kind,
				   void *data)
{
	struct pvsched_runner_local_vmentry_ctx *ctx = data;
	struct pvsched_runner_runtime *runtime =
		container_of(attachment, struct pvsched_runner_runtime, attachment);

	/* A failed binding is reported, not recorded as the runner's fault. */
	if (kind == PVSCHED_ATTACHMENT_VISIT_BINDING_FAILED)
		runtime->restore_owed_error = -ESTALE;
	ctx->ret = pvsched_runner_vmentry_locked(runtime, ctx->input, ctx->host);
}

int pvsched_runner_local_vmentry(const void *vcpu_key,
				 const struct pvsched_runner_vmentry_input *input,
				 struct pvsched_host_area *host)
{
	struct pvsched_runner_local_vmentry_ctx ctx = {
		.input = input,
		.host = host,
		.ret = -ENOENT,
	};

	pvsched_attachment_local_visit(vcpu_key, PVSCHED_RUNNER_GUEST,
		input->mode_flags, pvsched_runner_local_vmentry_visit, &ctx);
	return ctx.ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_local_vmentry);

int pvsched_runner_cleanup_vmentry(struct task_struct *task,
				   const struct pvsched_runner_vmentry_input *input,
				   struct pvsched_host_area *host)
{
	struct pvsched_runner_local_vmentry_ctx ctx = {
		.input = input,
		.host = host,
		.ret = -ENOENT,
	};

	pvsched_attachment_cleanup_visit(task,
		pvsched_runner_local_vmentry_visit, &ctx);
	return ctx.ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_cleanup_vmentry);

void pvsched_runner_guest_exit_irqoff(struct pvsched_runner_runtime *runtime)
{
	/*
	 * The pinned timer belongs to this CPU, so same-CPU IRQ-off VMEXIT may
	 * try-cancel it without state_lock.  IRQ-on VMEXIT performs settlement.
	 */
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_guest_exit_irqoff);
