// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/sched/cputime.h>
#include <linux/string.h>
#include <uapi/linux/sched/types.h>
#include <kunit/visibility.h>
#include "runner_runtime.h"
#include "lifecycle.h"

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

static void pvsched_runner_inject_work(struct irq_work *work);

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

static bool pvsched_prio_equal(const struct pvsched_prio_desc *a,
			       const struct pvsched_prio_desc *b)
{
	return a->sched_policy == b->sched_policy && a->nice == b->nice &&
	       a->rt_prio == b->rt_prio;
}

static struct pvsched_prio_desc
pvsched_baseline_prio(const struct pvsched_runner_runtime *runtime)
{
	return (struct pvsched_prio_desc) {
		.sched_policy = runtime->baseline.policy,
		.nice = runtime->baseline.nice,
		.rt_prio = runtime->baseline.rt_priority,
	};
}

static bool pvsched_owned_state(const struct pvsched_runner_runtime *runtime)
{
	struct sched_task_state state;

	sched_get_task_state(runtime->attachment.task, &state);
	/*
	 * FIFO and IDLE leave fair nice latent, so compare the value last applied
	 * under NORMAL.  A default fair slice is dynamic and is compared only when
	 * ATTACH captured an explicit slice.  These checks make administrator
	 * setpriority(), chrt, or slice changes establish an external owner.
	 */
	return state.policy == runtime->applied.sched_policy &&
	       state.nice == runtime->expected_nice &&
	       state.rt_priority == runtime->applied.rt_prio &&
	       state.custom_slice == runtime->baseline.custom_slice &&
	       (!state.custom_slice ||
		state.slice_ns == runtime->baseline.slice_ns) &&
	       !state.reset_on_fork && !state.scx_active;
}

static int pvsched_apply(struct pvsched_runner_runtime *runtime,
			 const struct pvsched_prio_desc *prio)
{
	int ret;
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = prio->sched_policy,
		.sched_nice = prio->nice,
		.sched_priority = prio->rt_prio,
	};

	/*
	 * The captured slice was already clamped by __setparam_fair(); sending it
	 * back through the setter therefore restores that exact explicit value.
	 */
	if (prio->sched_policy == SCHED_NORMAL &&
	    runtime->baseline.custom_slice)
		attr.sched_runtime = runtime->baseline.slice_ns;

	/* Recheck ownership under the runner lock before the same-tuple skip. */
	if (!pvsched_owned_state(runtime))
		return -EOWNERDEAD;
	if (pvsched_prio_equal(&runtime->applied, prio))
		return 0;
#if IS_ENABLED(CONFIG_KUNIT)
	if (runtime->test_setattr)
		ret = runtime->test_setattr(runtime->attachment.task, &attr,
					    runtime->test_setattr_data);
	else
#endif
		ret = sched_setattr_nocheck_nopi(runtime->attachment.task, &attr);
	if (ret)
		return ret;
	/* Only RT policy entry clears task timer slack, so restore after FIFO. */
	if (runtime->applied.sched_policy == SCHED_FIFO &&
	    (prio->sched_policy == SCHED_NORMAL ||
	     prio->sched_policy == SCHED_IDLE))
		sched_set_task_timer_slack(runtime->attachment.task,
					   runtime->baseline.timer_slack_ns);
	/* FIFO and IDLE retain the latent fair nice rather than changing it. */
	if (prio->sched_policy == SCHED_NORMAL)
		runtime->expected_nice = prio->nice;
	return 0;
}

static void pvsched_idle_hold_reset(struct pvsched_runner_runtime *runtime)
{
	runtime->idle_hold_end_ns = 0;
	runtime->idle_holding = false;
}

