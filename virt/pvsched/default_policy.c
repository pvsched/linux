// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <kunit/visibility.h>
#include <asm/byteorder.h>

#include "default_policy.h"

static bool
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

struct pvsched_policy_ops pvsched_default_policy_ops = {
	.name = PVSCHED_DEFAULT_POLICY_NAME,
	.version = PVSCHED_DEFAULT_POLICY_VERSION,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = sizeof(struct pvsched_default_params),
};
EXPORT_SYMBOL_IF_KUNIT(pvsched_default_policy_ops);
