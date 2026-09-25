// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/sched/cputime.h>
#include <linux/string.h>
#include <uapi/linux/sched/types.h>

#include "runner_runtime.h"

static enum hrtimer_restart pvsched_cutoff_timer(struct hrtimer *timer)
{
	/* The interrupt's only job is to force a prompt nonfast KVM exit. */
	return HRTIMER_NORESTART;
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

	sched_get_task_state(runtime->task, &state);
	/*
	 * FIFO and IDLE leave fair nice latent, so compare the value last applied
	 * under NORMAL.  A default fair slice is dynamic and is compared only when
	 * ATTACH captured an explicit slice.  These checks make administrator
	 * setpriority(), chrt, or slice changes an external owner.
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

	/* Ownership intentionally precedes the same-tuple skip. */
	if (!pvsched_owned_state(runtime))
		return -EOWNERDEAD;
	if (pvsched_prio_equal(&runtime->applied, prio))
		return 0;
#if IS_ENABLED(CONFIG_KUNIT)
	runtime->setter_calls++;
#endif
	ret = sched_setattr_nocheck_nopi(runtime->task, &attr);
	if (ret)
		return ret;
	/* Only RT policy entry clears task timer slack, so restore after FIFO. */
	if (runtime->applied.sched_policy == SCHED_FIFO &&
	    (prio->sched_policy == SCHED_NORMAL ||
	     prio->sched_policy == SCHED_IDLE))
		sched_set_task_timer_slack(runtime->task,
					   runtime->baseline.timer_slack_ns);
	/* FIFO and IDLE retain the latent fair nice rather than changing it. */
	if (prio->sched_policy == SCHED_NORMAL)
		runtime->expected_nice = prio->nice;
	return 0;
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
			      task_sched_runtime(runtime->task) : 0,
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

static int pvsched_fail_locked(struct pvsched_runner_runtime *runtime, int ret)
{
	int restore_ret;

	runtime->active = false;
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
	/* External ownership is neither our fault nor ours to restore. */
	if (ret == -EOWNERDEAD)
		return ret;
	runtime->last_fault = ret;
	/* One bounded restore attempt; failure is terminal until teardown. */
	restore_ret = pvsched_restore_locked(runtime);
	if (restore_ret == -EOWNERDEAD)
		return ret;
	if (restore_ret) {
		WARN_ON_ONCE(1);
		runtime->restore_failed = true;
		runtime->last_fault = restore_ret;
	}
	return ret;
}

int pvsched_runner_runtime_init(struct pvsched_runner_runtime *runtime,
				struct task_struct *task, u64 generation,
				const struct pvsched_default_policy_config *config,
				u64 cs_budget_ns, u64 generic_budget_ns)
{
	struct pvsched_prio_desc baseline;
	int ret;

	memset(runtime, 0, sizeof(*runtime));
	raw_spin_lock_init(&runtime->state_lock);
	hrtimer_setup(&runtime->cutoff_timer, pvsched_cutoff_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	get_task_struct(task);
	runtime->task = task;
	runtime->generation = generation;
	runtime->config = *config;
	sched_get_task_state(task, &runtime->baseline);
	if (runtime->baseline.policy != SCHED_NORMAL ||
	    runtime->baseline.reset_on_fork || runtime->baseline.scx_active) {
		ret = -EOPNOTSUPP;
		goto err_put;
	}
	baseline = pvsched_baseline_prio(runtime);
	runtime->applied = baseline;
	runtime->expected_nice = runtime->baseline.nice;
	ret = pvsched_runtime_accounting_init(&runtime->accounting, cs_budget_ns,
					      generic_budget_ns, ktime_get_ns());
	if (ret)
		goto err_put;
	runtime->active = true;
	return 0;

err_put:
	put_task_struct(runtime->task);
	runtime->task = NULL;
	return ret;
}

void pvsched_runner_runtime_destroy(struct pvsched_runner_runtime *runtime)
{
	struct pvsched_prio_desc baseline;
	unsigned long flags;
	int ret;

	/* Close admission before synchronously waiting for a timer callback. */
	raw_spin_lock_irqsave(&runtime->state_lock, flags);
	runtime->closing = true;
	runtime->active = false;
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
	hrtimer_cancel(&runtime->cutoff_timer);

	raw_spin_lock_irqsave(&runtime->state_lock, flags);
	/* sched_process_exit makes scheduler restoration both unsafe and moot. */
	if (!runtime->exited) {
		runtime->restore_owed_error = 0;
		baseline = pvsched_baseline_prio(runtime);
		if (!pvsched_prio_equal(&runtime->applied, &baseline)) {
			ret = pvsched_restore_locked(runtime);
			if (ret && ret != -EOWNERDEAD) {
				/* Activation wiring must copy this fault to persistent state. */
				runtime->last_fault = ret;
				pr_err_ratelimited("pvsched: failed to restore runner %d during teardown: %d\n",
						   task_pid_nr(runtime->task), ret);
			}
		}
	}
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
	put_task_struct(runtime->task);
	runtime->task = NULL;
}

int pvsched_runner_reconcile(struct pvsched_runner_runtime *runtime,
			     enum pvsched_reconcile_event event,
			     const struct pvsched_default_guest_area *guest)
{
	struct pvsched_default_policy_result result;
	struct pvsched_runtime_sample sample;
	const struct pvsched_default_guest_area *input = guest;
	struct pvsched_prio_desc target;
	enum pvsched_budget_account domain;
	bool old_cs_throttled, old_generic_throttled;
	unsigned long flags;
	u64 runtime_ns, *runtime_ptr = NULL;
	int deferred_fault;
	int event_ret = 0;
	int ret;

	raw_spin_lock_irqsave(&runtime->state_lock, flags);
	/* Mandatory cleanup precedes admission and policy selection. */
	if (runtime->exited) {
		ret = -ESRCH;
		goto out;
	}
	if (runtime->restore_owed_error) {
		/* Pay the single restore owed by a late-VMENTRY fault before work. */
		deferred_fault = runtime->restore_owed_error;
		runtime->restore_owed_error = 0;
		hrtimer_try_to_cancel(&runtime->cutoff_timer);
		ret = pvsched_restore_locked(runtime);
		if (ret && ret != -EOWNERDEAD) {
			WARN_ON_ONCE(1);
			runtime->restore_failed = true;
			runtime->last_fault = ret;
		}
		ret = deferred_fault;
		goto out;
	}
	if (runtime->restore_failed) {
		ret = runtime->last_fault ?: -EIO;
		goto out;
	}
	if (runtime->closing || !runtime->active) {
		ret = -ESHUTDOWN;
		goto out;
	}
	switch (event) {
	case PVSCHED_RECONCILE_RUN_ENTER:
	case PVSCHED_RECONCILE_VMEXIT:
	case PVSCHED_RECONCILE_CANCEL:
	case PVSCHED_RECONCILE_RUN_LEAVE:
		break;
	default:
		ret = -EINVAL;
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

	if (event == PVSCHED_RECONCILE_RUN_LEAVE) {
		/* GUEST may remain open on a terminal path lacking IRQ-on VMEXIT. */
		/* Do not leave a cutoff armed while the runner returns to QEMU. */
		hrtimer_try_to_cancel(&runtime->cutoff_timer);
		target = pvsched_baseline_prio(runtime);
	} else {
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
		} else if (input) {
			runtime->last_guest_area = *input;
			runtime->last_guest_area_valid = true;
		}
		if (!input) {
			ret = -EINVAL;
			goto internal_fault;
		}
		ret = pvsched_default_policy_select(&runtime->config,
					runtime->baseline.nice, input,
					old_cs_throttled,
					old_generic_throttled, &result);
		if (ret) {
			/* Malformed guest data is event-local, not a runner fault. */
			target = pvsched_baseline_prio(runtime);
			event_ret = ret;
		} else {
			target = result.prio;
		}
	}

selected:
	domain = pvsched_charge_class(runtime, &target);
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
		runtime_ns = task_sched_runtime(runtime->task);
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
	ret = event_ret;
	goto out;

apply_fault:
	ret = pvsched_fail_locked(runtime, ret);
	goto out;
internal_fault:
	/* An internal failure must not leave an owned elevated tuple applied. */
	ret = pvsched_fail_locked(runtime, ret);
out:
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
	return ret;
}

int pvsched_runner_guest_start(struct pvsched_runner_runtime *runtime)
{
	struct pvsched_runtime_sample sample;
	bool was_host;
	unsigned long flags;
	int ret = -ESHUTDOWN;

	raw_spin_lock_irqsave(&runtime->state_lock, flags);
	if (runtime->restore_owed_error && !runtime->closing &&
	    !runtime->exited) {
		/* Fast reentry cannot restore, but must re-arm the immediate kick. */
		hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(0),
			      HRTIMER_MODE_REL_PINNED_HARD);
		ret = runtime->restore_owed_error;
	} else if (runtime->active && !runtime->closing && !runtime->exited &&
		   !runtime->restore_failed) {
		was_host = runtime->accounting.phase == PVSCHED_RUNTIME_HOST;
		sample = pvsched_sample(runtime, false);
		ret = pvsched_runtime_guest_start(&runtime->accounting, sample);
		if (ret) {
			/*
			 * Late VMENTRY follows KVM's final reschedule check, so a setter
			 * cannot safely take effect before entry.  Defer restoration.
			 */
			runtime->active = false;
			runtime->last_fault = ret;
			runtime->restore_owed_error = ret;
			hrtimer_start(&runtime->cutoff_timer, ns_to_ktime(0),
				      HRTIMER_MODE_REL_PINNED_HARD);
		} else if (was_host) {
			/* Arm only for the HOST -> GUEST transition, not fast reentry. */
			pvsched_arm_cutoff_locked(runtime);
		}
	}
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
	return ret;
}

void pvsched_runner_guest_exit_irqoff(struct pvsched_runner_runtime *runtime)
{
	/*
	 * The pinned timer belongs to this CPU, so same-CPU IRQ-off VMEXIT may
	 * try-cancel it without state_lock.  IRQ-on VMEXIT performs settlement.
	 */
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
}

void pvsched_runner_publish_vmentry(struct pvsched_runner_runtime *runtime,
				    struct pvsched_host_area *host)
{
	union pvsched_applied_state state = { };
	unsigned long flags;

	raw_spin_lock_irqsave(&runtime->state_lock, flags);
	state.task = runtime->applied;
	if (runtime->accounting.accounting.cs.throttled)
		state.flags |= PVSCHED_APPLIED_CS_THROTTLED;
	if (runtime->accounting.accounting.generic.throttled)
		state.flags |= PVSCHED_APPLIED_TOTAL_THROTTLED;
	WRITE_ONCE(host->applied_state.raw, state.raw);
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
}
