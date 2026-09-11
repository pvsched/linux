// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/limits.h>

#include "accounting.h"

static void pvsched_expect_budget(struct kunit *test,
				  const struct pvsched_budget *budget,
					u64 debt, u64 limit, bool throttled)
{
	KUNIT_EXPECT_EQ(test, budget->debt_ns, debt);
	KUNIT_EXPECT_EQ(test, budget->limit_ns, limit);
	KUNIT_EXPECT_EQ(test, budget->throttled, throttled);
}

static void pvsched_expect_accounting(struct kunit *test,
				      const struct pvsched_accounting *accounting,
				      const struct pvsched_accounting *expected)
{
	pvsched_expect_budget(test, &accounting->cs, expected->cs.debt_ns,
			      expected->cs.limit_ns, expected->cs.throttled);
	pvsched_expect_budget(test, &accounting->generic,
			      expected->generic.debt_ns,
			      expected->generic.limit_ns,
			      expected->generic.throttled);
}

static void pvsched_accounting_init_test(struct kunit *test)
{
	struct pvsched_accounting accounting;
	u64 cs_recovery_ns = 2 * 1000 * 1000 - 1;
	u64 generic_interval_ns = 500 * 1000 * 1000;

	KUNIT_EXPECT_EQ(test, PVSCHED_DEFAULT_CS_BUDGET_NS, 2000000ULL);
	KUNIT_EXPECT_EQ(test, PVSCHED_DEFAULT_GENERIC_BUDGET_NS, 500000000ULL);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting,
						      PVSCHED_DEFAULT_CS_BUDGET_NS,
						      PVSCHED_DEFAULT_GENERIC_BUDGET_NS), 0);
	pvsched_expect_budget(test, &accounting.cs, 0, 2000000ULL, false);
	pvsched_expect_budget(test, &accounting.generic, 0, 500000000ULL, false);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 2 * 1000 * 1000,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_EXPECT_TRUE(test, accounting.cs.throttled);
	KUNIT_EXPECT_FALSE(test, accounting.generic.throttled);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 1,
							PVSCHED_BUDGET_REFILL), 0);
	KUNIT_EXPECT_TRUE(test, accounting.cs.throttled);
	KUNIT_EXPECT_EQ(test, accounting.cs.debt_ns, (u64)(2 * 1000 * 1000 - 1));
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, cs_recovery_ns,
							PVSCHED_BUDGET_REFILL), 0);
	KUNIT_EXPECT_FALSE(test, accounting.cs.throttled);
	KUNIT_EXPECT_EQ(test, accounting.cs.debt_ns, (u64)0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, generic_interval_ns,
							PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_EXPECT_TRUE(test, accounting.generic.throttled);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, generic_interval_ns,
							PVSCHED_BUDGET_REFILL), 0);
	KUNIT_EXPECT_FALSE(test, accounting.generic.throttled);
}

static void pvsched_accounting_invalid_init_test(struct kunit *test)
{
	struct pvsched_accounting accounting = {
		.cs = { .debt_ns = 7, .limit_ns = 11, .throttled = true },
		.generic = { .debt_ns = 13, .limit_ns = 17, .throttled = false },
	};
	struct pvsched_accounting before = accounting;

	KUNIT_EXPECT_EQ(test, pvsched_accounting_init(&accounting, 0, 10), -EINVAL);
	pvsched_expect_accounting(test, &accounting, &before);
	KUNIT_EXPECT_EQ(test, pvsched_accounting_init(&accounting, 10, 0), -EINVAL);
	pvsched_expect_accounting(test, &accounting, &before);
}

static void pvsched_accounting_charge_classes_test(struct kunit *test)
{
	struct pvsched_accounting accounting;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, 100, 200), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 30,
							PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	pvsched_expect_budget(test, &accounting.cs, 0, 100, false);
	pvsched_expect_budget(test, &accounting.generic, 30, 200, false);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 40,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_expect_budget(test, &accounting.cs, 40, 100, false);
	pvsched_expect_budget(test, &accounting.generic, 70, 200, false);
}

static void pvsched_accounting_recovery_test(struct kunit *test)
{
	struct pvsched_accounting accounting;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, 100, 200), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 80,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 30,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &accounting.cs, 50, 100, false);
	pvsched_expect_budget(test, &accounting.generic, 50, 200, false);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 60,
							PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	pvsched_expect_budget(test, &accounting.cs, 0, 100, false);
	pvsched_expect_budget(test, &accounting.generic, 110, 200, false);
}

static void pvsched_accounting_cutoff_test(struct kunit *test)
{
	struct pvsched_accounting accounting;
	struct pvsched_accounting boundary;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, 10, 20), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&boundary, 10, 20), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&boundary, 9,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_EXPECT_FALSE(test, boundary.cs.throttled);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&boundary, 1,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_EXPECT_TRUE(test, boundary.cs.throttled);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 20,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_expect_budget(test, &accounting.cs, 20, 10, true);
	pvsched_expect_budget(test, &accounting.generic, 20, 20, true);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 15,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &accounting.cs, 5, 10, true);
	pvsched_expect_budget(test, &accounting.generic, 5, 20, true);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 1,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_expect_budget(test, &accounting.cs, 6, 10, true);
	pvsched_expect_budget(test, &accounting.generic, 6, 20, true);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 5,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &accounting.cs, 1, 10, true);
	pvsched_expect_budget(test, &accounting.generic, 1, 20, true);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 1,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &accounting.cs, 0, 10, false);
	pvsched_expect_budget(test, &accounting.generic, 0, 20, false);
}

