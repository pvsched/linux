// SPDX-License-Identifier: GPL-2.0
/*
 * A sample pvsched host policy that boosts through fair nice alone.
 *
 * A guest critical section, a halt or an interrupt handoff gives the
 * vCPU thread nice -20 (class CS).  Otherwise the guest's most urgent
 * published task maps to a nice value: a fair task's own nice, -10 for
 * an RT or deadline task and 19 for an idle one; a value below the
 * baseline's is a TASK boost.  The baseline is the thread's captured
 * nice, slice and timer slack.  The policy never uses SCHED_FIFO.
 */
#include <linux/module.h>
#include <linux/pvsched_policy.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <uapi/linux/sched/types.h>

struct sample_nice_params {
	s8 nice;
	u8 custom_slice;
	u8 reserved[6];
	u64 slice_ns;
	u64 timer_slack_ns;
};
static_assert(sizeof(struct sample_nice_params) == 24);

#define SAMPLE_NICE_RT		-10

static int sample_nice_capture(struct pvsched_policy_ctx *ctx, void *baseline)
{
	struct sample_nice_params *params = baseline;
	struct sched_task_state state;

	sched_get_task_state(ctx->task, &state);
	if (state.policy != SCHED_NORMAL || state.reset_on_fork ||
	    state.scx_active)
		return -EOPNOTSUPP;
	params->nice = state.nice;
	params->custom_slice = state.custom_slice;
	params->slice_ns = state.slice_ns;
	params->timer_slack_ns = state.timer_slack_ns;
	return 0;
}

static int sample_nice_of(const struct pvsched_prio_desc *task)
{
	switch (task->sched_policy) {
	case SCHED_NORMAL:
	case SCHED_BATCH:
		return task->nice;
	case SCHED_IDLE:
		return MAX_NICE;
	default:
		/* FIFO, RR and DEADLINE: the framework validated the rest. */
		return SAMPLE_NICE_RT;
	}
}

static int sample_nice_map(struct pvsched_policy_ctx *ctx,
			   const struct pvsched_map_input *in, void *out,
			   enum pvsched_boost_class *class, u32 *out_flags)
{
	const struct pvsched_default_guest_area *guest = in->guest;
	const struct sample_nice_params *baseline = ctx->baseline;
	const struct sample_nice_params *applied = ctx->applied;
	struct sample_nice_params *params = out;
	bool throttled = in->cs_throttled || in->generic_throttled;
	int nice;

	*params = *baseline;
	if (!throttled &&
	    (le64_to_cpu(guest->cs_state) || in->reasons || in->ticket_live)) {
		params->nice = MIN_NICE;
		*class = PVSCHED_CLASS_CS;
		return 0;
	}

	nice = sample_nice_of(&guest->task_intent.current_task);
	if (guest->task_intent.flags & PVSCHED_INTENT_FLAG_PENDING_VALID)
		nice = min(nice, sample_nice_of(&guest->task_intent.pending_task));
	/* An exhausted budget allows no elevation, only a deboost. */
	if (in->generic_throttled)
		nice = max_t(int, nice, baseline->nice);
	/* Keep a boost while the framework lets an idle guest keep it. */
	if (in->hold_offered && applied->nice < baseline->nice &&
	    applied->nice < nice) {
		*params = *applied;
		*out_flags |= PVSCHED_MAP_HELD;
		return 0;
	}
	params->nice = nice;
	*class = nice < baseline->nice ? PVSCHED_CLASS_TASK :
					 PVSCHED_CLASS_BASELINE;
	return 0;
}

static int sample_nice_apply(struct pvsched_policy_ctx *ctx, const void *data)
{
	const struct sample_nice_params *params = data;
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_NORMAL,
		.sched_nice = params->nice,
	};

	/* Keep an explicit slice the thread had. */
	if (params->custom_slice)
		attr.sched_runtime = params->slice_ns;
	return sched_setattr_nocheck_nopi(ctx->task, &attr);
}

static bool sample_nice_owned(struct pvsched_policy_ctx *ctx)
{
	const struct sample_nice_params *applied = ctx->applied;
	struct sched_task_state state;

	sched_get_task_state(ctx->task, &state);
	return state.policy == SCHED_NORMAL && state.nice == applied->nice &&
	       state.custom_slice == applied->custom_slice &&
	       (!state.custom_slice || state.slice_ns == applied->slice_ns) &&
	       !state.reset_on_fork && !state.scx_active;
}

static struct pvsched_policy_ops sample_nice_ops = {
	.name = "sample-nice",
	.version = 1,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = sizeof(struct sample_nice_params),
	.capture_baseline = sample_nice_capture,
	.map = sample_nice_map,
	.apply = sample_nice_apply,
	.owned = sample_nice_owned,
};

static int __init sample_nice_init(void)
{
	return pvsched_register_policy(&sample_nice_ops);
}

static void __exit sample_nice_exit(void)
{
	pvsched_unregister_policy(&sample_nice_ops);
}

module_init(sample_nice_init);
module_exit(sample_nice_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pvsched sample host policy that boosts with fair nice");
