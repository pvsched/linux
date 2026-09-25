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
	struct pvsched_ticket_owner ticket_owner;
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
	bool decline_reasons;
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
	const struct pvsched_runner_test_params *applied = ctx->applied;
	struct pvsched_runner_test_params *params = out;
	struct pvsched_map_input input = *in;
	int ret;

	state->map_calls++;
	state->last_input = *in;
	if (state->map_error)
		return state->map_error;
	if (state->decline_reasons) {
		input.reasons = 0;
		input.ticket_live = false;
	}
	ret = pvsched_default_policy_ops.map(ctx, &input, out, class, out_flags);
	if (ret)
		return ret;
	if (*out_flags & PVSCHED_MAP_HELD)
		*params = *applied;
	else
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
	setter_calls = pvsched_runner_test_setter_calls(test);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
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
	setter_calls = pvsched_runner_test_setter_calls(test);

	/* Inject an impossible accounting phase to fail while VMENTRY is unsafe. */
	ctx->runtime.accounting.phase = PVSCHED_RUNTIME_GUEST + 1;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_guest_start(&ctx->runtime), -EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.restore_owed_error, -EINVAL);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setter_calls);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));

	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
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

static int pvsched_runner_test_event(struct pvsched_runner_test_ctx *ctx,
				     enum pvsched_reconcile_event event,
				     struct pvsched_default_guest_area *guest,
				     enum pvsched_runner_position position,
				     u32 mode_flags)
{
	struct pvsched_runner_event_input input = {
		.guest = guest,
		.mode_flags = mode_flags,
	};

	/* Model the target hook that established the remote callback's state. */
	ctx->runtime.position = position;

	return pvsched_runner_reconcile(&ctx->runtime, event, &input);
}

static int pvsched_runner_test_vmentry(struct pvsched_runner_test_ctx *ctx,
				       struct pvsched_default_guest_area *guest,
				       bool ready, u32 mode_flags,
				       struct pvsched_host_area *host)
{
	struct pvsched_runner_vmentry_input input = {
		.guest = *guest,
		.mode_flags = mode_flags,
		.interrupt_ready = ready,
	};

	return pvsched_runner_vmentry(&ctx->runtime, &input, host);
}

static void pvsched_runner_halt_handoff_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u64 first;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	first = ctx->runtime.interrupt_ticket;
	KUNIT_EXPECT_NE(test, first, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	guest.interrupt_ack = first;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	KUNIT_EXPECT_GT(test, ctx->runtime.interrupt_ticket, first);
	KUNIT_EXPECT_GT(test, ktime_to_ns(hrtimer_get_remaining(
		&ctx->runtime.cutoff_timer)), 0LL);
}

static void pvsched_runner_hlt_exit_boosts_until_block_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	/* An idle guest publishes the baseline tuple. */
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_VMEXIT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);

	/* The HLT exit takes the halt boost at once ... */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_VMEXIT, &guest, PVSCHED_RUNNER_HOST,
		PVSCHED_RUNNER_MODE_HLT_EXIT), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.reasons &
			  PVSCHED_RUNNER_REASON_HALT);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	/* ... so blocking needs no further scheduler change. */
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
}

static void pvsched_runner_throttled_hlt_exit_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	ctx->runtime.accounting.accounting.cs.debt_ns = NSEC_PER_SEC;
	ctx->runtime.accounting.accounting.cs.throttled = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_VMEXIT, &guest, PVSCHED_RUNNER_HOST,
		PVSCHED_RUNNER_MODE_HLT_EXIT), 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.reasons &
			   PVSCHED_RUNNER_REASON_HALT);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static int pvsched_runner_test_vmexit(struct pvsched_runner_test_ctx *ctx,
				      struct pvsched_default_guest_area *guest,
				      bool idle, bool cs)
{
	guest->task_intent.flags = idle ? PVSCHED_INTENT_FLAG_IDLE : 0;
	guest->cs_state = cpu_to_le64(cs ? PVSCHED_CS_NMI : 0);
	return pvsched_runner_test_event(ctx, PVSCHED_RECONCILE_VMEXIT, guest,
					 PVSCHED_RUNNER_HOST, 0);
}

