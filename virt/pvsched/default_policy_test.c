// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/string.h>

#include "default_policy.h"
#include "policy.h"

static struct pvsched_default_policy_config pvsched_test_config(void)
{
	return (struct pvsched_default_policy_config) {
		.cs_rt_prio = 60,
		.deadline_rt_prio = 50,
		.guest_rt_cap = 50,
	};
}

static struct pvsched_default_guest_area pvsched_test_guest(void)
{
	struct pvsched_default_guest_area guest = { };

	guest.task_intent.current_task.sched_policy = SCHED_NORMAL;
	return guest;
}

static void pvsched_expect_select(struct kunit *test,
				  const struct pvsched_default_policy_config *config,
				  int baseline_nice,
				  const struct pvsched_default_guest_area *guest,
				  bool cs_throttled, bool generic_throttled,
				  int expected_ret,
				  const struct pvsched_default_policy_result *expected)
{
	struct pvsched_default_policy_config config_before = *config;
	struct pvsched_default_guest_area guest_before = *guest;
	u8 result_before[sizeof(struct pvsched_default_policy_result)];
	struct pvsched_default_policy_result result;
	int ret;

	memset(&result, 0xa5, sizeof(result));
	result.prio = (struct pvsched_prio_desc) { SCHED_FIFO, -7, 9 };
	result.source = PVSCHED_DEFAULT_POLICY_CS;
	memcpy(result_before, &result, sizeof(result));
	/* The framework validates the guest area before any policy sees it. */
	if (!pvsched_default_guest_valid(guest))
		ret = -EINVAL;
	else
		ret = pvsched_default_policy_select(config, baseline_nice,
						    guest, cs_throttled,
						    generic_throttled, &result);
	KUNIT_EXPECT_EQ(test, ret, expected_ret);
	KUNIT_EXPECT_MEMEQ(test, config, &config_before, sizeof(*config));
	KUNIT_EXPECT_MEMEQ(test, guest, &guest_before, sizeof(*guest));
	if (expected_ret) {
		KUNIT_EXPECT_MEMEQ(test, &result, result_before, sizeof(result));
	} else {
		KUNIT_EXPECT_EQ(test, result.prio.sched_policy,
				expected->prio.sched_policy);
		KUNIT_EXPECT_EQ(test, result.prio.nice, expected->prio.nice);
		KUNIT_EXPECT_EQ(test, result.prio.rt_prio,
				expected->prio.rt_prio);
		KUNIT_EXPECT_EQ(test, result.source, expected->source);
	}
}

struct pvsched_mapping_case {
	struct pvsched_prio_desc guest;
	struct pvsched_prio_desc host;
};

static void pvsched_default_policy_mapping_test(struct kunit *test)
{
	static const struct pvsched_mapping_case cases[] = {
		{ { SCHED_NORMAL, -20, 0 }, { SCHED_NORMAL, -20, 0 } },
		{ { SCHED_NORMAL, 19, 0 }, { SCHED_NORMAL, 19, 0 } },
		{ { SCHED_BATCH, -20, 0 }, { SCHED_NORMAL, -20, 0 } },
		{ { SCHED_BATCH, 19, 0 }, { SCHED_NORMAL, 19, 0 } },
		{ { SCHED_FIFO, 0, 1 }, { SCHED_FIFO, 0, 1 } },
		{ { SCHED_FIFO, 0, 99 }, { SCHED_FIFO, 0, 50 } },
		{ { SCHED_RR, 0, 1 }, { SCHED_FIFO, 0, 1 } },
		{ { SCHED_RR, 0, 99 }, { SCHED_FIFO, 0, 50 } },
		{ { SCHED_IDLE, 0, 0 }, { SCHED_IDLE, 0, 0 } },
		{ { SCHED_DEADLINE, 0, 0 }, { SCHED_FIFO, 0, 50 } },
	};
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest;
	struct pvsched_default_policy_result expected;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		guest = pvsched_test_guest();
		guest.task_intent.current_task = cases[i].guest;
		expected = (struct pvsched_default_policy_result) {
			.prio = cases[i].host,
			.source = PVSCHED_DEFAULT_POLICY_TASK,
		};
		pvsched_expect_select(test, &config, -10, &guest, false, false,
				      0, &expected);
	}
}

