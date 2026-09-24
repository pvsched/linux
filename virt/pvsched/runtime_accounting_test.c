// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include "runtime_accounting.h"

#define SAMPLE(_wall, _runtime) \
	((struct pvsched_runtime_sample){ .wall_ns = (_wall), \
					    .runtime_ns = (_runtime), \
					    .runtime_valid = true })

#define SAMPLE_MISSING(_wall) \
	((struct pvsched_runtime_sample){ .wall_ns = (_wall) })

/* Exercise an ordinary pre-selection settle followed by a successful apply. */
static int test_checkpoint(struct pvsched_runtime_accounting *runtime,
			   struct pvsched_runtime_sample sample,
			   enum pvsched_budget_account domain)
{
	int ret;

	ret = pvsched_runtime_settle(runtime, sample);
	if (ret)
		return ret;
	/* Commit reads the runtime only for a REFILL -> drain transition. */
	return pvsched_runtime_commit(runtime, domain, sample.runtime_valid ?
				      &sample.runtime_ns : NULL);
}

static void pvsched_runtime_expect(struct kunit *test,
				   struct pvsched_runtime_accounting *runtime,
				   u64 cs_debt, u64 generic_debt,
				   bool cs_throttled, bool generic_throttled)
{
	KUNIT_EXPECT_EQ(test, runtime->accounting.cs.debt_ns, cs_debt);
	KUNIT_EXPECT_EQ(test, runtime->accounting.generic.debt_ns, generic_debt);
	KUNIT_EXPECT_EQ(test, runtime->accounting.cs.throttled, cs_throttled);
	KUNIT_EXPECT_EQ(test, runtime->accounting.generic.throttled, generic_throttled);
}

static void pvsched_runtime_transition_order_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 10, 20, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(10, 4),
			PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 4, 4, false, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(15, 6),
			PVSCHED_BUDGET_REFILL), 0);
	pvsched_runtime_expect(test, &runtime, 0, 3, false, false);
}

static void pvsched_runtime_conservative_cutoff_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	struct pvsched_accounting cpu_first, offcpu_first;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 10, 20, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(8, 8),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&cpu_first, 10, 20), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&cpu_first, 8,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	offcpu_first = cpu_first;
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&cpu_first, 5,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&cpu_first, 7,
			PVSCHED_BUDGET_REFILL), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&offcpu_first, 7,
			PVSCHED_BUDGET_REFILL), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&offcpu_first, 5,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_EXPECT_EQ(test, cpu_first.cs.debt_ns, 6ULL);
	KUNIT_EXPECT_EQ(test, offcpu_first.cs.debt_ns, 6ULL);
	KUNIT_EXPECT_TRUE(test, cpu_first.cs.throttled);
	KUNIT_EXPECT_FALSE(test, offcpu_first.cs.throttled);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(20, 13),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 6, 6, true, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(32, 18),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 5, 5, true, false);
}

static void pvsched_runtime_latch_survives_mixed_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 10, 20, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(10, 10),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(15, 10),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 5, 5, true, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(21, 11),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 1, 1, true, false);
}

static void pvsched_runtime_guest_close_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 100, 200, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(5, 2)), 0);
	pvsched_runtime_expect(test, &runtime, 2, 2, false, false);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, SAMPLE(15, 14)), 0);
	pvsched_runtime_expect(test, &runtime, 12, 12, false, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(23, 16),
			PVSCHED_BUDGET_REFILL), 0);
	pvsched_runtime_expect(test, &runtime, 8, 8, false, false);
}

static void pvsched_runtime_fast_reentry_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	struct pvsched_runtime_accounting before;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 100, 200, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(1, 1)), 0);
	before = runtime;
	KUNIT_EXPECT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(2, 2)), 0);
	KUNIT_EXPECT_MEMEQ(test, &runtime, &before, sizeof(runtime));
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, SAMPLE(6, 6)), 0);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
}

static void pvsched_runtime_initial_guest_close_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 10, 20, 5), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 5ULL);
	KUNIT_EXPECT_FALSE(test, runtime.anchor.runtime_valid);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, SAMPLE(10, 0)), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_FALSE(test, runtime.anchor.runtime_valid);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(15, 3),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(20, 5),
			PVSCHED_BUDGET_REFILL), 0);
	pvsched_runtime_expect(test, &runtime, 2, 2, false, false);
}

static void pvsched_runtime_refill_zero_runtime_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime,
			SAMPLE_MISSING(0)), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE_MISSING(10),
			PVSCHED_BUDGET_REFILL), 0);
	/* The commit into DRAIN starts a fresh accurate runtime anchor. */
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(20, 7),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(30, 14),
			PVSCHED_BUDGET_REFILL), 0);
	pvsched_runtime_expect(test, &runtime, 7, 7, false, false);
}