static void pvsched_runner_idle_hold_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	guest.task_intent.current_task.nice = 5;

	/* A boost is kept for an idle guest and charged as CS time ... */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC);
	KUNIT_EXPECT_NE(test, ctx->runtime.idle_hold_end_ns, 0ULL);

	/* ... until its grace ends; then deboost for the rest of the episode. */
	ctx->runtime.idle_hold_end_ns = 1;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.idle_hold_end_ns, 1ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 true), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);

	/* Leaving idle starts a new episode with a fresh grace. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.idle_hold_end_ns, 0ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	/* A halt ends the episode as well. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.idle_hold_end_ns, 0ULL);
}

static void pvsched_runner_idle_hold_stale_deadline_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.idle_holding);

	/* The guest's own request keeps FIFO: nothing is held any more. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 true), 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.idle_holding);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);

	/*
	 * A grace deadline in the past must not force an exit at entry: the
	 * guest has to run to change its request.  The cutoff follows the
	 * remaining CS budget instead.
	 */
	ctx->runtime.idle_hold_end_ns = 1;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
							  &host), 0);
	KUNIT_EXPECT_GT(test, ktime_to_ns(hrtimer_get_remaining(
		&ctx->runtime.cutoff_timer)), 100LL * NSEC_PER_USEC);

	/* The passed grace is spent: an idle exit now deboosts. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.idle_hold_end_ns, 1ULL);
}

static void pvsched_runner_idle_hold_cutoff_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	s64 remaining;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);

	/* A held boost is capped at its grace, well inside the CS budget. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
							  &host), 0);
	remaining = ktime_to_ns(hrtimer_get_remaining(&ctx->runtime.cutoff_timer));
	KUNIT_EXPECT_GT(test, remaining, 0LL);
	KUNIT_EXPECT_LE(test, remaining, 200LL * NSEC_PER_USEC);

	/* At the grace deadline the forced exit deboosts the idle guest. */
	ctx->runtime.idle_hold_end_ns = 1;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.idle_holding);
}

static void pvsched_runner_idle_hold_after_wake_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	guest.task_intent.current_task.nice = 5;
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_IDLE;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	/* VMENTRY retires the halt reason and forces an exit to deboost. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
							  &host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);

	/* Still in its idle task, the woken guest keeps the boost ... */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.idle_holding);

	/* ... until it switches to the woken task. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_idle_hold_paths_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	guest.task_intent.current_task.nice = 5;

	/* A cancelled entry holds like an exit. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_IDLE;
	guest.cs_state = 0;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_CANCEL, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.idle_holding);

	/* A return to the VMM ends the episode and restores the baseline. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_RUN_LEAVE, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.idle_holding);
	KUNIT_EXPECT_EQ(test, ctx->runtime.idle_hold_end_ns, 0ULL);

	/* A held guest-RT tuple is charged as CS time, not generic time. */
	guest.task_intent.flags = 0;
	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_FIFO, 0, 99,
	};
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_RUN_ENTER, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 50);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_GENERIC);
	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_NORMAL, 5, 0,
	};
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 50);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC);
}

static void pvsched_runner_idle_hold_limits_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;

	/* Off: an idle guest is deboosted as before. */
	ctx->runtime.idle_hold_ns = 0;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);

	/* The hint never raises an unboosted vCPU. */
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.idle_hold_end_ns, 0ULL);

	/* A throttled runner is not held. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 true), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	ctx->runtime.accounting.accounting.cs.debt_ns = NSEC_PER_SEC;
	ctx->runtime.accounting.accounting.cs.throttled = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_assist_expiry_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
}

static void pvsched_runner_no_ticket_retires_assists_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true,
		PVSCHED_RUNNER_MODE_NO_TICKET, &host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
}

/* Guest CS that still selects the CS FIFO needs no forced deboost exit. */
static void pvsched_runner_guest_cs_avoids_forced_exit_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	struct sched_task_state state;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	guest.cs_state = cpu_to_le64(PVSCHED_CS_HARDIRQ);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true,
		PVSCHED_RUNNER_MODE_NO_TICKET, &host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	/* Only the ordinary cutoff is armed, not an immediate exit. */
	KUNIT_EXPECT_GT(test, ktime_to_ns(hrtimer_get_remaining(
		&ctx->runtime.cutoff_timer)), 0LL);
	sched_get_task_state(ctx->task, &state);
	KUNIT_EXPECT_EQ(test, state.policy, SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, state.rt_priority, 60U);
}