static void pvsched_default_policy_invalid_tuple_test(struct kunit *test)
{
	static const struct pvsched_prio_desc cases[] = {
		{ SCHED_NORMAL, -21, 0 }, { SCHED_NORMAL, 20, 0 },
		{ SCHED_NORMAL, 0, 1 }, { SCHED_BATCH, -21, 0 },
		{ SCHED_BATCH, 20, 0 }, { SCHED_BATCH, 0, 1 },
		{ SCHED_FIFO, -1, 1 }, { SCHED_FIFO, 0, 0 },
		{ SCHED_FIFO, 0, 100 }, { SCHED_RR, 1, 1 },
		{ SCHED_RR, 0, 0 }, { SCHED_RR, 0, 100 },
		{ SCHED_IDLE, 1, 0 }, { SCHED_IDLE, 0, 1 },
		{ SCHED_DEADLINE, -1, 0 }, { SCHED_DEADLINE, 0, 1 },
		{ SCHED_EXT, 0, 0 }, { 4, 0, 0 },
	};
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		guest = pvsched_test_guest();
		guest.task_intent.current_task = cases[i];
		pvsched_expect_select(test, &config, 0, &guest, false, false,
				      -EINVAL, NULL);
	}
}

static void pvsched_default_policy_pending_validation_test(struct kunit *test)
{
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest;
	unsigned int i;

	for (i = 0; i < sizeof(guest.task_intent.pending_task); i++) {
		guest = pvsched_test_guest();
		((u8 *)&guest.task_intent.pending_task)[i] = 1;
		pvsched_expect_select(test, &config, 0, &guest, false, false,
				      -EINVAL, NULL);
	}

	guest = pvsched_test_guest();
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
	guest.task_intent.pending_task = (struct pvsched_prio_desc) {
		SCHED_FIFO, 0, 0,
	};
	pvsched_expect_select(test, &config, 0, &guest, false, false, -EINVAL,
			      NULL);
	guest = pvsched_test_guest();
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_IDLE << 1;
	pvsched_expect_select(test, &config, 0, &guest, false, false, -EINVAL,
			      NULL);
	/* The idle hint is valid and does not change the selection. */
	guest = pvsched_test_guest();
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_IDLE;
	pvsched_expect_select(test, &config, 0, &guest, false, false, 0,
			      &(struct pvsched_default_policy_result) {
				      .prio = { SCHED_NORMAL, 0, 0 },
				      .source = PVSCHED_DEFAULT_POLICY_TASK,
			      });
	guest = pvsched_test_guest();
	guest.task_intent.reserved = 1;
	pvsched_expect_select(test, &config, 0, &guest, false, false, -EINVAL,
			      NULL);
	guest = pvsched_test_guest();
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
	guest.task_intent.pending_task.sched_policy = SCHED_EXT;
	pvsched_expect_select(test, &config, 0, &guest, true, true, -EINVAL,
			      NULL);
}

static void pvsched_default_policy_guest_reserved_test(struct kunit *test)
{
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest;
	unsigned int i;

	guest = pvsched_test_guest();
	guest.cs_state = cpu_to_le64(PVSCHED_CS_RESERVED_MASK);
	pvsched_expect_select(test, &config, 0, &guest, false, false, -EINVAL,
			      NULL);
	for (i = 0; i < ARRAY_SIZE(guest.reserved); i++) {
		guest = pvsched_test_guest();
		guest.reserved[i] = 1;
		pvsched_expect_select(test, &config, 0, &guest, false, false,
				      -EINVAL, NULL);
	}
	guest = pvsched_test_guest();
	guest.interrupt_ack = cpu_to_le64(1);
	pvsched_expect_select(test, &config, 0, &guest, false, false, 0,
			      &(struct pvsched_default_policy_result) {
				      .prio = { SCHED_NORMAL, 0, 0 },
				      .source = PVSCHED_DEFAULT_POLICY_TASK,
			      });

	guest = pvsched_test_guest();
	guest.task_intent.current_task.sched_policy = SCHED_EXT;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	pvsched_expect_select(test, &config, 0, &guest, true, true, -EINVAL,
			      NULL);
}

