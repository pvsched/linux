// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/minmax.h>
#include <kunit/visibility.h>

#include "runtime_accounting.h"

static bool pvsched_runtime_domain_valid(enum pvsched_budget_account domain)
{
	return domain >= PVSCHED_BUDGET_REFILL &&
	       domain <= PVSCHED_BUDGET_DRAIN_GENERIC;
}

static bool pvsched_runtime_sample_valid(
		const struct pvsched_runtime_accounting *runtime,
		struct pvsched_runtime_sample sample)
{
	return runtime->domain == PVSCHED_BUDGET_REFILL ||
	       sample.runtime_valid;
}

static bool
pvsched_runtime_anchor_state_valid(const struct pvsched_runtime_accounting *runtime)
{
	return runtime->domain == PVSCHED_BUDGET_REFILL ||
	       runtime->anchor.runtime_valid;
}

static void
pvsched_runtime_advance_anchor(struct pvsched_runtime_accounting *runtime,
			       struct pvsched_runtime_sample sample)
{
	WARN_ON_ONCE(!sample.runtime_valid);
	/* Each nanosecond is settled once; regressions never move anchors back. */
	runtime->anchor.wall_ns = max(runtime->anchor.wall_ns, sample.wall_ns);
	runtime->anchor.runtime_ns = max(runtime->anchor.runtime_ns,
					 sample.runtime_ns);
	runtime->anchor.runtime_valid = true;
}

static void
pvsched_runtime_advance_wall_anchor(struct pvsched_runtime_accounting *runtime,
				    struct pvsched_runtime_sample sample)
{
	/* REFILL ignores task runtime but retains the same monotonic wall rule. */
	runtime->anchor.wall_ns = max(runtime->anchor.wall_ns, sample.wall_ns);
}

static bool pvsched_budget_would_cutoff(const struct pvsched_budget *budget,
					 u64 charge_ns)
{
	/* Test debt first so limit - debt cannot underflow. */
	return budget->debt_ns >= budget->limit_ns ||
	       charge_ns >= budget->limit_ns - budget->debt_ns;
}

static void pvsched_runtime_budget_settle(struct pvsched_budget *budget,
					  u64 recovery_ns, u64 charge_ns)
{
	/*
	 * Only the totals for this preemptible HOST interval are known, not
	 * whether boosted CPU time preceded eligible off-CPU recovery.  Debt
	 * floors at zero and the latch clears only at zero, so order matters.
	 * Use the order worst for the host: latch if the full charge could have
	 * crossed the limit from the starting debt, then recover before
	 * charging.  max(debt - recovery, 0) + charge is never less than
	 * max(debt + charge - recovery, 0), while retaining a possible cutoff
	 * until the conservative debt reaches zero.  An existing latch is
	 * expected when a downgrade setter failed and the runner stayed boosted.
	 */
	bool possible_cutoff = pvsched_budget_would_cutoff(budget, charge_ns);
	bool keep_throttled = budget->throttled || possible_cutoff;

	pvsched_budget_recover(budget, recovery_ns);
	pvsched_budget_charge(budget, charge_ns);
	if (keep_throttled && budget->debt_ns)
		budget->throttled = true;
}

/*
 * A preemptible interval does not reveal whether CPU execution preceded or
 * followed its off-CPU time.  Latch every cutoff made possible by the full
 * CPU charge before applying recovery, then recover first and charge CPU.
 */
static void pvsched_runtime_settle_host(struct pvsched_runtime_accounting *runtime,
					 struct pvsched_runtime_sample sample)
{
	u64 wall_ns = sample.wall_ns > runtime->anchor.wall_ns ?
		      sample.wall_ns - runtime->anchor.wall_ns : 0;
	u64 cpu_ns = sample.runtime_ns > runtime->anchor.runtime_ns ?
		     sample.runtime_ns - runtime->anchor.runtime_ns : 0;
	u64 off_cpu_ns = wall_ns > cpu_ns ? wall_ns - cpu_ns : 0;

	switch (runtime->domain) {
	case PVSCHED_BUDGET_REFILL:
		/* runtime_ns is deliberately ignored throughout REFILL windows. */
		pvsched_budget_recover(&runtime->accounting.cs, wall_ns);
		pvsched_budget_recover(&runtime->accounting.generic, wall_ns);
		pvsched_runtime_advance_wall_anchor(runtime, sample);
		break;
	case PVSCHED_BUDGET_DRAIN_CS_GENERIC:
		pvsched_runtime_budget_settle(&runtime->accounting.cs,
					      off_cpu_ns, cpu_ns);
		pvsched_runtime_budget_settle(&runtime->accounting.generic,
					      off_cpu_ns, cpu_ns);
		pvsched_runtime_advance_anchor(runtime, sample);
		break;
	case PVSCHED_BUDGET_DRAIN_GENERIC:
		/*
		 * CS isn't charged during a task-only boost, so it recovers the
		 * full wall interval.
		 */
		pvsched_budget_recover(&runtime->accounting.cs, wall_ns);
		pvsched_runtime_budget_settle(&runtime->accounting.generic,
					      off_cpu_ns, cpu_ns);
		pvsched_runtime_advance_anchor(runtime, sample);
		break;
	}
}