static void pvsched_runner_unsupported_revocation_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true,
		PVSCHED_RUNNER_MODE_UNSUPPORTED, &host), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.revoke_pending);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.revoke_pending);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.active);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_remote_unsupported_is_record_only_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_runner_test_params applied;
	u64 anchor_wall;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	ctx->runtime.interrupt_ticket = 7;
	ctx->runtime.reasons = PVSCHED_RUNNER_REASON_INJECT;
	setters = pvsched_runner_test_setter_calls(test);
	anchor_wall = ctx->runtime.accounting.anchor.wall_ns;
	memcpy(&applied, ctx->runtime.applied, sizeof(applied));
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_QEMU,
		PVSCHED_RUNNER_MODE_UNSUPPORTED), 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.revoke_pending);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 7ULL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons,
			PVSCHED_RUNNER_REASON_INJECT);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.anchor.wall_ns,
			anchor_wall);
	KUNIT_EXPECT_MEMEQ(test, ctx->runtime.applied, &applied,
			   sizeof(applied));
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.phase,
			PVSCHED_RUNTIME_HOST);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_CANCEL, &guest, PVSCHED_RUNNER_HOST,
		PVSCHED_RUNNER_MODE_UNSUPPORTED),
		-EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.active);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
}

static void pvsched_runner_position_is_target_owned_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_event_input inject = { };
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_LEAVE, NULL), 0);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_INJECT, &inject), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.position, PVSCHED_RUNNER_QEMU);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);

	/* A remote nested hint cannot overwrite target-local mode state. */
	inject.mode_flags = PVSCHED_RUNNER_MODE_NESTED;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_INJECT, &inject), 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.nested_l2);
	ctx->runtime.position = PVSCHED_RUNNER_HOST;
	ctx->runtime.nested_l2 = true;
	inject.mode_flags = 0;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_INJECT, &inject), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.nested_l2);
}

static void pvsched_runner_blocked_inject_is_record_only_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_event_input inject = { };
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	ctx->runtime.accounting.accounting.cs.debt_ns = U64_MAX;
	ctx->runtime.accounting.accounting.cs.throttled = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_HALT, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 0, 0);
	ctx->runtime.accounting.accounting.cs.debt_ns = 0;
	ctx->runtime.accounting.accounting.cs.throttled = false;
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_INJECT, &inject), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.reasons &
			  PVSCHED_RUNNER_REASON_INJECT);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_UNHALT, NULL), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_INJECT, &inject), 0);
	KUNIT_EXPECT_GT(test, pvsched_runner_test_setter_calls(test), setters);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 0, 60);
}

static void pvsched_runner_missing_guest_is_event_error_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_event_input input = { };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &input), -EINVAL);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.active);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.last_guest_area_valid);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
}

static void pvsched_runner_revocation_skips_accounting_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	struct pvsched_runtime_accounting accounting;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	accounting = ctx->runtime.accounting;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_CANCEL, &guest, PVSCHED_RUNNER_HOST,
		PVSCHED_RUNNER_MODE_UNSUPPORTED), -EOPNOTSUPP);
	KUNIT_EXPECT_MEMEQ(test, &ctx->runtime.accounting, &accounting,
			   sizeof(accounting));
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
}

static void pvsched_runner_closing_blocks_deferred_restore_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	ctx->runtime.restore_owed_error = -EIO;
	ctx->runtime.closing = true;
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	pvsched_runner_runtime_destroy(&ctx->runtime);
	ctx->runtime_live = false;
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_unsupported_run_leave_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_RUN_LEAVE, &guest, PVSCHED_RUNNER_QEMU,
		PVSCHED_RUNNER_MODE_UNSUPPORTED), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.active);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_halt_not_ready_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_TRUE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
}