/*
 * An idle guest is heading to a halt, where the HLT exit and HALT keep it
 * boosted until it blocks.  Deboosting it on the way, at another exit,
 * lets a host task preempt it while its guest timer runs on a timer the
 * host cannot see, so it would wait a whole slice.  The same applies just
 * after a wake: the guest's idle task handles the wakeup before it switches
 * to the woken task, and a deboost there lets a host task run first.  Keep
 * the applied FIFO boost instead, for at most one short grace per idle
 * episode; the caller charges it to the CS budget.  An episode ends when the
 * guest leaves idle, at a halt or wake, or on a return to the VMM.  Returns
 * true when @target was replaced.
 */
static bool pvsched_idle_hold_locked(struct pvsched_runner_runtime *runtime,
				     enum pvsched_reconcile_event event,
				     const struct pvsched_default_guest_area *input,
				     struct pvsched_prio_desc *target,
				     bool throttled, u64 now)
{
	const struct pvsched_prio_desc *applied = &runtime->applied;
	u64 end = runtime->idle_hold_end_ns;

	if (!(input->task_intent.flags & PVSCHED_INTENT_FLAG_IDLE)) {
		pvsched_idle_hold_reset(runtime);
		return false;
	}
	/* A grace that has passed stays spent until the guest leaves idle. */
	if ((event != PVSCHED_RECONCILE_VMEXIT &&
	     event != PVSCHED_RECONCILE_CANCEL) ||
	    !runtime->idle_hold_ns || throttled || (end && now >= end) ||
	    applied->sched_policy != SCHED_FIFO ||
	    (target->sched_policy == SCHED_FIFO &&
	     target->rt_prio >= applied->rt_prio))
		return false;
	if (!end)
		runtime->idle_hold_end_ns = now + runtime->idle_hold_ns;
	*target = *applied;
	return true;
}

static enum pvsched_budget_account
pvsched_charge_class(const struct pvsched_runner_runtime *runtime,
		     const struct pvsched_prio_desc *prio)
{
	/* Validation makes cs_rt_prio strictly higher than every other class. */
	if (prio->sched_policy == SCHED_FIFO &&
	    prio->rt_prio == runtime->config.cs_rt_prio)
		return PVSCHED_BUDGET_DRAIN_CS_GENERIC;
	if (prio->sched_policy == SCHED_FIFO ||
	    (prio->sched_policy == SCHED_NORMAL &&
	     prio->nice < runtime->baseline.nice))
		return PVSCHED_BUDGET_DRAIN_GENERIC;
	return PVSCHED_BUDGET_REFILL;
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
	struct pvsched_prio_desc baseline = pvsched_baseline_prio(runtime);
	int ret;

	ret = pvsched_apply(runtime, &baseline);
	if (!ret)
		runtime->applied = baseline;
	return ret;
}

/* Stop service: close admission, drop the cutoff and host-initiated boost. */
static void pvsched_deactivate_locked(struct pvsched_runner_runtime *runtime)
{
	pvsched_attachment_disable_locked(&runtime->attachment);
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
	pvsched_reset_host_initiated_boost_locked(runtime);
}

/*
 * An owned restore failed: ordinary checkpoints must not retry it, so it is
 * terminal until teardown.  @requested, when given, is the tuple the
 * failure is reported after.  Callers warn at their own site.
 */
static void
pvsched_restore_failed_locked(struct pvsched_runner_runtime *runtime,
			      const struct pvsched_prio_desc *requested,
			      int error)
{
	struct sched_task_state observed;

	runtime->restore_failed = true;
	runtime->last_fault = error;
	if (!requested)
		return;
	sched_get_task_state(runtime->attachment.task, &observed);
	pr_err_ratelimited("pvsched: runner %d restore failed after policy=%u nice=%d rt_priority=%u: observed policy=%u nice=%d rt_priority=%u error=%d\n",
			   task_pid_nr(runtime->attachment.task),
			   requested->sched_policy, requested->nice,
			   requested->rt_prio, observed.policy, observed.nice,
			   observed.rt_priority, error);
}