int pvsched_runtime_accounting_init(struct pvsched_runtime_accounting *runtime,
				    u64 cs_limit_ns, u64 generic_limit_ns,
				    u64 now_wall_ns)
{
	struct pvsched_runtime_accounting next = {
		.anchor.wall_ns = now_wall_ns,
		.domain = PVSCHED_BUDGET_REFILL,
		.phase = PVSCHED_RUNTIME_HOST,
	};
	int ret;

	ret = pvsched_accounting_init(&next.accounting, cs_limit_ns,
				       generic_limit_ns);
	if (ret)
		return ret;

	*runtime = next;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runtime_accounting_init);

int pvsched_runtime_settle(struct pvsched_runtime_accounting *runtime,
			   struct pvsched_runtime_sample sample)
{
	if (runtime->phase != PVSCHED_RUNTIME_HOST ||
	    !pvsched_runtime_domain_valid(runtime->domain) ||
	    !pvsched_runtime_anchor_state_valid(runtime) ||
	    !pvsched_runtime_sample_valid(runtime, sample))
		return -EINVAL;

	pvsched_runtime_settle_host(runtime, sample);

	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runtime_settle);

int pvsched_runtime_commit(struct pvsched_runtime_accounting *runtime,
			   enum pvsched_budget_account domain,
			   const u64 *runtime_ns)
{
	/* Only init and a validated commit write the current domain. */
	if (runtime->phase != PVSCHED_RUNTIME_HOST ||
	    !pvsched_runtime_domain_valid(domain) ||
	    !pvsched_runtime_anchor_state_valid(runtime) ||
	    (runtime->domain == PVSCHED_BUDGET_REFILL &&
	     domain != PVSCHED_BUDGET_REFILL && !runtime_ns))
		return -EINVAL;

	if (runtime->domain == PVSCHED_BUDGET_REFILL &&
	    domain != PVSCHED_BUDGET_REFILL) {
		/* Only REFILL -> drain needs runtime; REFILL never maintained it. */
		runtime->anchor.runtime_ns = *runtime_ns;
		runtime->anchor.runtime_valid = true;
	}
	runtime->domain = domain;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runtime_commit);

int pvsched_runtime_guest_start(struct pvsched_runtime_accounting *runtime,
				struct pvsched_runtime_sample sample)
{
	if (!pvsched_runtime_domain_valid(runtime->domain) ||
	    !pvsched_runtime_anchor_state_valid(runtime))
		return -EINVAL;
	if (runtime->phase == PVSCHED_RUNTIME_GUEST)
		return 0;
	if (runtime->phase != PVSCHED_RUNTIME_HOST)
		return -EINVAL;
	if (!pvsched_runtime_sample_valid(runtime, sample))
		return -EINVAL;

	pvsched_runtime_settle_host(runtime, sample);
	runtime->guest_start_ns = runtime->anchor.wall_ns;
	runtime->phase = PVSCHED_RUNTIME_GUEST;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runtime_guest_start);

int pvsched_runtime_guest_close(struct pvsched_runtime_accounting *runtime,
				struct pvsched_runtime_sample sample)
{
	u64 elapsed_ns;

	if (!pvsched_runtime_domain_valid(runtime->domain) ||
	    !pvsched_runtime_anchor_state_valid(runtime) ||
	    !pvsched_runtime_sample_valid(runtime, sample))
		return -EINVAL;

	if (runtime->phase == PVSCHED_RUNTIME_GUEST) {
		/* Match guest_start(): charge this GUEST window exactly once. */
		elapsed_ns = sample.wall_ns > runtime->guest_start_ns ?
			     sample.wall_ns - runtime->guest_start_ns : 0;
		pvsched_accounting_update(&runtime->accounting, elapsed_ns,
					  runtime->domain);
		if (runtime->domain == PVSCHED_BUDGET_REFILL)
			pvsched_runtime_advance_wall_anchor(runtime, sample);
		else
			pvsched_runtime_advance_anchor(runtime, sample);
	} else if (runtime->phase == PVSCHED_RUNTIME_HOST) {
		/* Mid-run ATTACH may observe a close without an earlier start. */
		pvsched_runtime_settle_host(runtime, sample);
	} else {
		return -EINVAL;
	}

	runtime->phase = PVSCHED_RUNTIME_HOST;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runtime_guest_close);