static void pvsched_accounting_zero_identity_test(struct kunit *test)
{
	struct pvsched_accounting accounting;
	struct pvsched_accounting before;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, 10, 20), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 10,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	before = accounting;
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 0,
							PVSCHED_BUDGET_REFILL), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 0,
							PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 0,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_expect_accounting(test, &accounting, &before);
}

static void pvsched_accounting_split_equivalence_test(struct kunit *test)
{
	static const enum pvsched_budget_account domains[] = {
		PVSCHED_BUDGET_DRAIN_CS_GENERIC,
		PVSCHED_BUDGET_REFILL,
		PVSCHED_BUDGET_DRAIN_GENERIC,
	};
	struct pvsched_accounting one, split;
	unsigned int i;

	/* One interval and the same interval split in two settle alike. */
	for (i = 0; i < ARRAY_SIZE(domains); i++) {
		KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&one, 100, 200), 0);
		/* Give a refill a throttled debt to recover. */
		if (domains[i] == PVSCHED_BUDGET_REFILL)
			KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&one, 100,
					PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
		split = one;
		KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&one, 100,
								domains[i]), 0);
		KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&split, 40,
								domains[i]), 0);
		KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&split, 60,
								domains[i]), 0);
		pvsched_expect_accounting(test, &split, &one);
	}
}

static void pvsched_accounting_ordered_intervals_test(struct kunit *test)
{
	struct pvsched_accounting first, second;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&first, 10, 20), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&second, 10, 20), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&first, 10,
							PVSCHED_BUDGET_REFILL), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&first, 10,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&second, 10,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&second, 10,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &first.cs, 10, 10, true);
	pvsched_expect_budget(test, &second.cs, 0, 10, false);
}

static void pvsched_accounting_invalid_charge_test(struct kunit *test)
{
	struct pvsched_accounting accounting;
	struct pvsched_accounting before;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, 100, 200), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 30,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	before = accounting;
	KUNIT_EXPECT_EQ(test, pvsched_accounting_update(&accounting, 50,
							(enum pvsched_budget_account)99), -EINVAL);
	pvsched_expect_accounting(test, &accounting, &before);
	KUNIT_EXPECT_EQ(test, pvsched_accounting_update(&accounting, 0,
							(enum pvsched_budget_account)-1), -EINVAL);
	pvsched_expect_accounting(test, &accounting, &before);
}

static void pvsched_accounting_saturation_test(struct kunit *test)
{
	struct pvsched_accounting accounting;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, U64_MAX,
						      U64_MAX), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, U64_MAX - 4,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 10,
							PVSCHED_BUDGET_DRAIN_CS_GENERIC), 0);
	pvsched_expect_budget(test, &accounting.cs, U64_MAX, U64_MAX, true);
	pvsched_expect_budget(test, &accounting.generic, U64_MAX, U64_MAX, true);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, U64_MAX - 1,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &accounting.cs, 1, U64_MAX, true);
	pvsched_expect_budget(test, &accounting.generic, 1, U64_MAX, true);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 1,
							PVSCHED_BUDGET_REFILL), 0);
	pvsched_expect_budget(test, &accounting.cs, 0, U64_MAX, false);
	pvsched_expect_budget(test, &accounting.generic, 0, U64_MAX, false);
}

static void pvsched_accounting_reverse_limits_test(struct kunit *test)
{
	struct pvsched_accounting accounting;

	KUNIT_ASSERT_EQ(test, pvsched_accounting_init(&accounting, 20, 5), 0);
	KUNIT_ASSERT_EQ(test, pvsched_accounting_update(&accounting, 5,
							PVSCHED_BUDGET_DRAIN_GENERIC), 0);
	KUNIT_EXPECT_FALSE(test, accounting.cs.throttled);
	KUNIT_EXPECT_TRUE(test, accounting.generic.throttled);
}

static struct kunit_case pvsched_accounting_test_cases[] = {
	KUNIT_CASE(pvsched_accounting_init_test),
	KUNIT_CASE(pvsched_accounting_invalid_init_test),
	KUNIT_CASE(pvsched_accounting_charge_classes_test),
	KUNIT_CASE(pvsched_accounting_recovery_test),
	KUNIT_CASE(pvsched_accounting_cutoff_test),
	KUNIT_CASE(pvsched_accounting_zero_identity_test),
	KUNIT_CASE(pvsched_accounting_split_equivalence_test),
	KUNIT_CASE(pvsched_accounting_ordered_intervals_test),
	KUNIT_CASE(pvsched_accounting_invalid_charge_test),
	KUNIT_CASE(pvsched_accounting_saturation_test),
	KUNIT_CASE(pvsched_accounting_reverse_limits_test),
	{}
};

static struct kunit_suite pvsched_accounting_test_suite = {
	.name = "pvsched-accounting",
	.test_cases = pvsched_accounting_test_cases,
};

kunit_test_suite(pvsched_accounting_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