/* An internal failure must not leave an owned elevated tuple applied. */
static int
pvsched_internal_fault_locked(struct pvsched_runner_runtime *runtime, int ret)
{
	int restore_ret;

	runtime->last_fault = ret;
	/* One bounded restore attempt; failure is terminal until teardown. */
	pvsched_deactivate_locked(runtime);
	restore_ret = pvsched_restore_locked(runtime);
	if (WARN_ON_ONCE(restore_ret && restore_ret != -EOWNERDEAD))
		pvsched_restore_failed_locked(runtime, &runtime->applied,
					      restore_ret);
	else
		pvsched_runner_request_cleanup_locked(&runtime->attachment);
	return ret;
}

static int
pvsched_run_leave_fail_locked(struct pvsched_runner_runtime *runtime,
			      const struct pvsched_prio_desc *requested, int error)
{
	if (error == -EOWNERDEAD || !pvsched_owned_state(runtime)) {
		pvsched_runner_request_cleanup_locked(&runtime->attachment);
	} else {
		WARN_ON_ONCE(1);
		pvsched_restore_failed_locked(runtime, requested, error);
	}
	pvsched_deactivate_locked(runtime);
	return error;
}

static int
pvsched_request_fail_locked(struct pvsched_runner_runtime *runtime,
			    const struct pvsched_prio_desc *requested, int error)
{
	int restore_ret;

	hrtimer_try_to_cancel(&runtime->cutoff_timer);
	pvsched_reset_host_initiated_boost_locked(runtime);
	restore_ret = pvsched_restore_locked(runtime);
	if (restore_ret) {
		pvsched_attachment_disable_locked(&runtime->attachment);
		if (restore_ret == -EOWNERDEAD) {
			pvsched_runner_request_cleanup_locked(&runtime->attachment);
		} else {
			WARN_ON_ONCE(1);
			pvsched_restore_failed_locked(runtime, requested,
						      restore_ret);
		}
		return error;
	}

	/* The rejected tuple never owned an interval; resume from baseline. */
	WARN_ON_ONCE(pvsched_runtime_commit(&runtime->accounting,
					   PVSCHED_BUDGET_REFILL, NULL));
	runtime->last_fault = error;
	WARN_ON_ONCE(1);
	pr_err_ratelimited("pvsched: runner %d rejected policy=%u nice=%d rt_priority=%u: %d\n",
			   task_pid_nr(runtime->attachment.task),
			   requested->sched_policy, requested->nice,
			   requested->rt_prio, error);
	return error;
}