static void pvsched_runner_unhalt_does_not_freshly_boost_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ctx->runtime.accounting.accounting.cs.debt_ns = NSEC_PER_SEC;
	ctx->runtime.accounting.accounting.cs.throttled = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	ctx->runtime.accounting.accounting.cs.debt_ns = 0;
	ctx->runtime.accounting.accounting.cs.throttled = false;
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_revocation_respects_external_owner_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	struct sched_attr external = {
		.size = sizeof(external),
		.sched_policy = SCHED_NORMAL,
		.sched_nice = 10,
	};
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, sched_setattr_nocheck_nopi(ctx->task, &external), 0);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_CANCEL, &guest, PVSCHED_RUNNER_HOST,
		PVSCHED_RUNNER_MODE_UNSUPPORTED), -EOWNERDEAD);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.restore_failed);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 10, 0);
}

static void pvsched_runner_cancel_preserves_assists_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons,
			PVSCHED_RUNNER_REASON_HALT |
			PVSCHED_RUNNER_REASON_INJECT);
}

static void pvsched_runner_ticket_guards_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u64 ticket;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	ticket = ctx->runtime.interrupt_ticket;
	KUNIT_ASSERT_NE(test, ticket, 0ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_GUEST, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	guest.interrupt_ack = ticket + 1;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	ctx->runtime.interrupt_ticket = 0;
	ctx->ticket_owner.last_ticket = U64_MAX;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
}

static void pvsched_runner_nested_suppresses_handoff_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { .applied_state.raw = cpu_to_le64(U64_MAX) };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	ctx->runtime.interrupt_ticket = 9;
	guest.interrupt_ack = 9;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true,
		PVSCHED_RUNNER_MODE_NESTED, &host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 9ULL);
	KUNIT_EXPECT_EQ(test, host.applied_state.raw, cpu_to_le64(U64_MAX));
}

static void pvsched_runner_cutoff_downgrades_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	ctx->runtime.interrupt_ticket = 17;
	ctx->runtime.accounting.accounting.cs.debt_ns = NSEC_PER_SEC;
	ctx->runtime.accounting.accounting.cs.throttled = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
}

static void pvsched_runner_vmentry_feedback_matrix_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	union pvsched_applied_state state;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	ctx->runtime.accounting.accounting.cs.debt_ns =
		ctx->runtime.accounting.accounting.cs.limit_ns;
	ctx->runtime.accounting.accounting.generic.debt_ns =
		ctx->runtime.accounting.accounting.generic.limit_ns;
	ctx->runtime.accounting.accounting.cs.throttled = true;
	ctx->runtime.accounting.accounting.generic.throttled = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	state.raw = host.applied_state.raw;
	/* The class of the value applied, whatever the budgets say now. */
	KUNIT_EXPECT_EQ(test, state.boost, PVSCHED_BOOST_CS);
	KUNIT_EXPECT_EQ(test, state.reserved0[0] | state.reserved0[1] |
			state.reserved0[2] | state.reserved1[0] |
			state.reserved1[1], 0);
	KUNIT_EXPECT_TRUE(test, state.flags & PVSCHED_APPLIED_CS_THROTTLED);
	KUNIT_EXPECT_TRUE(test, state.flags & PVSCHED_APPLIED_TOTAL_THROTTLED);
	KUNIT_EXPECT_TRUE(test, state.hints & PVSCHED_HINT_KICK_DEBOOST);
	ctx->runtime.deboost_notify = false;
	host.applied_state.raw = 0;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	state.raw = host.applied_state.raw;
	KUNIT_EXPECT_FALSE(test, state.hints & PVSCHED_HINT_KICK_DEBOOST);
}

