// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched/task.h>
#include <uapi/linux/sched/types.h>

#include "runner_runtime.h"

struct pvsched_runner_test_ctx {
	struct completion ready;
	struct task_struct *task;
	struct pvsched_runner_runtime runtime;
	struct sched_attr original_attr;
	struct pvsched_ticket_owner ticket_owner;
	u64 original_slack_ns;
	bool runtime_live;
	bool original_valid;
};

static int pvsched_runner_test_thread(void *data)
{
	struct pvsched_runner_test_ctx *ctx = data;

	complete(&ctx->ready);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static struct pvsched_default_policy_config pvsched_runner_test_config(void)
{
	return (struct pvsched_default_policy_config) {
		.cs_rt_prio = 60,
		.deadline_rt_prio = 50,
		.guest_rt_cap = 50,
	};
}

static struct pvsched_default_guest_area pvsched_runner_test_guest(void)
{
	struct pvsched_default_guest_area guest = { };

	guest.task_intent.current_task.sched_policy = SCHED_NORMAL;
	return guest;
}

static int pvsched_runner_test_reconcile(struct pvsched_runner_runtime *runtime,
				 enum pvsched_reconcile_event event,
				 const struct pvsched_default_guest_area *guest)
{
	struct pvsched_runner_event_input input = {
		.guest = guest,
		.mode_flags = runtime->nested_l2 ? PVSCHED_RUNNER_MODE_NESTED : 0,
	};

	return pvsched_runner_reconcile(runtime, event, &input);
}

static int pvsched_runner_test_guest_start(struct pvsched_runner_runtime *runtime)
{
	struct pvsched_runner_vmentry_input input = { };
	struct pvsched_host_area host = { };

	return pvsched_runner_vmentry(runtime, &input, &host);
}

static void pvsched_runner_test_cleanup(void *data)
{
	struct pvsched_runner_test_ctx *ctx = data;

	if (ctx->runtime_live) {
		pvsched_runner_runtime_destroy(&ctx->runtime);
		ctx->runtime_live = false;
	}
	if (ctx->task && ctx->original_valid) {
		sched_setattr_nocheck(ctx->task, &ctx->original_attr);
		task_lock(ctx->task);
		ctx->task->timer_slack_ns = ctx->original_slack_ns;
		task_unlock(ctx->task);
	}
	if (ctx->task)
		kthread_stop(ctx->task);
}

static struct pvsched_runner_test_ctx *
pvsched_runner_test_setup(struct kunit *test, int nice, u64 slice_ns,
			  u64 slack_ns)
{
	struct pvsched_runner_test_ctx *ctx;
	struct sched_task_state state;
	struct sched_attr attr;
	struct pvsched_default_policy_config config = pvsched_runner_test_config();
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		KUNIT_FAIL(test, "failed to allocate runner test context");
		return NULL;
	}
	init_completion(&ctx->ready);
	ctx->task = kthread_run(pvsched_runner_test_thread, ctx,
				"pvsched-runner-test");
	if (IS_ERR(ctx->task)) {
		KUNIT_FAIL(test, "failed to create runner test kthread: %ld",
			   PTR_ERR(ctx->task));
		ctx->task = NULL;
		return NULL;
	}
	if (!wait_for_completion_timeout(&ctx->ready, HZ)) {
		KUNIT_FAIL(test, "runner test kthread did not start");
		kthread_stop(ctx->task);
		ctx->task = NULL;
		return NULL;
	}

	sched_get_task_state(ctx->task, &state);
	if (state.policy != SCHED_NORMAL || state.scx_active) {
		kunit_skip(test, "requires a SCHED_NORMAL kthread with sched_ext inactive");
		kthread_stop(ctx->task);
		ctx->task = NULL;
		return NULL;
	}
	ctx->original_attr = (struct sched_attr) {
		.size = sizeof(ctx->original_attr),
		.sched_policy = SCHED_NORMAL,
		.sched_flags = state.reset_on_fork ? SCHED_FLAG_RESET_ON_FORK : 0,
		.sched_nice = state.nice,
		.sched_runtime = state.slice_ns,
	};
	ctx->original_slack_ns = state.timer_slack_ns;
	ctx->original_valid = true;

	attr = (struct sched_attr) {
		.size = sizeof(attr),
		.sched_policy = SCHED_NORMAL,
		.sched_nice = nice,
		.sched_runtime = slice_ns,
	};
	ret = sched_setattr_nocheck_nopi(ctx->task, &attr);
	if (ret) {
		KUNIT_FAIL(test, "failed to set test baseline: %d", ret);
		pvsched_runner_test_cleanup(ctx);
		return NULL;
	}
	if (slack_ns) {
		task_lock(ctx->task);
		ctx->task->timer_slack_ns = slack_ns;
		task_unlock(ctx->task);
	}
	ret = pvsched_runner_runtime_init(&ctx->runtime, ctx->task, 1,
				  &config, NSEC_PER_SEC,
				  NSEC_PER_SEC, &ctx->ticket_owner, true);
	if (ret) {
		KUNIT_FAIL(test, "failed to initialize runner runtime: %d", ret);
		pvsched_runner_test_cleanup(ctx);
		return NULL;
	}
	ctx->runtime_live = true;
	ret = kunit_add_action_or_reset(test, pvsched_runner_test_cleanup, ctx);
	if (ret) {
		KUNIT_FAIL(test, "failed to register runner cleanup: %d", ret);
		/* add_action_or_reset() has already run pvsched_runner_test_cleanup(). */
		return NULL;
	}
	return ctx;
}