int pvsched_runner_runtime_prepare(struct pvsched_runner_runtime *runtime,
				struct task_struct *task,
				const struct pvsched_default_policy_config *config,
				u64 cs_budget_ns, u64 generic_budget_ns,
				struct pvsched_ticket_owner *ticket_owner,
				bool deboost_notify)
{
	struct pvsched_prio_desc baseline;
	int ret;

	if (!ticket_owner)
		return -EINVAL;
	memset(runtime, 0, sizeof(*runtime));
	hrtimer_setup(&runtime->cutoff_timer, pvsched_cutoff_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	runtime->inject_work = IRQ_WORK_INIT_HARD(pvsched_runner_inject_work);
	ret = pvsched_attachment_init(&runtime->attachment, task);
	if (ret)
		return ret;
	runtime->config = *config;
	runtime->ticket_owner = ticket_owner;
	runtime->deboost_notify = deboost_notify;
	sched_get_task_state(task, &runtime->baseline);
	baseline = pvsched_baseline_prio(runtime);
	runtime->applied = baseline;
	runtime->expected_nice = runtime->baseline.nice;
	if (runtime->baseline.policy != SCHED_NORMAL ||
	    runtime->baseline.reset_on_fork || runtime->baseline.scx_active) {
		ret = -EOPNOTSUPP;
		goto err_put;
	}
	ret = pvsched_runtime_accounting_init(&runtime->accounting, cs_budget_ns,
					      generic_budget_ns, ktime_get_ns());
	if (ret)
		goto err_put;
	return 0;

err_put:
	pvsched_runner_runtime_release(runtime);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_prepare);

int pvsched_runner_runtime_publish(struct pvsched_runner_runtime *runtime,
				   pvsched_attachment_commit_fn commit, void *data)
{
	return pvsched_attachment_publish(&runtime->attachment, commit, data);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_publish);

/**
 * pvsched_runner_runtime_disable() - begin one-way runtime close
 * @runtime: runtime whose open interval and admission must be closed
 *
 * Settles the open accounting interval once, marks the runtime closing and
 * disables attachment admission. A disabled runtime is never reopened.
 */
void pvsched_runner_runtime_disable(struct pvsched_runner_runtime *runtime)
{
	struct pvsched_runtime_sample sample;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	if (!runtime->closing) {
		sample = pvsched_sample(runtime, true);
		ret = pvsched_runtime_guest_close(&runtime->accounting, sample);
		WARN_ON_ONCE(ret);
	}
	runtime->closing = true;
	pvsched_attachment_disable_locked(&runtime->attachment);
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_disable);

void pvsched_runner_runtime_drain_actions(struct pvsched_runner_runtime *runtime)
{
	WARN_ON_ONCE(!runtime->closing);
	hrtimer_cancel(&runtime->cutoff_timer);
	irq_work_sync(&runtime->inject_work);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_drain_actions);

enum pvsched_runner_restore_disposition
pvsched_runner_runtime_finish_close(struct pvsched_runner_runtime *runtime,
				    bool final_attempt)
{
	struct pvsched_prio_desc baseline;
	unsigned long flags;
	enum pvsched_runner_restore_disposition disposition = PVSCHED_RESTORE_RESTORED;
	int ret;

	/* Admission must be disabled before synchronously waiting for callbacks. */
	WARN_ON_ONCE(!runtime->closing);

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	/* sched_process_exit makes scheduler restoration both unsafe and moot. */
	if (runtime->attachment.exited) {
		disposition = PVSCHED_RESTORE_EXITED;
	} else if (!pvsched_owned_state(runtime)) {
		disposition = PVSCHED_RESTORE_EXTERNAL_OWNER;
	} else if (runtime->restore_failed && !final_attempt) {
		disposition = PVSCHED_RESTORE_OWNED_FAILURE;
	} else {
		runtime->restore_owed_error = 0;
		baseline = pvsched_baseline_prio(runtime);
		if (!pvsched_prio_equal(&runtime->applied, &baseline)) {
			ret = pvsched_restore_locked(runtime);
			if (ret == -EOWNERDEAD) {
				disposition = PVSCHED_RESTORE_EXTERNAL_OWNER;
			} else if (ret) {
				runtime->last_fault = ret;
				runtime->restore_failed = true;
				disposition = PVSCHED_RESTORE_OWNED_FAILURE;
				pr_err_ratelimited("pvsched: failed to restore runner %d during teardown: %d\n",
						   task_pid_nr(runtime->attachment.task), ret);
			}
		}
	}
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	return disposition;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_finish_close);

void pvsched_runner_runtime_unhash(struct pvsched_runner_runtime *runtime)
{
	pvsched_attachment_unhash(&runtime->attachment);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_unhash);

void pvsched_runner_runtime_release(struct pvsched_runner_runtime *runtime)
{
	pvsched_attachment_release(&runtime->attachment);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_release);

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
	struct pvsched_default_policy_result result;
	struct pvsched_runtime_sample sample;
	const struct pvsched_default_guest_area *input;
	struct pvsched_prio_desc target;
	enum pvsched_budget_account domain;
	bool old_cs_throttled, old_generic_throttled, throttled;
	bool fresh_snapshot;
	bool idle_hold = false;
	u64 runtime_ns, *runtime_ptr = NULL;
	int deferred_fault;
	int event_ret = 0;
	int ret;

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
		if (WARN_ON_ONCE(ret && ret != -EOWNERDEAD))
			pvsched_restore_failed_locked(runtime, NULL, ret);
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
		/* Otherwise the VMM only sees an INACTIVE runner. */
		pr_warn_ratelimited("pvsched: runner %d: unsupported guest execution mode, revoking the attachment\n",
				    task_pid_nr(runtime->attachment.task));
		pvsched_deactivate_locked(runtime);
		ret = pvsched_restore_locked(runtime);
		pvsched_runner_request_cleanup_locked(&runtime->attachment);
		runtime->revoke_pending = false;
		if (WARN_ON_ONCE(ret && ret != -EOWNERDEAD))
			pvsched_restore_failed_locked(runtime, NULL, ret);
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
		target = throttled ? pvsched_baseline_prio(runtime) :
				     runtime->applied;
		goto selected;
	}

	if (event == PVSCHED_RECONCILE_RUN_LEAVE) {
		pvsched_idle_hold_reset(runtime);
		/* GUEST may remain open on a terminal path lacking IRQ-on VMEXIT. */
		hrtimer_try_to_cancel(&runtime->cutoff_timer);
		runtime->reasons = 0;
		target = pvsched_baseline_prio(runtime);
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
			target = pvsched_baseline_prio(runtime);
			event_ret = -EINVAL;
			goto selected;
		}
		input = &runtime->last_guest_area;
	}
	if (!input) {
		WARN_ON_ONCE(1);
		runtime->last_guest_area_valid = false;
		target = pvsched_baseline_prio(runtime);
		event_ret = -EINVAL;
		goto selected;
	}
	ret = pvsched_default_policy_select(&runtime->config,
					    runtime->baseline.nice, input,
					    old_cs_throttled,
					    old_generic_throttled, &result);
	if (ret) {
		/* Malformed guest data is event-local, not a runner fault. */
		if (fresh_snapshot) {
			runtime->last_guest_area = *input;
			runtime->last_guest_area_valid = false;
		}
		target = pvsched_baseline_prio(runtime);
		event_ret = ret;
	} else {
		if (fresh_snapshot) {
			runtime->last_guest_area = *input;
			runtime->last_guest_area_valid = true;
		}
		target = result.prio;
		if ((runtime->reasons || runtime->interrupt_ticket) &&
		    !throttled)
			target = (struct pvsched_prio_desc) {
				.sched_policy = SCHED_FIFO,
				.rt_prio = runtime->config.cs_rt_prio,
			};
		if (fresh_snapshot)
			idle_hold = pvsched_idle_hold_locked(runtime, event,
							     input, &target,
							     throttled,
							     sample.wall_ns);
	}

