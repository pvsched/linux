/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_RUNTIME_ACCOUNTING_H
#define _VIRT_PVSCHED_RUNTIME_ACCOUNTING_H

#include "accounting.h"

struct pvsched_runtime_sample {
	/* CLOCK_MONOTONIC, sampled with ktime_get_ns() on any CPU. */
	u64 wall_ns;
	/* Accurate task runtime at drain boundaries; ignored during REFILL. */
	u64 runtime_ns;
	/* Distinguishes a valid zero runtime sample from an omitted sample. */
	bool runtime_valid;
};

enum pvsched_runtime_phase {
	/* Host-side interval between KVM checkpoints. */
	PVSCHED_RUNTIME_HOST,
	/* Wall-charged interval from late VMENTRY through IRQ-on VMEXIT. */
	PVSCHED_RUNTIME_GUEST,
};

struct pvsched_runtime_accounting {
	/* Persistent debt and cutoff latches, retained until DETACH. */
	struct pvsched_accounting accounting;
	/*
	 * Start of the open HOST interval.  Runtime is valid only in drain
	 * domains and is deliberately ignored during REFILL.  Consequently,
	 * anchor.runtime_valid is meaningful only in drain domains.
	 */
	struct pvsched_runtime_sample anchor;
	/* Classification applied to the interval ending at the next boundary. */
	enum pvsched_budget_account domain;
	/* HOST permits transitions; GUEST blocks remote scheduling application. */
	enum pvsched_runtime_phase phase;
	/* GUEST wall-window start; valid only while phase is GUEST. */
	u64 guest_start_ns;
};

/*
 * Runtime accounting is a pure settlement helper.  Its caller supplies
 * coherent samples and serializes every operation (eventually under the
 * runner state lock); the helper reads neither clocks nor scheduler state.
 * Small clock regressions are clamped to zero elapsed time; neither anchor
 * coordinate moves backwards.
 */
int pvsched_runtime_accounting_init(struct pvsched_runtime_accounting *runtime,
				    u64 cs_limit_ns, u64 generic_limit_ns,
				    u64 now_wall_ns);

/*
 * Settle the open HOST interval without changing its charge domain.  At a
 * setter-capable checkpoint the required order is:
 *
 * settle -> select from the updated latches -> setter -> commit
 *
 * The caller supplies task runtime iff the current domain drains.  A failed
 * setter must not be followed by commit.
 */
int pvsched_runtime_settle(struct pvsched_runtime_accounting *runtime,
			   struct pvsched_runtime_sample sample);

/*
 * Commit the domain of the applied tuple without resampling or settlement.
 * Only REFILL -> drain needs a fresh task-runtime value; pass NULL otherwise.
 * The wall anchor remains at the preceding settlement boundary.  For a local
 * boost, CPU time between that boundary and the runtime read can appear as
 * off-CPU recovery at the next settlement (a small conservative undercharge).
 */
int pvsched_runtime_commit(struct pvsched_runtime_accounting *runtime,
			   enum pvsched_budget_account domain,
			   const u64 *runtime_ns);

/* Settle HOST accounting and begin one wall-charged guest window. */
int pvsched_runtime_guest_start(struct pvsched_runtime_accounting *runtime,
				struct pvsched_runtime_sample sample);

/*
 * At IRQ-on VMEXIT, close an open GUEST window or settle an existing HOST
 * interval, then establish the accurate anchor for the next HOST interval.
 * A close before the first observed start settles the initial REFILL window.
 */
int pvsched_runtime_guest_close(struct pvsched_runtime_accounting *runtime,
				struct pvsched_runtime_sample sample);

#endif /* _VIRT_PVSCHED_RUNTIME_ACCOUNTING_H */
