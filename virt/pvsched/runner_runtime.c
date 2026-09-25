// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/ktime.h>
#include <linux/err.h>
#include <linux/sched/cputime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <kunit/visibility.h>

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
				   task_pid_nr((runtime)->task),	\
				   (runtime)->ops->name, ##__VA_ARGS__);	\
	else								\
		pr_warn_ratelimited("pvsched: runner %d policy %s: " fmt,	\
				    task_pid_nr((runtime)->task),	\
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

/*
 * Ask the policy for the parameters and class to apply next.  The class is
 * trusted, but it must be one the budgets allow now.
 */
static int pvsched_map_locked(struct pvsched_runner_runtime *runtime,
			      const struct pvsched_map_input *in,
			      enum pvsched_boost_class *class)
{
	u32 size = runtime->ops->params_size;
	u32 flags = 0;
	int ret;

	memset(runtime->out, 0, size);
	*class = PVSCHED_CLASS_BASELINE;
	ret = runtime->ops->map(&runtime->ctx, in, runtime->out, class, &flags);
	if (ret)
		return pvsched_policy_errno(runtime, ret);
	/* No output flags are defined yet. */
	if (flags)
		return -EINVAL;
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
			      task_sched_runtime(runtime->task) : 0,
		.runtime_valid = drain || force_runtime,
	};
}

/* pvsched_apply() refuses with -EOWNERDEAD once another owner took over. */
static int pvsched_restore_locked(struct pvsched_runner_runtime *runtime)
{
	return pvsched_apply(runtime, runtime->baseline, PVSCHED_CLASS_BASELINE);
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
		.task = runtime->task,
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
				u64 cs_budget_ns, u64 generic_budget_ns)
{
	int ret;

	if (!entry)
		return -EINVAL;
	memset(runtime, 0, sizeof(*runtime));
	raw_spin_lock_init(&runtime->state_lock);
	hrtimer_setup(&runtime->cutoff_timer, pvsched_cutoff_timer,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED_HARD);
	get_task_struct(task);
	runtime->task = task;
	runtime->generation = generation;
	ret = pvsched_runner_policy_bind(runtime, entry);
	if (ret)
		goto err_put;
	ret = pvsched_runtime_accounting_init(&runtime->accounting, cs_budget_ns,
					      generic_budget_ns, ktime_get_ns());
	if (ret)
		goto err_put;
	runtime->active = true;
	return 0;

err_put:
	put_task_struct(runtime->task);
	runtime->task = NULL;
	kfree(runtime->baseline);
	runtime->baseline = NULL;
	runtime->applied = NULL;
	runtime->out = NULL;
	if (runtime->entry) {
		pvsched_policy_entry_put(runtime->entry);
		runtime->entry = NULL;
	}
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_init);

void pvsched_runner_runtime_destroy(struct pvsched_runner_runtime *runtime)
{
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
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
	put_task_struct(runtime->task);
	runtime->task = NULL;
	kfree(runtime->baseline);
	runtime->baseline = NULL;
	runtime->applied = NULL;
	runtime->out = NULL;
	if (runtime->entry) {
		pvsched_policy_entry_put(runtime->entry);
		runtime->entry = NULL;
	}
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_runtime_destroy);

int pvsched_runner_reconcile(struct pvsched_runner_runtime *runtime,
			     enum pvsched_reconcile_event event,
			     const struct pvsched_default_guest_area *guest)
{
	enum pvsched_boost_class class = PVSCHED_CLASS_BASELINE;
	struct pvsched_runtime_sample sample;
	const struct pvsched_default_guest_area *input = guest;
	struct pvsched_map_input map_input;
	const void *target;
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
		if (pvsched_policy_bug_on(runtime, ret && ret != -EOWNERDEAD)) {
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
		target = runtime->baseline;
	} else {
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
		} else if (input) {
			runtime->last_guest_area = *input;
			runtime->last_guest_area_valid =
				pvsched_default_guest_valid(input);
			if (!runtime->last_guest_area_valid) {
				/* Malformed guest data is event-local, not a runner fault. */
				target = runtime->baseline;
				event_ret = -EINVAL;
				goto selected;
			}
		}
		if (!input) {
			ret = -EINVAL;
			goto internal_fault;
		}
		map_input = (struct pvsched_map_input) {
			.event = event,
			.guest = input,
			.cs_throttled = old_cs_throttled,
			.generic_throttled = old_generic_throttled,
		};
		ret = pvsched_map_locked(runtime, &map_input, &class);
		if (ret)
			goto apply_fault;
		target = runtime->out;
	}

selected:
	domain = pvsched_class_account(class);
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
	ret = pvsched_apply(runtime, target, class);
	if (ret)
		goto apply_fault;
	/* Commit is infallible after the prevalidation above and a valid setter. */
	WARN_ON_ONCE(pvsched_runtime_commit(&runtime->accounting, domain,
					    runtime_ptr));
	ret = event_ret;
	goto out;

apply_fault:
	ret = pvsched_fail_locked(runtime, ret);
	goto out;
internal_fault:
	/* An internal failure must not leave an owned elevated value applied. */
	ret = pvsched_fail_locked(runtime, ret);
out:
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_reconcile);

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
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_guest_start);

void pvsched_runner_guest_exit_irqoff(struct pvsched_runner_runtime *runtime)
{
	/*
	 * The pinned timer belongs to this CPU, so same-CPU IRQ-off VMEXIT may
	 * try-cancel it without state_lock.  IRQ-on VMEXIT performs settlement.
	 */
	hrtimer_try_to_cancel(&runtime->cutoff_timer);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_guest_exit_irqoff);

void pvsched_runner_publish_vmentry(struct pvsched_runner_runtime *runtime,
				    struct pvsched_host_area *host)
{
	union pvsched_applied_state state = { };
	unsigned long flags;

	raw_spin_lock_irqsave(&runtime->state_lock, flags);
	state.boost = runtime->applied_class;
	if (runtime->accounting.accounting.cs.throttled)
		state.flags |= PVSCHED_APPLIED_CS_THROTTLED;
	if (runtime->accounting.accounting.generic.throttled)
		state.flags |= PVSCHED_APPLIED_TOTAL_THROTTLED;
	WRITE_ONCE(host->applied_state.raw, state.raw);
	raw_spin_unlock_irqrestore(&runtime->state_lock, flags);
}
