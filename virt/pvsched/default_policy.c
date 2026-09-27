// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <uapi/linux/sched/types.h>
#include <kunit/visibility.h>
#include <asm/byteorder.h>

#include "default_policy.h"

/*
 * The default policy's configuration: fixed at module load (or on the
 * kernel command line when built in) and validated once at initialization.
 */
static unsigned int cs_rt_prio = 60;
module_param(cs_rt_prio, uint, 0444);
MODULE_PARM_DESC(cs_rt_prio, "SCHED_FIFO priority for guest critical sections");

static unsigned int deadline_rt_prio = 50;
module_param(deadline_rt_prio, uint, 0444);
MODULE_PARM_DESC(deadline_rt_prio, "SCHED_FIFO priority for guest deadline tasks");

static unsigned int guest_rt_cap = 50;
module_param(guest_rt_cap, uint, 0444);
MODULE_PARM_DESC(guest_rt_cap, "Highest SCHED_FIFO priority for guest RT tasks");

struct pvsched_default_policy_config pvsched_default_policy_get_config(void)
{
	return (struct pvsched_default_policy_config) {
		.cs_rt_prio = cs_rt_prio,
		.deadline_rt_prio = deadline_rt_prio,
		.guest_rt_cap = guest_rt_cap,
	};
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_default_policy_get_config);

bool
pvsched_default_policy_config_valid(const struct pvsched_default_policy_config *config)
{
	if (!config->cs_rt_prio || config->cs_rt_prio >= MAX_RT_PRIO)
		return false;
	if (!config->deadline_rt_prio ||
	    config->deadline_rt_prio >= MAX_RT_PRIO)
		return false;
	if (!config->guest_rt_cap || config->guest_rt_cap >= MAX_RT_PRIO)
		return false;

	return config->cs_rt_prio > config->deadline_rt_prio &&
	       config->cs_rt_prio > config->guest_rt_cap;
}

static void
pvsched_default_policy_map(const struct pvsched_default_policy_config *config,
			   const struct pvsched_prio_desc *guest,
			   struct pvsched_prio_desc *host)
{
	*host = (struct pvsched_prio_desc) {
		.sched_policy = SCHED_NORMAL,
		.nice = guest->nice,
	};

	switch (guest->sched_policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		host->sched_policy = SCHED_FIFO;
		host->nice = 0;
		host->rt_prio = min_t(unsigned int, guest->rt_prio, config->guest_rt_cap);
		break;
	case SCHED_IDLE:
		host->sched_policy = SCHED_IDLE;
		host->nice = 0;
		break;
	case SCHED_DEADLINE:
		host->sched_policy = SCHED_FIFO;
		host->nice = 0;
		host->rt_prio = config->deadline_rt_prio;
		break;
	}
}

/* Lower values have higher priority; IDLE is a comparison-only sentinel. */
static int pvsched_default_policy_prio(const struct pvsched_prio_desc *prio)
{
	if (prio->sched_policy == SCHED_FIFO)
		return MAX_RT_PRIO - 1 - prio->rt_prio;
	if (prio->sched_policy == SCHED_IDLE)
		return MAX_PRIO;
	return NICE_TO_PRIO(prio->nice);
}

static bool
pvsched_above_baseline(const struct pvsched_prio_desc *prio, int baseline_nice)
{
	return prio->sched_policy == SCHED_FIFO ||
	       (prio->sched_policy == SCHED_NORMAL &&
		prio->nice < baseline_nice);
}

static bool
pvsched_below_baseline(const struct pvsched_prio_desc *prio, int baseline_nice)
{
	return prio->sched_policy == SCHED_IDLE ||
	       (prio->sched_policy == SCHED_NORMAL &&
		prio->nice > baseline_nice);
}

int
pvsched_default_policy_select(const struct pvsched_default_policy_config *config,
			      int baseline_nice,
			      const struct pvsched_default_guest_area *guest,
			      bool cs_throttled, bool generic_throttled,
			      struct pvsched_default_policy_result *result)
{
	struct pvsched_default_policy_result selected;
	struct pvsched_prio_desc pending;
	u64 cs_state;

	/* Reject malformed input before throttle state can affect selection. */
	if (!pvsched_default_policy_config_valid(config) ||
	    baseline_nice < MIN_NICE || baseline_nice > MAX_NICE)
		return -EINVAL;