static void pvsched_default_policy_config_baseline_test(struct kunit *test)
{
	static const struct pvsched_default_policy_config invalid[] = {
		{ 0, 50, 50 }, { 100, 50, 50 },
		{ 60, 0, 50 }, { 60, 100, 50 },
		{ 60, 50, 0 }, { 60, 50, 100 },
		{ 60, 60, 50 }, { 60, 50, 60 },
		{ 60, 99, 50 }, { 60, 50, 99 },
	};
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest = pvsched_test_guest();
	struct pvsched_default_policy_result expected = {
		.prio = { SCHED_NORMAL, 7, 0 },
		.source = PVSCHED_DEFAULT_POLICY_BASELINE,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(invalid); i++)
		pvsched_expect_select(test, &invalid[i], 0, &guest, false, false,
				      -EINVAL, NULL);
	pvsched_expect_select(test, &config, -21, &guest, false, false, -EINVAL,
			      NULL);
	pvsched_expect_select(test, &config, 20, &guest, false, false, -EINVAL,
			      NULL);
	pvsched_expect_select(test, &config, 7, &guest, false, true, 0,
			      &expected);
}

struct pvsched_pending_case {
	struct pvsched_prio_desc current_task;
	struct pvsched_prio_desc pending_task;
	struct pvsched_prio_desc selected;
};

static void pvsched_default_policy_pending_selection_test(struct kunit *test)
{
	static const struct pvsched_pending_case cases[] = {
		{ { SCHED_NORMAL, 10, 0 }, { SCHED_NORMAL, 0, 0 },
		  { SCHED_NORMAL, 0, 0 } },
		{ { SCHED_NORMAL, 0, 0 }, { SCHED_NORMAL, 10, 0 },
		  { SCHED_NORMAL, 0, 0 } },
		{ { SCHED_IDLE, 0, 0 }, { SCHED_NORMAL, 19, 0 },
		  { SCHED_NORMAL, 19, 0 } },
		{ { SCHED_NORMAL, 19, 0 }, { SCHED_IDLE, 0, 0 },
		  { SCHED_NORMAL, 19, 0 } },
		{ { SCHED_NORMAL, -20, 0 }, { SCHED_FIFO, 0, 1 },
		  { SCHED_FIFO, 0, 1 } },
		{ { SCHED_FIFO, 0, 1 }, { SCHED_NORMAL, -20, 0 },
		  { SCHED_FIFO, 0, 1 } },
		{ { SCHED_IDLE, 0, 0 }, { SCHED_FIFO, 0, 1 },
		  { SCHED_FIFO, 0, 1 } },
		{ { SCHED_FIFO, 0, 1 }, { SCHED_IDLE, 0, 0 },
		  { SCHED_FIFO, 0, 1 } },
		{ { SCHED_FIFO, 0, 20 }, { SCHED_RR, 0, 40 },
		  { SCHED_FIFO, 0, 40 } },
		{ { SCHED_FIFO, 0, 40 }, { SCHED_RR, 0, 20 },
		  { SCHED_FIFO, 0, 40 } },
		{ { SCHED_NORMAL, -5, 0 }, { SCHED_BATCH, -5, 0 },
		  { SCHED_NORMAL, -5, 0 } },
		{ { SCHED_FIFO, 0, 50 }, { SCHED_RR, 0, 50 },
		  { SCHED_FIFO, 0, 50 } },
	};
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest;
	struct pvsched_default_policy_result expected;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		guest = pvsched_test_guest();
		guest.task_intent.current_task = cases[i].current_task;
		guest.task_intent.pending_task = cases[i].pending_task;
		guest.task_intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
		expected = (struct pvsched_default_policy_result) {
			.prio = cases[i].selected,
			.source = PVSCHED_DEFAULT_POLICY_TASK,
		};
		pvsched_expect_select(test, &config, 0, &guest, false, false,
				      0, &expected);
	}
}