selected:
	domain = idle_hold ? PVSCHED_BUDGET_DRAIN_CS_GENERIC :
		 pvsched_charge_class(runtime, &target);
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
	ret = pvsched_apply(runtime, &target);
	if (ret)
		goto apply_fault;
	/* Commit is infallible after the prevalidation above and a valid setter. */
	WARN_ON_ONCE(pvsched_runtime_commit(&runtime->accounting, domain,
					    runtime_ptr));
	WARN_ON_ONCE((domain == PVSCHED_BUDGET_DRAIN_CS_GENERIC &&
		      old_cs_throttled) ||
		     (domain != PVSCHED_BUDGET_REFILL &&
		      old_generic_throttled));
	runtime->applied = target;
	runtime->idle_holding = idle_hold;
	ret = event_ret;
	goto out;

apply_fault:
	pvsched_idle_hold_reset(runtime);
	if (event == PVSCHED_RECONCILE_RUN_LEAVE)
		ret = pvsched_run_leave_fail_locked(runtime, &target, ret);
	else
		ret = pvsched_request_fail_locked(runtime, &target, ret);
	goto out;
internal_fault:
	pvsched_idle_hold_reset(runtime);
	ret = pvsched_internal_fault_locked(runtime, ret);
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
 * latches allow the CS FIFO tuple.  Selection validates the rest of the guest
 * area at the next checkpoint; the armed cutoff timer bounds the boost until
 * then either way.
 */