	selected = (struct pvsched_default_policy_result) {
		.prio = {
			.sched_policy = SCHED_NORMAL,
			.nice = baseline_nice,
		},
		.source = PVSCHED_DEFAULT_POLICY_BASELINE,
	};
	cs_state = le64_to_cpu(guest->cs_state);
	if (generic_throttled) {
		struct pvsched_prio_desc current_prio;

		/*
		 * Exhaustion is an elevation ceiling.  Clamp each task candidate
		 * to baseline, then retain the normal rule that pending may only
		 * replace current when it is strictly more urgent.
		 */
		pvsched_default_policy_map(config,
					   &guest->task_intent.current_task,
					   &current_prio);
		if (pvsched_above_baseline(&current_prio, baseline_nice))
			current_prio = selected.prio;
		else if (pvsched_below_baseline(&current_prio, baseline_nice))
			selected.source = PVSCHED_DEFAULT_POLICY_TASK;
		selected.prio = current_prio;

		if (guest->task_intent.flags &
		    PVSCHED_INTENT_FLAG_PENDING_VALID) {
			pvsched_default_policy_map(config,
						   &guest->task_intent.pending_task,
						   &pending);
			if (pvsched_above_baseline(&pending, baseline_nice))
				pending = (struct pvsched_prio_desc) {
					.sched_policy = SCHED_NORMAL,
					.nice = baseline_nice,
				};
			if (pvsched_default_policy_prio(&pending) <
			    pvsched_default_policy_prio(&selected.prio)) {
				selected.prio = pending;
				selected.source = PVSCHED_DEFAULT_POLICY_TASK;
			}
		}
		goto out;
	}

	if (cs_state && !cs_throttled) {
		selected.prio.sched_policy = SCHED_FIFO;
		selected.prio.nice = 0;
		selected.prio.rt_prio = config->cs_rt_prio;
		selected.source = PVSCHED_DEFAULT_POLICY_CS;
		goto out;
	}

	pvsched_default_policy_map(config, &guest->task_intent.current_task,
				   &selected.prio);
	selected.source = PVSCHED_DEFAULT_POLICY_TASK;
	if (guest->task_intent.flags & PVSCHED_INTENT_FLAG_PENDING_VALID) {
		pvsched_default_policy_map(config,
					   &guest->task_intent.pending_task,
					   &pending);
		if (pvsched_default_policy_prio(&pending) <
		    pvsched_default_policy_prio(&selected.prio))
			selected.prio = pending;
	}

out:
	*result = selected;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_default_policy_select);

/*
 * Charge by what the tuple is for: validation makes cs_rt_prio strictly
 * higher than every other priority, so only CS and host reasons select it.
 */
static enum pvsched_boost_class
pvsched_default_policy_class(const struct pvsched_default_policy_config *config,
			     int baseline_nice,
			     const struct pvsched_prio_desc *prio)
{
	if (prio->sched_policy == SCHED_FIFO &&
	    prio->rt_prio == config->cs_rt_prio)
		return PVSCHED_CLASS_CS;
	if (pvsched_above_baseline(prio, baseline_nice))
		return PVSCHED_CLASS_TASK;
	return PVSCHED_CLASS_BASELINE;
}

static int pvsched_default_ops_capture(struct pvsched_policy_ctx *ctx,
				       void *baseline)
{
	struct pvsched_default_params *params = baseline;
	struct pvsched_default_priv *priv = ctx->priv;
	struct sched_task_state state;

	sched_get_task_state(ctx->task, &state);
	if (state.policy != SCHED_NORMAL || state.reset_on_fork ||
	    state.scx_active)
		return -EOPNOTSUPP;
	params->prio = (struct pvsched_prio_desc) {
		.sched_policy = state.policy,
		.nice = state.nice,
		.rt_prio = state.rt_priority,
	};
	params->custom_slice = state.custom_slice;
	params->slice_ns = state.slice_ns;
	params->timer_slack_ns = state.timer_slack_ns;
	priv->expected_nice = state.nice;
	return 0;
}

/*
 * An idle guest is heading to a halt, where the HLT exit and HALT keep it
 * boosted until it blocks.  Deboosting it on the way, at another exit,
 * lets a host task preempt it while its guest timer runs on a timer the
 * host cannot see, so it would wait a whole slice.  The same applies just
 * after a wake: the guest's idle task handles the wakeup before it switches
 * to the woken task, and a deboost there lets a host task run first.  So
 * when the framework offers the hold, keep an applied FIFO boost rather
 * than lower it.
 */
static bool pvsched_default_ops_hold(const struct pvsched_map_input *in,
				     const struct pvsched_prio_desc *applied,
				     const struct pvsched_prio_desc *target)
{
	return in->hold_offered && applied->sched_policy == SCHED_FIFO &&
	       !(target->sched_policy == SCHED_FIFO &&
		 target->rt_prio >= applied->rt_prio);
}

