// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/limits.h>

#include "accounting.h"

void pvsched_budget_recover(struct pvsched_budget *budget, u64 recovery_ns)
{
	if (recovery_ns >= budget->debt_ns) {
		budget->debt_ns = 0;
		budget->throttled = false;
	} else {
		budget->debt_ns -= recovery_ns;
	}
}

void pvsched_budget_charge(struct pvsched_budget *budget, u64 charge_ns)
{
	if (charge_ns > U64_MAX - budget->debt_ns)
		budget->debt_ns = U64_MAX;
	else
		budget->debt_ns += charge_ns;

	if (budget->debt_ns >= budget->limit_ns)
		budget->throttled = true;
}

static void pvsched_budget_update(struct pvsched_budget *budget,
				  u64 elapsed_ns, bool drain)
{
	if (!elapsed_ns)
		return;

	if (drain)
		pvsched_budget_charge(budget, elapsed_ns);
	else
		pvsched_budget_recover(budget, elapsed_ns);
}

int pvsched_accounting_init(struct pvsched_accounting *accounting,
			    u64 cs_limit_ns, u64 generic_limit_ns)
{
	if (!cs_limit_ns || !generic_limit_ns)
		return -EINVAL;

	*accounting = (struct pvsched_accounting) {
		.cs.limit_ns = cs_limit_ns,
		.generic.limit_ns = generic_limit_ns,
	};

	return 0;
}

int pvsched_accounting_update(struct pvsched_accounting *accounting,
			      u64 elapsed_ns,
			      enum pvsched_budget_account budget_account)
{
	switch (budget_account) {
	case PVSCHED_BUDGET_REFILL:
		pvsched_budget_update(&accounting->cs, elapsed_ns, false);
		pvsched_budget_update(&accounting->generic, elapsed_ns, false);
		break;
	case PVSCHED_BUDGET_DRAIN_CS_GENERIC:
		pvsched_budget_update(&accounting->cs, elapsed_ns, true);
		pvsched_budget_update(&accounting->generic, elapsed_ns, true);
		break;
	case PVSCHED_BUDGET_DRAIN_GENERIC:
		pvsched_budget_update(&accounting->cs, elapsed_ns, false);
		pvsched_budget_update(&accounting->generic, elapsed_ns, true);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