static bool
pvsched_guest_cs_selects_fifo(const struct pvsched_runner_runtime *runtime,
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

	state.task = runtime->applied;
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
		 * The last host-initiated reason just retired while CS FIFO is
		 * applied: force an exit so a checkpoint can deboost, unless the
		 * guest's own CS state still asks for that FIFO.
		 */
		if (had_host_reason && !runtime->reasons &&
		    !runtime->interrupt_ticket &&
		    runtime->applied.sched_policy == SCHED_FIFO &&
		    runtime->applied.rt_prio == runtime->config.cs_rt_prio &&
		    !pvsched_guest_cs_selects_fifo(runtime, input, !nested)) {
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

	/* INJECT is exclusively vCPU-keyed and must not mutate local facts. */
	if (event == PVSCHED_RECONCILE_INJECT)
		return -EINVAL;

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

struct pvsched_runner_remote_inject_ctx {
	u32 mode_flags;
	bool source_guest_mode;
};

static void
pvsched_runner_remote_inject_visit(struct pvsched_attachment *attachment,
				   enum pvsched_attachment_visit_kind kind,
				   void *data)
{
	struct pvsched_runner_remote_inject_ctx *ctx = data;
	struct pvsched_runner_runtime *runtime =
		container_of(attachment, struct pvsched_runner_runtime, attachment);
	struct pvsched_runner_event_input input = {
		.mode_flags = ctx->mode_flags,
	};
	int cpu;

	if (kind != PVSCHED_ATTACHMENT_VISIT_ORDINARY)
		return;
	if (attachment->position == PVSCHED_RUNNER_HOST &&
	    !task_is_runnable(attachment->task))
		return;
	if (!ctx->source_guest_mode) {
		pvsched_runner_reconcile_locked(runtime,
						PVSCHED_RECONCILE_INJECT, &input);
		return;
	}
	if (!task_is_runnable(attachment->task))
		return;
	runtime->inject_mode_flags = ctx->mode_flags;
	cpu = task_cpu(attachment->task);
	if (cpu != smp_processor_id() && cpu_online(cpu))
		irq_work_queue_on(&runtime->inject_work, cpu);
	else
		irq_work_queue(&runtime->inject_work);
}

unsigned int pvsched_runner_remote_inject(const void *vcpu_key,
					  u32 mode_flags,
					  bool source_guest_mode)
{
	struct pvsched_runner_remote_inject_ctx ctx = {
		.mode_flags = mode_flags,
		.source_guest_mode = source_guest_mode,
	};

	return pvsched_attachment_remote_visit(vcpu_key, mode_flags,
					       pvsched_runner_remote_inject_visit, &ctx);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_remote_inject);

static void pvsched_runner_inject_work(struct irq_work *work)
{
	struct pvsched_runner_runtime *runtime =
		container_of(work, struct pvsched_runner_runtime, inject_work);
	struct pvsched_runner_event_input input;
	unsigned long flags;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	if (!runtime->attachment.active ||
	    !runtime->attachment.target_mode_valid ||
	    (runtime->attachment.target_mode_flags &
	     (PVSCHED_RUNNER_MODE_NESTED | PVSCHED_RUNNER_MODE_UNSUPPORTED)) ||
	    (runtime->attachment.position != PVSCHED_RUNNER_HOST &&
	     runtime->attachment.position != PVSCHED_RUNNER_BLOCKED) ||
	    (runtime->attachment.position == PVSCHED_RUNNER_HOST &&
	     !task_is_runnable(runtime->attachment.task)))
		goto unlock;
	input = (struct pvsched_runner_event_input) {
		.mode_flags = runtime->inject_mode_flags,
	};
	pvsched_runner_reconcile_locked(runtime, PVSCHED_RECONCILE_INJECT,
					&input);
unlock:
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
}

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

void pvsched_runner_guest_exit_irqoff(struct pvsched_runner_runtime *runtime)
{
	/*
	 * The pinned timer belongs to this CPU, so same-CPU IRQ-off VMEXIT may
	 * try-cancel it without state_lock.  IRQ-on VMEXIT performs settlement.
	 */
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_guest_exit_irqoff);
