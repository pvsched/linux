/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_ACCOUNTING_H
#define _VIRT_PVSCHED_ACCOUNTING_H

#include <linux/time64.h>
#include <linux/types.h>

/* Default budgets; the module parameters start from these. */
#define PVSCHED_DEFAULT_CS_BUDGET_NS	(1ULL * NSEC_PER_MSEC)
#define PVSCHED_DEFAULT_GENERIC_BUDGET_NS (500ULL * NSEC_PER_MSEC)

struct pvsched_budget {
	u64 debt_ns;
	u64 limit_ns;
	bool throttled;
};

struct pvsched_accounting {
	struct pvsched_budget cs;
	struct pvsched_budget generic;
};

/*
 * REFILL refills both budgets; DRAIN_CS_GENERIC drains both; DRAIN_GENERIC
 * drains the generic budget and refills the CS budget.
 */
enum pvsched_budget_account {
	PVSCHED_BUDGET_REFILL,
	PVSCHED_BUDGET_DRAIN_CS_GENERIC,
	PVSCHED_BUDGET_DRAIN_GENERIC,
};

/*
 * Per-budget arithmetic.  Recovery floors debt at zero, where the latch
 * clears; a charge saturates debt and latches at the limit.
 */
void pvsched_budget_recover(struct pvsched_budget *budget, u64 recovery_ns);
void pvsched_budget_charge(struct pvsched_budget *budget, u64 charge_ns);

/*
 * Initialize a new account whose nonzero limits remain immutable.  Invalid
 * limits leave the account unchanged.
 */
int pvsched_accounting_init(struct pvsched_accounting *accounting,
			    u64 cs_limit_ns, u64 generic_limit_ns);

/*
 * Apply one homogeneous interval of actual execution classification.  The
 * caller serializes updates and measures elapsed_ns; this core stores no time
 * and performs no runtime enforcement.  Invalid classifications leave the
 * account unchanged.
 */
int pvsched_accounting_update(struct pvsched_accounting *accounting,
			      u64 elapsed_ns,
			      enum pvsched_budget_account budget_account);

#endif /* _VIRT_PVSCHED_ACCOUNTING_H */