static void pvsched_default_policy_mapping_before_comparison_test(struct kunit *test)
{
	struct pvsched_default_policy_config config = {
		.cs_rt_prio = 80,
		.deadline_rt_prio = 70,
		.guest_rt_cap = 40,
	};
	struct pvsched_default_guest_area guest = pvsched_test_guest();
	struct pvsched_default_policy_result expected = {
		.prio = { SCHED_FIFO, 0, 70 },
		.source = PVSCHED_DEFAULT_POLICY_TASK,
	};

	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_FIFO, 0, 99,
	};
	guest.task_intent.pending_task = (struct pvsched_prio_desc) {
		SCHED_DEADLINE, 0, 0,
	};
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
	pvsched_expect_select(test, &config, 0, &guest, false, false, 0,
			      &expected);

	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_DEADLINE, 0, 0,
	};
	guest.task_intent.pending_task = (struct pvsched_prio_desc) {
		SCHED_FIFO, 0, 99,
	};
	pvsched_expect_select(test, &config, 0, &guest, false, false, 0,
			      &expected);
}

static void pvsched_default_policy_cs_throttle_test(struct kunit *test)
{
	static const u64 cs_bits[] = {
		PVSCHED_CS_NMI,
		PVSCHED_CS_HARDIRQ,
		PVSCHED_CS_SOFTIRQ,
		PVSCHED_CS_PREEMPT_DISABLED,
	};
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest = pvsched_test_guest();
	struct pvsched_default_policy_result expected = {
		.prio = { SCHED_FIFO, 0, 60 },
		.source = PVSCHED_DEFAULT_POLICY_CS,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cs_bits); i++) {
		guest.cs_state = cpu_to_le64(cs_bits[i]);
		pvsched_expect_select(test, &config, 0, &guest, false, false,
				      0, &expected);
	}

	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_FIFO, 0, 25,
	};
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	expected = (struct pvsched_default_policy_result) {
		.prio = { SCHED_FIFO, 0, 25 },
		.source = PVSCHED_DEFAULT_POLICY_TASK,
	};
	pvsched_expect_select(test, &config, 0, &guest, true, false, 0,
			      &expected);

	expected = (struct pvsched_default_policy_result) {
		.prio = { SCHED_NORMAL, 6, 0 },
		.source = PVSCHED_DEFAULT_POLICY_BASELINE,
	};
	pvsched_expect_select(test, &config, 6, &guest, false, true, 0,
			      &expected);
	pvsched_expect_select(test, &config, 6, &guest, true, true, 0,
			      &expected);

	guest.cs_state = 0;
	expected = (struct pvsched_default_policy_result) {
		.prio = { SCHED_FIFO, 0, 25 },
		.source = PVSCHED_DEFAULT_POLICY_TASK,
	};
	pvsched_expect_select(test, &config, 6, &guest, false, false, 0,
			      &expected);
	pvsched_expect_select(test, &config, 6, &guest, true, false, 0,
			      &expected);
}

struct pvsched_generic_throttle_case {
	struct pvsched_prio_desc current_task;
	struct pvsched_prio_desc pending_task;
	bool pending_valid;
	struct pvsched_prio_desc selected;
	enum pvsched_default_policy_source source;
};