static void pvsched_runner_inject_eligibility_matrix_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	u32 setters;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.reasons &
			  PVSCHED_RUNNER_REASON_INJECT);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_GUEST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_QEMU, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	ctx->runtime.reasons = 0;
	guest.reserved[0] = 1;
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), -EINVAL);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.last_guest_area_valid);
	setters = pvsched_runner_test_setter_calls(test);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_setter_calls(test), setters);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
}

static void pvsched_runner_ticket_survives_run_boundary_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u64 ticket;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	ticket = ctx->runtime.interrupt_ticket;
	KUNIT_ASSERT_NE(test, ticket, 0ULL);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_LEAVE, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
}

static void pvsched_runner_expiry_forces_safe_downgrade_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	ktime_t now;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	now = ktime_get();
	KUNIT_EXPECT_LE(test, ktime_compare(hrtimer_get_expires(
		&ctx->runtime.cutoff_timer), now), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_REFILL);
}

static void pvsched_runner_combined_reason_lifecycle_test(struct kunit *test)
{
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	u64 ticket;
	ktime_t now;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons,
			PVSCHED_RUNNER_REASON_HALT |
			PVSCHED_RUNNER_REASON_INJECT);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, 0ULL);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	now = ktime_get();
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_EXPECT_LE(test, ktime_compare(hrtimer_get_expires(
		&ctx->runtime.cutoff_timer), now), 0);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_REFILL);

	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	ticket = ctx->runtime.interrupt_ticket;
	KUNIT_ASSERT_NE(test, ticket, 0ULL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	guest.interrupt_ack = 0;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, true, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.reasons &
			  PVSCHED_RUNNER_REASON_HALT);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_LEAVE, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.interrupt_ticket, ticket);
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, state->map_calls, maps + 1);
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters + 1);
	KUNIT_EXPECT_EQ(test, state->last_input.event,
			PVSCHED_RECONCILE_RUN_ENTER);
	KUNIT_EXPECT_FALSE(test, state->last_input.hold_offered);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_applied(ctx)->marker, 0x5a);
	KUNIT_EXPECT_MEMEQ(test, &state->last_applied, ctx->runtime.applied,
			   sizeof(state->last_applied));
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);

	/* The same bytes again are not applied... */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, state->map_calls, maps + 2);
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters + 1);

	/* ...but bytes only the policy reads are, with the same tuple. */
	state->marker = 0x5b;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters + 2);
	KUNIT_EXPECT_EQ(test, pvsched_runner_test_applied(ctx)->marker, 0x5b);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
}

/* The returned class alone selects the budgets, and UNHALT keeps it. */
static void pvsched_runner_policy_class_charging_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;
	u32 setters, maps;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_REFILL);
	setters = state->setter_calls;
	state->force_class = PVSCHED_CLASS_TASK;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_GENERIC);
	state->force_class = PVSCHED_CLASS_CS;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_CS);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC);
	/* A relabelled value is still the same bytes: nothing is applied. */
	KUNIT_EXPECT_EQ(test, state->setter_calls, setters);

	/* UNHALT charges the stored class without asking the policy. */
	state->force_class = PVSCHED_CLASS_TASK;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	state->force_class = PVSCHED_CLASS_BASELINE;
	maps = state->map_calls;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, state->map_calls, maps);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_GENERIC);

	/* The class is trusted: an elevated value labelled BASELINE refills. */
	guest.task_intent.current_task.nice = -5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_REFILL);
}