static void pvsched_runtime_valid_zero_sample_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(1, 0)), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, SAMPLE(2, 0)), 0);
	pvsched_runtime_expect(test, &runtime, 0, 1, false, false);
}

static void pvsched_runtime_invalid_refill_runtime_ignored_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	struct pvsched_runtime_sample missing = {
		.runtime_ns = 1000,
	};

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, missing), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(10, 10),
			PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(15, 15)), 0);
	pvsched_runtime_expect(test, &runtime, 0, 5, false, false);
}

static void pvsched_runtime_commit_anchor_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	u64 initial_runtime = 4;
	u64 later_runtime = 9;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(10)), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_GENERIC, &initial_runtime), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 4ULL);
	KUNIT_EXPECT_TRUE(test, runtime.anchor.runtime_valid);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC, &later_runtime), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 4ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_REFILL, NULL), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 4ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(12)), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 12ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_GENERIC, &later_runtime), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 12ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 9ULL);
}

static void pvsched_runtime_commit_clamped_wall_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	u64 runtime_ns = 3;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(10)), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(9)), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_GENERIC, &runtime_ns), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 3ULL);
}

static void pvsched_runtime_mixed_anchor_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	u64 runtime_ns = 4;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(10)), 0);
	/* Local select ran for 2 CPU ns after the wall settlement. */
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_GENERIC, &runtime_ns), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(15, 7)), 0);
	/* Wall delta 5, CPU delta 3: the 2 ns pre-commit CPU is credited off-CPU. */
	pvsched_runtime_expect(test, &runtime, 0, 3, false, false);
}

static void pvsched_runtime_missing_sample_rejected_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;
	struct pvsched_runtime_accounting before;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(0)), 0);
	before = runtime;
	KUNIT_EXPECT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_GENERIC, NULL), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &runtime, &before, sizeof(runtime));
	{
		u64 zero = 0;

		KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
				PVSCHED_BUDGET_DRAIN_GENERIC, &zero), 0);
	}
	before = runtime;
	KUNIT_EXPECT_EQ(test, test_checkpoint(&runtime,
			SAMPLE_MISSING(1), PVSCHED_BUDGET_REFILL), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &runtime, &before, sizeof(runtime));
	KUNIT_EXPECT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(1)),
			-EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &runtime, &before, sizeof(runtime));
	KUNIT_EXPECT_EQ(test, pvsched_runtime_guest_start(&runtime,
			SAMPLE_MISSING(1)), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &runtime, &before, sizeof(runtime));
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(1, 1)), 0);
	before = runtime;
	KUNIT_EXPECT_EQ(test, pvsched_runtime_guest_close(&runtime,
			SAMPLE_MISSING(2)), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, &runtime, &before, sizeof(runtime));
}

static void pvsched_runtime_settle_before_commit_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 5, 10, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(10, 10)), 0);
	KUNIT_EXPECT_TRUE(test, runtime.accounting.generic.throttled);
	KUNIT_EXPECT_EQ(test, runtime.domain, PVSCHED_BUDGET_DRAIN_GENERIC);
	/* A failed setter leaves the old domain; settle it again at the next hook. */
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(11, 11)), 0);
	KUNIT_EXPECT_EQ(test, runtime.domain, PVSCHED_BUDGET_DRAIN_GENERIC);
	/* A successful setter commits the downgrade without another sample. */
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(12, 12),
			PVSCHED_BUDGET_REFILL), 0);
	KUNIT_EXPECT_EQ(test, runtime.domain, PVSCHED_BUDGET_REFILL);
	/* A same-tuple checkpoint still settles, without a setter. */
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(13, 13)), 0);
	KUNIT_EXPECT_EQ(test, runtime.domain, PVSCHED_BUDGET_REFILL);
}

static void pvsched_runtime_commit_gap_cutoff_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 10, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(9, 9)), 0);
	KUNIT_EXPECT_FALSE(test, runtime.accounting.generic.throttled);
	/* The setter interval is not settled at commit. */
	KUNIT_ASSERT_EQ(test, pvsched_runtime_commit(&runtime,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC, NULL), 0);
	pvsched_runtime_expect(test, &runtime, 0, 9, false, false);
	KUNIT_EXPECT_EQ(test, runtime.domain, PVSCHED_BUDGET_DRAIN_CS_GENERIC);
	/* The next VMENTRY settles the gap and exposes the cutoff to its timer. */
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(11, 11)), 0);
	pvsched_runtime_expect(test, &runtime, 2, 11, false, true);
}

static void pvsched_runtime_cs_latch_generic_drain_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 5, 20, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE(5, 5)), 0);
	KUNIT_EXPECT_TRUE(test, runtime.accounting.cs.throttled);
	KUNIT_EXPECT_FALSE(test, runtime.accounting.generic.throttled);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(6, 6),
			PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_EXPECT_EQ(test, runtime.domain, PVSCHED_BUDGET_DRAIN_GENERIC);
}