static void pvsched_default_policy_generic_throttle_test(struct kunit *test)
{
	static const struct pvsched_generic_throttle_case cases[] = {
		/* Elevated and baseline-equivalent task intent is suppressed. */
		{ { SCHED_NORMAL, -5, 0 }, { }, false,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },
		{ { SCHED_FIFO, 0, 20 }, { }, false,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },
		{ { SCHED_DEADLINE, 0, 0 }, { }, false,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },
		{ { SCHED_NORMAL, 5, 0 }, { }, false,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },

		/* Current positive-nice and IDLE deboost intent remains valid. */
		{ { SCHED_NORMAL, 10, 0 }, { }, false,
		  { SCHED_NORMAL, 10, 0 }, PVSCHED_DEFAULT_POLICY_TASK },
		{ { SCHED_IDLE, 0, 0 }, { }, false,
		  { SCHED_IDLE, 0, 0 }, PVSCHED_DEFAULT_POLICY_TASK },

		/* Clamp both candidates before pending arbitration. */
		{ { SCHED_NORMAL, 10, 0 }, { SCHED_FIFO, 0, 20 }, true,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_TASK },
		{ { SCHED_IDLE, 0, 0 }, { SCHED_NORMAL, -5, 0 }, true,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_TASK },

		/*
		 * Clamp both candidates before arbitration.  A stale less-urgent
		 * pending deboost cannot lower a running elevated task below its
		 * clamped baseline intent.
		 */
		{ { SCHED_FIFO, 0, 40 }, { SCHED_IDLE, 0, 0 }, true,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },
		{ { SCHED_FIFO, 0, 40 }, { SCHED_NORMAL, 10, 0 }, true,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },

		/* A pending deboost wins only when more urgent than current. */
		{ { SCHED_NORMAL, 10, 0 }, { SCHED_IDLE, 0, 0 }, true,
		  { SCHED_NORMAL, 10, 0 }, PVSCHED_DEFAULT_POLICY_TASK },
		{ { SCHED_NORMAL, 5, 0 }, { SCHED_IDLE, 0, 0 }, true,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_BASELINE },
		{ { SCHED_IDLE, 0, 0 }, { SCHED_NORMAL, 5, 0 }, true,
		  { SCHED_NORMAL, 5, 0 }, PVSCHED_DEFAULT_POLICY_TASK },

		/* Select the more urgent of two eligible deboosts. */
		{ { SCHED_NORMAL, 15, 0 }, { SCHED_NORMAL, 10, 0 }, true,
		  { SCHED_NORMAL, 10, 0 }, PVSCHED_DEFAULT_POLICY_TASK },
		{ { SCHED_IDLE, 0, 0 }, { SCHED_NORMAL, 19, 0 }, true,
		  { SCHED_NORMAL, 19, 0 }, PVSCHED_DEFAULT_POLICY_TASK },
		{ { SCHED_NORMAL, 19, 0 }, { SCHED_IDLE, 0, 0 }, true,
		  { SCHED_NORMAL, 19, 0 }, PVSCHED_DEFAULT_POLICY_TASK },
	};
	struct pvsched_default_policy_config config = pvsched_test_config();
	struct pvsched_default_guest_area guest;
	struct pvsched_default_policy_result expected;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		guest = pvsched_test_guest();
		guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
		guest.task_intent.current_task = cases[i].current_task;
		if (cases[i].pending_valid) {
			guest.task_intent.pending_task = cases[i].pending_task;
			guest.task_intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
		}
		expected = (struct pvsched_default_policy_result) {
			.prio = cases[i].selected,
			.source = cases[i].source,
		};
		pvsched_expect_select(test, &config, 5, &guest, false, true,
				      0, &expected);
		pvsched_expect_select(test, &config, 5, &guest, true, true,
				      0, &expected);
	}
}

static struct kunit_case pvsched_default_policy_test_cases[] = {
	KUNIT_CASE(pvsched_default_policy_mapping_test),
	KUNIT_CASE(pvsched_default_policy_invalid_tuple_test),
	KUNIT_CASE(pvsched_default_policy_pending_validation_test),
	KUNIT_CASE(pvsched_default_policy_guest_reserved_test),
	KUNIT_CASE(pvsched_default_policy_config_baseline_test),
	KUNIT_CASE(pvsched_default_policy_pending_selection_test),
	KUNIT_CASE(pvsched_default_policy_mapping_before_comparison_test),
	KUNIT_CASE(pvsched_default_policy_cs_throttle_test),
	KUNIT_CASE(pvsched_default_policy_generic_throttle_test),
	{}
};

static struct kunit_suite pvsched_default_policy_test_suite = {
	.name = "pvsched-default-policy",
	.test_cases = pvsched_default_policy_test_cases,
};

kunit_test_suite(pvsched_default_policy_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