static int pvsched_default_ops_map(struct pvsched_policy_ctx *ctx,
				   const struct pvsched_map_input *in,
				   void *out, enum pvsched_boost_class *class,
				   u32 *out_flags)
{
	struct pvsched_default_policy_config config =
		pvsched_default_policy_get_config();
	const struct pvsched_default_params *baseline = ctx->baseline;
	const struct pvsched_default_params *applied = ctx->applied;
	struct pvsched_default_params *params = out;
	struct pvsched_default_policy_result result;
	int ret;

	ret = pvsched_default_policy_select(&config, baseline->prio.nice,
					    in->guest, in->cs_throttled,
					    in->generic_throttled, &result);
	if (ret)
		return ret;
	*params = *baseline;
	params->prio = result.prio;
	if ((in->reasons || in->ticket_live) &&
	    !in->cs_throttled && !in->generic_throttled)
		params->prio = (struct pvsched_prio_desc) {
			.sched_policy = SCHED_FIFO,
			.rt_prio = config.cs_rt_prio,
		};
	if (pvsched_default_ops_hold(in, &applied->prio, &params->prio)) {
		*params = *applied;
		*out_flags |= PVSCHED_MAP_HELD;
	}
	*class = pvsched_default_policy_class(&config, baseline->prio.nice,
					      &params->prio);
	return 0;
}

static int pvsched_default_ops_apply(struct pvsched_policy_ctx *ctx,
				     const void *data)
{
	const struct pvsched_default_params *params = data;
	const struct pvsched_default_params *applied = ctx->applied;
	struct pvsched_default_priv *priv = ctx->priv;
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = params->prio.sched_policy,
		.sched_nice = params->prio.nice,
		.sched_priority = params->prio.rt_prio,
	};
	int ret;

	/*
	 * The captured slice was already clamped by __setparam_fair(); sending it
	 * back through the setter therefore restores that exact explicit value.
	 */
	if (params->prio.sched_policy == SCHED_NORMAL && params->custom_slice)
		attr.sched_runtime = params->slice_ns;
	ret = sched_setattr_nocheck_nopi(ctx->task, &attr);
	if (ret)
		return ret;
	/* Only RT policy entry clears task timer slack, so restore after FIFO. */
	if (applied->prio.sched_policy == SCHED_FIFO &&
	    (params->prio.sched_policy == SCHED_NORMAL ||
	     params->prio.sched_policy == SCHED_IDLE))
		sched_set_task_timer_slack(ctx->task, params->timer_slack_ns);
	/* FIFO and IDLE retain the latent fair nice rather than changing it. */
	if (params->prio.sched_policy == SCHED_NORMAL)
		priv->expected_nice = params->prio.nice;
	return 0;
}

static bool pvsched_default_ops_owned(struct pvsched_policy_ctx *ctx)
{
	const struct pvsched_default_params *baseline = ctx->baseline;
	const struct pvsched_default_params *applied = ctx->applied;
	const struct pvsched_default_priv *priv = ctx->priv;
	struct sched_task_state state;

	sched_get_task_state(ctx->task, &state);
	/*
	 * FIFO and IDLE leave fair nice latent, so compare the value last applied
	 * under NORMAL.  A default fair slice is dynamic and is compared only when
	 * ATTACH captured an explicit slice.  These checks make administrator
	 * setpriority(), chrt, or slice changes establish an external owner.
	 */
	return state.policy == applied->prio.sched_policy &&
	       state.nice == priv->expected_nice &&
	       state.rt_priority == applied->prio.rt_prio &&
	       state.custom_slice == baseline->custom_slice &&
	       (!state.custom_slice || state.slice_ns == baseline->slice_ns) &&
	       !state.reset_on_fork && !state.scx_active;
}

struct pvsched_policy_ops pvsched_default_policy_ops = {
	.name = PVSCHED_DEFAULT_POLICY_NAME,
	.version = PVSCHED_DEFAULT_POLICY_VERSION,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = sizeof(struct pvsched_default_params),
	.priv_size = sizeof(struct pvsched_default_priv),
	.capture_baseline = pvsched_default_ops_capture,
	.map = pvsched_default_ops_map,
	.apply = pvsched_default_ops_apply,
	.owned = pvsched_default_ops_owned,
};
EXPORT_SYMBOL_IF_KUNIT(pvsched_default_policy_ops);
