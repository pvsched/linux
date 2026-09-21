// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <trace/events/sched.h>
#include <uapi/linux/sched/types.h>

struct pvsched_scheduler_test_ctx {
	struct completion ready;
	struct completion exit_seen;
	struct task_struct *task;
	struct sched_attr baseline_attr;
	u64 baseline_slack_ns;
	bool baseline_valid;
#ifdef CONFIG_TRACEPOINTS
	bool trace_registered;
#endif
};

static int pvsched_scheduler_test_thread(void *data)
{
	struct pvsched_scheduler_test_ctx *ctx = data;

	complete(&ctx->ready);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

#ifdef CONFIG_TRACEPOINTS
static void pvsched_scheduler_test_exit(void *data, struct task_struct *task,
					bool group_dead)
{
	struct pvsched_scheduler_test_ctx *ctx = data;

	if (task == READ_ONCE(ctx->task))
		complete(&ctx->exit_seen);
}
#endif

static int
pvsched_scheduler_test_restore(struct pvsched_scheduler_test_ctx *ctx)
{
	int ret;

	if (!ctx->baseline_valid || !ctx->task)
		return 0;

	ret = sched_setattr_nocheck(ctx->task, &ctx->baseline_attr);
	if (ret)
		return ret;
	task_lock(ctx->task);
	ctx->task->timer_slack_ns = ctx->baseline_slack_ns;
	task_unlock(ctx->task);
	return 0;
}

static void pvsched_scheduler_test_cleanup(void *data)
{
	struct pvsched_scheduler_test_ctx *ctx = data;

	pvsched_scheduler_test_restore(ctx);
	if (ctx->task) {
		kthread_stop(ctx->task);
		ctx->task = NULL;
	}
#ifdef CONFIG_TRACEPOINTS
	if (ctx->trace_registered) {
		unregister_trace_sched_process_exit(pvsched_scheduler_test_exit,
						    ctx);
		tracepoint_synchronize_unregister();
		ctx->trace_registered = false;
	}
#endif
}

static void pvsched_scheduler_interfaces_test(struct kunit *test)
{
	const u64 custom_slice_ns = 2 * NSEC_PER_MSEC;
	const u64 custom_slack_ns = 1234567;
	struct pvsched_scheduler_test_ctx *ctx;
	struct sched_task_state baseline;
	struct sched_task_state state;
	struct sched_attr attr;
	u64 default_slack_ns;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);
	init_completion(&ctx->ready);
	init_completion(&ctx->exit_seen);
	ctx->task = kthread_create(pvsched_scheduler_test_thread, ctx,
				   "pvsched-sched-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->task);
	ret = kunit_add_action_or_reset(test, pvsched_scheduler_test_cleanup,
					ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	sched_get_task_state(ctx->task, &baseline);
	if (baseline.policy != SCHED_NORMAL || baseline.scx_active)
		kunit_skip(test, "requires a SCHED_NORMAL task with sched_ext inactive");

	ctx->baseline_attr = (struct sched_attr) {
		.sched_policy = SCHED_NORMAL,
		.sched_flags = baseline.reset_on_fork ?
			SCHED_FLAG_RESET_ON_FORK : 0,
		.sched_nice = baseline.nice,
		.sched_runtime = baseline.slice_ns,
	};
	ctx->baseline_slack_ns = baseline.timer_slack_ns;
	ctx->baseline_valid = true;
	default_slack_ns = READ_ONCE(ctx->task->default_timer_slack_ns);
	if (!baseline.custom_slice)
		KUNIT_EXPECT_EQ(test, baseline.slice_ns, 0ULL);

#ifdef CONFIG_TRACEPOINTS
	ret = register_trace_sched_process_exit(pvsched_scheduler_test_exit,
						ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ctx->trace_registered = true;
#endif
	wake_up_process(ctx->task);
	KUNIT_ASSERT_NE(test,
			wait_for_completion_timeout(&ctx->ready, HZ), 0UL);

	attr = (struct sched_attr) {
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = NSEC_PER_MSEC,
		.sched_deadline = 2 * NSEC_PER_MSEC,
		.sched_period = 2 * NSEC_PER_MSEC,
	};
	KUNIT_EXPECT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), -EINVAL);

	attr = (struct sched_attr) {
		.sched_policy = SCHED_NORMAL,
		.sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
	};
	KUNIT_EXPECT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), -EINVAL);
	attr.sched_flags = SCHED_FLAG_UTIL_CLAMP_MAX;
	KUNIT_EXPECT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), -EINVAL);
	attr.sched_flags = SCHED_FLAG_UTIL_CLAMP;
	KUNIT_EXPECT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), -EINVAL);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, baseline.policy);
	KUNIT_EXPECT_EQ(test, state.nice, baseline.nice);
	KUNIT_EXPECT_EQ(test, state.rt_priority, baseline.rt_priority);
	KUNIT_EXPECT_EQ(test, state.reset_on_fork, baseline.reset_on_fork);
	KUNIT_EXPECT_EQ(test, state.custom_slice, baseline.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, baseline.slice_ns);

	attr = (struct sched_attr) {
		.sched_policy = SCHED_NORMAL,
		.sched_flags = SCHED_FLAG_RESET_ON_FORK,
		.sched_nice = 7,
		.sched_runtime = custom_slice_ns,
	};
	KUNIT_ASSERT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), 0);
	task_lock(ctx->task);
	ctx->task->timer_slack_ns = custom_slack_ns;
	task_unlock(ctx->task);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, (unsigned int)SCHED_NORMAL);
	KUNIT_EXPECT_EQ(test, state.nice, 7);
	KUNIT_EXPECT_EQ(test, state.rt_priority, 0U);
	KUNIT_EXPECT_TRUE(test, state.reset_on_fork);
	KUNIT_EXPECT_TRUE(test, state.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, custom_slice_ns);
	KUNIT_EXPECT_EQ(test, state.timer_slack_ns, custom_slack_ns);
	KUNIT_EXPECT_FALSE(test, state.scx_active);

	attr = (struct sched_attr) {
		.sched_policy = SCHED_FIFO,
		.sched_priority = 1,
	};
	KUNIT_ASSERT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), 0);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, (unsigned int)SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, state.nice, 7);
	KUNIT_EXPECT_EQ(test, state.rt_priority, 1U);
	KUNIT_EXPECT_FALSE(test, state.reset_on_fork);
	KUNIT_EXPECT_TRUE(test, state.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, custom_slice_ns);
	KUNIT_EXPECT_EQ(test, state.timer_slack_ns, 0ULL);

	attr = (struct sched_attr) {
		.sched_policy = SCHED_IDLE,
	};
	KUNIT_ASSERT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), 0);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, (unsigned int)SCHED_IDLE);
	KUNIT_EXPECT_EQ(test, state.nice, 7);
	KUNIT_EXPECT_EQ(test, state.rt_priority, 0U);
	KUNIT_EXPECT_TRUE(test, state.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, custom_slice_ns);
	KUNIT_EXPECT_EQ(test, state.timer_slack_ns, default_slack_ns);

	attr = (struct sched_attr) {
		.sched_policy = SCHED_NORMAL,
		.sched_nice = 7,
		.sched_runtime = custom_slice_ns,
	};
	KUNIT_ASSERT_EQ(test,
			sched_setattr_nocheck_nopi(ctx->task, &attr), 0);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, (unsigned int)SCHED_NORMAL);
	KUNIT_EXPECT_EQ(test, state.nice, 7);
	KUNIT_EXPECT_EQ(test, state.rt_priority, 0U);
	KUNIT_EXPECT_TRUE(test, state.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, custom_slice_ns);

	KUNIT_ASSERT_EQ(test, pvsched_scheduler_test_restore(ctx), 0);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, baseline.policy);
	KUNIT_EXPECT_EQ(test, state.nice, baseline.nice);
	KUNIT_EXPECT_EQ(test, state.rt_priority, baseline.rt_priority);
	KUNIT_EXPECT_EQ(test, state.reset_on_fork, baseline.reset_on_fork);
	KUNIT_EXPECT_EQ(test, state.custom_slice, baseline.custom_slice);
	KUNIT_EXPECT_EQ(test, state.slice_ns, baseline.slice_ns);
	KUNIT_EXPECT_EQ(test, state.timer_slack_ns, baseline.timer_slack_ns);
	KUNIT_EXPECT_EQ(test, kthread_stop(ctx->task), 0);
#ifdef CONFIG_TRACEPOINTS
	KUNIT_EXPECT_NE(test,
			wait_for_completion_timeout(&ctx->exit_seen, HZ), 0UL);
#endif
	ctx->task = NULL;
#ifdef CONFIG_TRACEPOINTS
	unregister_trace_sched_process_exit(pvsched_scheduler_test_exit, ctx);
	tracepoint_synchronize_unregister();
	ctx->trace_registered = false;
#endif
}

static struct kunit_case pvsched_scheduler_test_cases[] = {
	KUNIT_CASE(pvsched_scheduler_interfaces_test),
	{}
};

static struct kunit_suite pvsched_scheduler_test_suite = {
	.name = "pvsched-scheduler",
	.test_cases = pvsched_scheduler_test_cases,
};

kunit_test_suite(pvsched_scheduler_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched scheduler interfaces");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