static void pvsched_runtime_wall_regression_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(10, 10),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(9, 9),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 10ULL);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(15, 15),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 5, 5, false, false);
}

static void pvsched_runtime_runtime_regression_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(10, 10),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(15, 9),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 15ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 10ULL);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(20, 15),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 5, 5, false, false);
}

static void pvsched_runtime_guest_close_regression_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 20, 40, 0), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(10, 10),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime, SAMPLE(15, 15)), 0);
	pvsched_runtime_expect(test, &runtime, 5, 5, false, false);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, SAMPLE(14, 14)), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 15ULL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.runtime_ns, 15ULL);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(20, 20),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_runtime_expect(test, &runtime, 10, 10, false, false);
}

static void pvsched_runtime_invalid_domain_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 10, 20, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runtime_commit(&runtime,
			(enum pvsched_budget_account)99, NULL), -EINVAL);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 0ULL);
}

static void pvsched_runtime_first_wall_regression_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime, 10, 20, 10), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_settle(&runtime, SAMPLE_MISSING(9)), 0);
	KUNIT_EXPECT_EQ(test, runtime.anchor.wall_ns, 10ULL);
	KUNIT_EXPECT_FALSE(test, runtime.anchor.runtime_valid);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
}

static void pvsched_runtime_golden_vector_test(struct kunit *test)
{
	struct pvsched_runtime_accounting runtime;

	KUNIT_ASSERT_EQ(test, pvsched_runtime_accounting_init(&runtime,
			2 * NSEC_PER_MSEC, 500 * NSEC_PER_MSEC, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime, SAMPLE(0, 0)), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime, SAMPLE(0, 0),
			PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	/* Four ms wall time: three running and one preempted. */
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_start(&runtime,
			SAMPLE(4 * NSEC_PER_MSEC, 3 * NSEC_PER_MSEC)), 0);
	pvsched_runtime_expect(test, &runtime, 3 * NSEC_PER_MSEC,
			3 * NSEC_PER_MSEC, true, false);
	KUNIT_ASSERT_EQ(test, pvsched_runtime_guest_close(&runtime,
			SAMPLE(4 * NSEC_PER_MSEC, 3 * NSEC_PER_MSEC)), 0);
	KUNIT_ASSERT_EQ(test, test_checkpoint(&runtime,
			SAMPLE(7 * NSEC_PER_MSEC, 3 * NSEC_PER_MSEC),
			PVSCHED_BUDGET_REFILL), 0);
	pvsched_runtime_expect(test, &runtime, 0, 0, false, false);
}

static struct kunit_case pvsched_runtime_accounting_test_cases[] = {
	KUNIT_CASE(pvsched_runtime_transition_order_test),
	KUNIT_CASE(pvsched_runtime_conservative_cutoff_test),
	KUNIT_CASE(pvsched_runtime_latch_survives_mixed_test),
	KUNIT_CASE(pvsched_runtime_guest_close_test),
	KUNIT_CASE(pvsched_runtime_fast_reentry_test),
	KUNIT_CASE(pvsched_runtime_initial_guest_close_test),
	KUNIT_CASE(pvsched_runtime_refill_zero_runtime_test),
	KUNIT_CASE(pvsched_runtime_valid_zero_sample_test),
	KUNIT_CASE(pvsched_runtime_invalid_refill_runtime_ignored_test),
	KUNIT_CASE(pvsched_runtime_commit_anchor_test),
	KUNIT_CASE(pvsched_runtime_commit_clamped_wall_test),
	KUNIT_CASE(pvsched_runtime_mixed_anchor_test),
	KUNIT_CASE(pvsched_runtime_missing_sample_rejected_test),
	KUNIT_CASE(pvsched_runtime_settle_before_commit_test),
	KUNIT_CASE(pvsched_runtime_commit_gap_cutoff_test),
	KUNIT_CASE(pvsched_runtime_cs_latch_generic_drain_test),
	KUNIT_CASE(pvsched_runtime_wall_regression_test),
	KUNIT_CASE(pvsched_runtime_runtime_regression_test),
	KUNIT_CASE(pvsched_runtime_guest_close_regression_test),
	KUNIT_CASE(pvsched_runtime_invalid_domain_test),
	KUNIT_CASE(pvsched_runtime_first_wall_regression_test),
	KUNIT_CASE(pvsched_runtime_golden_vector_test),
	{}
};

static struct kunit_suite pvsched_runtime_accounting_test_suite = {
	.name = "pvsched-runtime-accounting",
	.test_cases = pvsched_runtime_accounting_test_cases,
};

kunit_test_suite(pvsched_runtime_accounting_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