static void pvsched_runner_expect_state(struct kunit *test,
					struct task_struct *task, unsigned int policy,
					int nice, unsigned int rt_prio)
{
	struct sched_task_state state;

	sched_get_task_state(task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, policy);
	KUNIT_EXPECT_EQ(test, state.nice, nice);
	KUNIT_EXPECT_EQ(test, state.rt_priority, rt_prio);
}

static void pvsched_runner_latent_nice_restore_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	guest.cs_state = 0;
	guest.task_intent.current_task.nice = 5;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.active);
}

static void pvsched_runner_normal_to_fifo_latent_nice_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();

	ctx = pvsched_runner_test_setup(test, 0, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = -5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, -5, 60);
	guest.cs_state = 0;
	guest.task_intent.current_task.nice = 0;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 0, 0);
}

static void pvsched_runner_slice_and_slack_restore_test(struct kunit *test)
{
	const u64 slice_ns = 2 * NSEC_PER_MSEC;
	const u64 slack_ns = 1234567;
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct sched_task_state state;

	ctx = pvsched_runner_test_setup(test, 5, slice_ns, slack_ns);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	guest.cs_state = 0;
	/* Every NORMAL guest request retains the captured custom slice. */
	guest.task_intent.current_task.nice = 10;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_CANCEL, &guest), 0);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_TRUE(test, state.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, slice_ns);
	KUNIT_EXPECT_EQ(test, state.timer_slack_ns, slack_ns);
}

static void pvsched_runner_same_tuple_skips_setter_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	u32 setter_calls;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	setter_calls = ctx->runtime.setter_calls;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.setter_calls, setter_calls);
}

static void pvsched_runner_run_leave_restores_and_cancels_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_guest_start(&ctx->runtime), 0);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	/* RUN_LEAVE normally follows terminal VMEXIT; keep a timer to test cleanup. */
	hrtimer_start(&ctx->runtime.cutoff_timer, ns_to_ktime(NSEC_PER_SEC),
		      HRTIMER_MODE_REL_PINNED_HARD);
	KUNIT_ASSERT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_LEAVE, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_FALSE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
}

static void pvsched_runner_run_leave_closes_guest_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_guest_start(&ctx->runtime), 0);
	KUNIT_ASSERT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));

	/* Defensive cleanup accepts a terminal path that did not report VMEXIT. */
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_LEAVE, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.phase, PVSCHED_RUNTIME_HOST);
	KUNIT_EXPECT_FALSE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
}

static void pvsched_runner_deferred_guest_start_has_no_late_setter_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	u32 setter_calls;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_guest_start(&ctx->runtime), 0);
	setter_calls = ctx->runtime.setter_calls;

	/* Inject an impossible accounting phase to fail while VMENTRY is unsafe. */
	ctx->runtime.accounting.phase = PVSCHED_RUNTIME_GUEST + 1;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_guest_start(&ctx->runtime), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.restore_owed_error, -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.setter_calls, setter_calls);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));

	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_VMEXIT, &guest), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.restore_owed_error, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.setter_calls, setter_calls + 1);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_nested_uses_last_guest_area_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	int ret;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ret = pvsched_runner_test_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_RUN_ENTER, &guest);
	KUNIT_ASSERT_EQ(test, ret, 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	ctx->runtime.nested_l2 = true;
	guest.cs_state = 0;
	guest.task_intent.current_task.nice = 10;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
}

static void pvsched_runner_initial_nested_uses_baseline_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	int ret;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ctx->runtime.nested_l2 = true;
	ret = pvsched_runner_test_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_CANCEL, &guest);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.active);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_malformed_snapshot_replaces_old_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	int ret;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ret = pvsched_runner_test_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_RUN_ENTER, &guest);
	KUNIT_ASSERT_EQ(test, ret, 0);

	guest.cs_state = 0;
	guest.reserved[0] = 1;
	ret = pvsched_runner_test_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_CANCEL, &guest);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);

	ctx->runtime.nested_l2 = true;
	guest.reserved[0] = 0;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ret = pvsched_runner_test_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_CANCEL, &guest);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_external_owner_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct sched_attr external = {
		.size = sizeof(external),
		.sched_policy = SCHED_NORMAL,
		.sched_nice = 10,
	};

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, sched_setattr_nocheck_nopi(ctx->task, &external), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			PVSCHED_RECONCILE_CANCEL, &guest), -EOWNERDEAD);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 10, 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.active);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
}

static struct kunit_case pvsched_runner_runtime_test_cases[] = {
	KUNIT_CASE(pvsched_runner_latent_nice_restore_test),
	KUNIT_CASE(pvsched_runner_normal_to_fifo_latent_nice_test),
	KUNIT_CASE(pvsched_runner_slice_and_slack_restore_test),
	KUNIT_CASE(pvsched_runner_same_tuple_skips_setter_test),
	KUNIT_CASE(pvsched_runner_run_leave_restores_and_cancels_test),
	KUNIT_CASE(pvsched_runner_run_leave_closes_guest_test),
	KUNIT_CASE(pvsched_runner_deferred_guest_start_has_no_late_setter_test),
	KUNIT_CASE(pvsched_runner_nested_uses_last_guest_area_test),
	KUNIT_CASE(pvsched_runner_initial_nested_uses_baseline_test),
	KUNIT_CASE(pvsched_runner_malformed_snapshot_replaces_old_test),
	KUNIT_CASE(pvsched_runner_external_owner_test),
	{}
};

static struct kunit_suite pvsched_runner_runtime_test_suite = {
	.name = "pvsched-runner-runtime",
	.test_cases = pvsched_runner_runtime_test_cases,
};

kunit_test_suite(pvsched_runner_runtime_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched runner reconciliation");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
