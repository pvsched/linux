// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched/task.h>
#include <uapi/linux/sched/types.h>

#include "default_policy.h"
#include "runner_runtime.h"

struct pvsched_runner_test_ctx {
	struct completion ready;
	struct task_struct *task;
	struct pvsched_runner_runtime runtime;
	struct sched_attr original_attr;
	u64 original_slack_ns;
	bool runtime_live;
	bool original_valid;
};

/*
 * The test policy's parameters: the default policy's, then bytes that only
 * the test policy uses, so that pvsched handles sizes other than the
 * default's.
 */
struct pvsched_runner_test_params {
	struct pvsched_default_params dflt;
	u8 marker;
	u8 reserved[15];
};
static_assert(sizeof(struct pvsched_runner_test_params) == 40);

struct pvsched_runner_test_state {
	u32 setter_calls;
	/* Test policy knobs and observations. */
	u32 map_calls;
	int map_error;
	/* Returned once by apply, or always by capture_baseline, if nonzero. */
	int apply_error;
	int capture_error;
	/* Replace the default's class when nonnegative. */
	int force_class;
	u32 force_flags;
	u8 marker;
	struct pvsched_map_input last_input;
	struct pvsched_runner_test_params last_applied;
};

/* The state of the running test, for policy callbacks on any thread. */
static struct pvsched_runner_test_state *pvsched_runner_test_state;

static u32 pvsched_runner_test_setter_calls(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;

	return state->setter_calls;
}

/*
 * The test policy wraps the default policy: its map adds the marker and
 * can force a class, flags or an error, or decline host reasons, and its
 * apply counts calls and injects failures.
 */
static int pvsched_runner_test_map(struct pvsched_policy_ctx *ctx,
				   const struct pvsched_map_input *in, void *out,
				   enum pvsched_boost_class *class, u32 *out_flags)
{
	struct pvsched_runner_test_state *state =
		READ_ONCE(pvsched_runner_test_state);
	struct pvsched_runner_test_params *params = out;
	int ret;

	state->map_calls++;
	state->last_input = *in;
	if (state->map_error)
		return state->map_error;
	ret = pvsched_default_policy_ops.map(ctx, in, out, class, out_flags);
	if (ret)
		return ret;
	params->marker = state->marker;
	if (state->force_class >= 0)
		*class = state->force_class;
	*out_flags |= state->force_flags;
	return 0;
}

static int pvsched_runner_test_apply(struct pvsched_policy_ctx *ctx,
				     const void *params)
{
	struct pvsched_runner_test_state *state =
		READ_ONCE(pvsched_runner_test_state);
	int ret;

	state->setter_calls++;
	if (state->apply_error) {
		ret = state->apply_error;
		state->apply_error = 0;
		return ret;
	}
	ret = pvsched_default_policy_ops.apply(ctx, params);
	if (!ret)
		memcpy(&state->last_applied, params, sizeof(state->last_applied));
	return ret;
}

static int pvsched_runner_test_capture(struct pvsched_policy_ctx *ctx,
				       void *baseline)
{
	struct pvsched_runner_test_state *state =
		READ_ONCE(pvsched_runner_test_state);

	if (state->capture_error)
		return state->capture_error;
	return pvsched_default_policy_ops.capture_baseline(ctx, baseline);
}

static struct pvsched_policy_ops pvsched_runner_test_ops = {
	.name = "kunit-runner",
	.version = 1,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = sizeof(struct pvsched_runner_test_params),
	.priv_size = sizeof(struct pvsched_default_priv),
	.map = pvsched_runner_test_map,
	.apply = pvsched_runner_test_apply,
};

/* The same policy without an ownership check. */
static struct pvsched_policy_ops pvsched_runner_test_unowned_ops = {
	.name = "kunit-runner-unowned",
	.version = 1,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = sizeof(struct pvsched_runner_test_params),
	.priv_size = sizeof(struct pvsched_default_priv),
	.map = pvsched_runner_test_map,
	.apply = pvsched_runner_test_apply,
};

static struct pvsched_policy_entry *pvsched_runner_test_entry;
static struct pvsched_policy_entry *pvsched_runner_test_unowned_entry;