/* The forced exit after a retired host reason follows the CS class only. */
static void pvsched_runner_policy_kick_by_class_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);

	/* The injection boost labelled TASK: no forced exit at entry. */
	state->force_class = PVSCHED_CLASS_TASK;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_TRUE(test, state->last_input.reasons &
			  PVSCHED_RUNNER_REASON_INJECT);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.reasons, 0UL);
	KUNIT_EXPECT_GT(test, ktime_compare(hrtimer_get_expires(
		&ctx->runtime.cutoff_timer), ktime_get()), 0);
	pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	state->force_class = -1;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_VMEXIT, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);

	/* The same boost labelled CS forces the exit. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_CS);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	KUNIT_EXPECT_LE(test, ktime_compare(hrtimer_get_expires(
		&ctx->runtime.cutoff_timer), ktime_get()), 0);
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
				NSEC_PER_SEC, NSEC_PER_SEC, &ctx->ticket_owner,
				true), -EOPNOTSUPP);
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
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 60);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_CS);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC);
	guest.cs_state = 0;
	guest.task_intent.current_task.nice = -5;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_CANCEL, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, -5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_LEAVE, &guest), 0);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class,
			PVSCHED_CLASS_BASELINE);
	KUNIT_EXPECT_EQ(test, ctx->runtime.last_fault, 0);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.active);
}

/* A hold keeps the applied value and class, is charged CS and is bounded. */
static void pvsched_runner_policy_idle_hold_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	ctx->runtime.idle_hold_ns = 200 * NSEC_PER_USEC;
	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_FIFO, 0, 10 };
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, false,
							 false), 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_GENERIC);

	guest.task_intent.current_task = (struct pvsched_prio_desc) {
		SCHED_NORMAL, 5, 0 };
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	KUNIT_EXPECT_TRUE(test, state->last_input.hold_offered);
	pvsched_runner_expect_state(test, ctx->task, SCHED_FIFO, 5, 10);
	KUNIT_EXPECT_TRUE(test, ctx->runtime.idle_holding);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class, PVSCHED_CLASS_TASK);
	KUNIT_EXPECT_EQ(test, ctx->runtime.accounting.domain,
			PVSCHED_BUDGET_DRAIN_CS_GENERIC);

	/* Once the grace has passed, the hold is no longer offered. */
	ctx->runtime.idle_hold_end_ns = 1;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmexit(ctx, &guest, true,
							 false), 0);
	KUNIT_EXPECT_FALSE(test, state->last_input.hold_offered);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_FALSE(test, ctx->runtime.idle_holding);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class,
			PVSCHED_CLASS_BASELINE);
}

/* A policy may decline host reasons; declining is never elevation. */
static void pvsched_runner_policy_declines_reasons_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = 5;
	state->decline_reasons = true;
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
		PVSCHED_RECONCILE_RUN_ENTER, &guest), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_INJECT, NULL, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_EXPECT_EQ(test, state->last_input.event,
			PVSCHED_RECONCILE_INJECT);
	KUNIT_EXPECT_TRUE(test, state->last_input.reasons &
			  PVSCHED_RUNNER_REASON_INJECT);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	KUNIT_EXPECT_EQ(test, ctx->runtime.applied_class,
			PVSCHED_CLASS_BASELINE);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_HALT, &guest, PVSCHED_RUNNER_BLOCKED, 0), 0);
	KUNIT_EXPECT_TRUE(test, state->last_input.reasons &
			  PVSCHED_RUNNER_REASON_HALT);
	pvsched_runner_expect_state(test, ctx->task, SCHED_NORMAL, 5, 0);
	/* Nothing elevated, so the retired reasons force no exit. */
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_event(ctx,
		PVSCHED_RECONCILE_UNHALT, &guest, PVSCHED_RUNNER_HOST, 0), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest, false, 0,
		&host), 0);
	KUNIT_EXPECT_FALSE(test, hrtimer_active(&ctx->runtime.cutoff_timer));
}