static int pvsched_runner_suite_init(struct kunit_suite *suite)
{
	int ret;

	pvsched_runner_test_ops.capture_baseline = pvsched_runner_test_capture;
	pvsched_runner_test_ops.owned = pvsched_default_policy_ops.owned;
	pvsched_runner_test_unowned_ops.capture_baseline =
		pvsched_default_policy_ops.capture_baseline;
	ret = pvsched_register_policy(&pvsched_runner_test_ops);
	if (ret)
		return ret;
	ret = pvsched_register_policy(&pvsched_runner_test_unowned_ops);
	if (ret) {
		pvsched_unregister_policy(&pvsched_runner_test_ops);
		return ret;
	}
	pvsched_runner_test_entry = pvsched_policy_lookup("kunit-runner", 1);
	pvsched_runner_test_unowned_entry =
		pvsched_policy_lookup("kunit-runner-unowned", 1);
	return 0;
}

static void pvsched_runner_suite_exit(struct kunit_suite *suite)
{
	pvsched_unregister_policy(&pvsched_runner_test_unowned_ops);
	pvsched_unregister_policy(&pvsched_runner_test_ops);
	pvsched_policy_entry_put(pvsched_runner_test_unowned_entry);
	pvsched_policy_entry_put(pvsched_runner_test_entry);
}

static int pvsched_runner_test_init(struct kunit *test)
{
	struct pvsched_runner_test_state *state;

	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->force_class = -1;
	test->priv = state;
	WRITE_ONCE(pvsched_runner_test_state, state);
	return 0;
}

static int pvsched_runner_test_thread(void *data)
{
	struct pvsched_runner_test_ctx *ctx = data;

	complete(&ctx->ready);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static struct pvsched_default_guest_area pvsched_runner_test_guest(void)
{
	struct pvsched_default_guest_area guest = { };

	guest.task_intent.current_task.sched_policy = SCHED_NORMAL;
	return guest;
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
pvsched_runner_test_setup_policy(struct kunit *test,
				 struct pvsched_policy_entry *entry, int nice,
				 u64 slice_ns, u64 slack_ns)
{
	struct pvsched_runner_test_ctx *ctx;
	struct sched_task_state state;
	struct sched_attr attr;
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
				  entry, NSEC_PER_SEC,
				  NSEC_PER_SEC);
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

static struct pvsched_runner_test_ctx *
pvsched_runner_test_setup(struct kunit *test, int nice, u64 slice_ns,
			  u64 slack_ns)
{
	return pvsched_runner_test_setup_policy(test, pvsched_runner_test_entry,
						nice, slice_ns, slack_ns);
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	guest.cs_state = 0;
	guest.task_intent.current_task.nice = 5;
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, -5, 60);
	guest.cs_state = 0;
	guest.task_intent.current_task.nice = 0;
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	guest.cs_state = 0;
	/* Every NORMAL guest request retains the captured custom slice. */
	guest.task_intent.current_task.nice = 10;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	setter_calls = pvsched_runner_test_setter_calls(test);
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setter_calls);
}

static void pvsched_runner_run_leave_restores_and_cancels_test(struct kunit *test)
{
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_guest_start(&ctx->runtime), 0);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	/* RUN_LEAVE normally follows terminal VMEXIT; keep a timer to test cleanup. */
	hrtimer_start(&ctx->runtime.cutoff_timer, ns_to_ktime(NSEC_PER_SEC),
		      HRTIMER_MODE_REL_PINNED_HARD);
	KUNIT_ASSERT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_guest_start(&ctx->runtime), 0);
	KUNIT_ASSERT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));

	/* Defensive cleanup accepts a terminal path that did not report VMEXIT. */
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_guest_start(&ctx->runtime), 0);
	setter_calls = pvsched_runner_test_setter_calls(test);

	/* Inject an impossible accounting phase to fail while VMENTRY is unsafe. */
	ctx->runtime.accounting.phase = PVSCHED_RUNTIME_GUEST + 1;
	KUNIT_EXPECT_EQ(test, pvsched_runner_guest_start(&ctx->runtime), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.restore_owed_error, -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setter_calls);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));

	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_VMEXIT, &guest), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.restore_owed_error, 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setter_calls + 1);
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
	ret = pvsched_runner_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_RUN_ENTER, &guest);
	KUNIT_ASSERT_EQ(test, ret, 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	ctx->runtime.nested_l2 = true;
	guest.cs_state = 0;
	guest.task_intent.current_task.nice = 10;
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
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
	ret = pvsched_runner_reconcile(&ctx->runtime,
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
	ret = pvsched_runner_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_RUN_ENTER, &guest);
	KUNIT_ASSERT_EQ(test, ret, 0);

	guest.cs_state = 0;
	guest.reserved[0] = 1;
	ret = pvsched_runner_reconcile(&ctx->runtime,
				       PVSCHED_RECONCILE_CANCEL, &guest);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);

	ctx->runtime.nested_l2 = true;
	guest.reserved[0] = 0;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ret = pvsched_runner_reconcile(&ctx->runtime,
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			 PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, sched_setattr_nocheck_nopi(ctx->task, &external), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
			PVSCHED_RECONCILE_CANCEL, &guest), -EOWNERDEAD);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 10, 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.active);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
}

static const struct pvsched_runner_test_params *
pvsched_runner_test_applied(const struct pvsched_runner_test_ctx *ctx)
{
	return ctx->runtime.applied;
}

/* map() and apply() see the policy's own bytes, and equal bytes skip apply. */
static void pvsched_runner_policy_bytes_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	u32 setters, maps;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = -5;
	state->marker = 0x5a;
	maps = state->map_calls;
	setters = state->setter_calls;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, state->map_calls, maps + 1);
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters + 1);
	KUNIT_EXPECT_EQ(test, state->last_input.event,
			PVSCHED_RECONCILE_RUN_ENTER);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_applied(ctx)->marker, 0x5a);
	KUNIT_EXPECT_MEMEQ(test, &state->last_applied, ctx->runtime.applied,
			   sizeof(state->last_applied));
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);

	/* The same bytes again are not applied... */
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, state->map_calls, maps + 2);
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters + 1);

	/* ...but bytes only the policy reads are, with the same tuple. */
	state->marker = 0x5b;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters + 2);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_applied(ctx)->marker, 0x5b);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
}

/* A capture refusal of any kind reads as the default's: not supported. */
static void pvsched_runner_policy_capture_errors_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	static const int errors[] = { 1, -EPROTO, -EBUSY, -ENOMEM };
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_runner_runtime failed;
	unsigned int i;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		state->capture_error = errors[i];
		KUNIT_EXPECT_EQ(test, pvsched_runner_runtime_init(&failed,
				ctx->task, 1, pvsched_runner_test_entry,
				NSEC_PER_SEC, NSEC_PER_SEC), -EOPNOTSUPP);
		KUNIT_EXPECT_PTR_EQ(test, failed.task, NULL);
	}
	state->capture_error = 0;
}

/* The built-in default policy itself, driven through reconcile. */
static void pvsched_runner_default_policy_smoke_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup_policy(test,
					       pvsched_policy_default_entry(),
					       5, 0, 0);
	if (!ctx)
		return;
	KUNIT_EXPECT_PTR_EQ(test, ctx->runtime.ops,
			    (const struct pvsched_policy_ops *)
			    &pvsched_default_policy_ops);
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_CS);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC);
	guest.cs_state = 0;
	guest.task_intent.current_task.nice = -5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_LEAVE, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class,
			PVSCHED_CLASS_BASELINE);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.active);
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
	KUNIT_CASE(pvsched_runner_policy_bytes_test),
	KUNIT_CASE(pvsched_runner_policy_capture_errors_test),
	KUNIT_CASE(pvsched_runner_default_policy_smoke_test),
	{}
};

static struct kunit_suite pvsched_runner_runtime_test_suite = {
	.name = "pvsched-runner-runtime",
	.suite_init = pvsched_runner_suite_init,
	.suite_exit = pvsched_runner_suite_exit,
	.init = pvsched_runner_test_init,
	.test_cases = pvsched_runner_runtime_test_cases,
};

kunit_test_suite(pvsched_runner_runtime_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched runner reconciliation");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