/* VMENTRY publishes the class of the applied value as the boost. */
static void pvsched_runner_policy_boost_feedback_test(struct kunit *test)
{
	struct pvsched_runner_test_state *state = test->priv;
	struct pvsched_default_guest_area guest = pvsched_runner_test_guest();
	struct pvsched_host_area host = { };
	struct pvsched_runner_test_ctx *ctx;
	union pvsched_applied_state applied;
	static const enum pvsched_boost_class classes[] = {
		PVSCHED_CLASS_TASK, PVSCHED_CLASS_CS, PVSCHED_CLASS_BASELINE,
	};
	unsigned int i;

	ctx = pvsched_runner_test_setup(test, 5, 0, 0);
	if (!ctx)
		return;
	guest.task_intent.current_task.nice = -5;
	for (i = 0; i < ARRAY_SIZE(classes); i++) {
		state->force_class = classes[i];
		KUNIT_ASSERT_EQ(test, pvsched_runner_test_reconcile(&ctx->runtime,
			i ? PVSCHED_RECONCILE_VMEXIT : PVSCHED_RECONCILE_RUN_ENTER,
			&guest), 0);
		KUNIT_ASSERT_EQ(test, pvsched_runner_test_vmentry(ctx, &guest,
			false, 0, &host), 0);
		applied.raw = host.applied_state.raw;
		KUNIT_EXPECT_EQ(test, applied.boost, (u8)classes[i]);
		pvsched_runner_guest_exit_irqoff(&ctx->runtime);
	}
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
	KUNIT_CASE(pvsched_runner_halt_handoff_test),
	KUNIT_CASE(pvsched_runner_hlt_exit_boosts_until_block_test),
	KUNIT_CASE(pvsched_runner_throttled_hlt_exit_test),
	KUNIT_CASE(pvsched_runner_idle_hold_test),
	KUNIT_CASE(pvsched_runner_idle_hold_limits_test),
	KUNIT_CASE(pvsched_runner_idle_hold_stale_deadline_test),
	KUNIT_CASE(pvsched_runner_idle_hold_cutoff_test),
	KUNIT_CASE(pvsched_runner_idle_hold_after_wake_test),
	KUNIT_CASE(pvsched_runner_idle_hold_paths_test),
	KUNIT_CASE(pvsched_runner_assist_expiry_test),
	KUNIT_CASE(pvsched_runner_no_ticket_retires_assists_test),
	KUNIT_CASE(pvsched_runner_guest_cs_avoids_forced_exit_test),
	KUNIT_CASE(pvsched_runner_unsupported_revocation_test),
	KUNIT_CASE(pvsched_runner_remote_unsupported_is_record_only_test),
	KUNIT_CASE(pvsched_runner_position_is_target_owned_test),
	KUNIT_CASE(pvsched_runner_blocked_inject_is_record_only_test),
	KUNIT_CASE(pvsched_runner_missing_guest_is_event_error_test),
	KUNIT_CASE(pvsched_runner_revocation_skips_accounting_test),
	KUNIT_CASE(pvsched_runner_closing_blocks_deferred_restore_test),
	KUNIT_CASE(pvsched_runner_unsupported_run_leave_test),
	KUNIT_CASE(pvsched_runner_halt_not_ready_test),
	KUNIT_CASE(pvsched_runner_unhalt_does_not_freshly_boost_test),
	KUNIT_CASE(pvsched_runner_revocation_respects_external_owner_test),
	KUNIT_CASE(pvsched_runner_cancel_preserves_assists_test),
	KUNIT_CASE(pvsched_runner_ticket_guards_test),
	KUNIT_CASE(pvsched_runner_nested_suppresses_handoff_test),
	KUNIT_CASE(pvsched_runner_cutoff_downgrades_test),
	KUNIT_CASE(pvsched_runner_vmentry_feedback_matrix_test),
	KUNIT_CASE(pvsched_runner_inject_eligibility_matrix_test),
	KUNIT_CASE(pvsched_runner_ticket_survives_run_boundary_test),
	KUNIT_CASE(pvsched_runner_expiry_forces_safe_downgrade_test),
	KUNIT_CASE(pvsched_runner_combined_reason_lifecycle_test),
	KUNIT_CASE(pvsched_runner_policy_bytes_test),
	KUNIT_CASE(pvsched_runner_policy_class_charging_test),
	KUNIT_CASE(pvsched_runner_policy_kick_by_class_test),
	KUNIT_CASE(pvsched_runner_policy_capture_errors_test),
	KUNIT_CASE(pvsched_runner_default_policy_smoke_test),
	KUNIT_CASE(pvsched_runner_policy_idle_hold_test),
	KUNIT_CASE(pvsched_runner_policy_declines_reasons_test),
	KUNIT_CASE(pvsched_runner_policy_boost_feedback_test),
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
