/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_DEFAULT_POLICY_H
#define _VIRT_PVSCHED_DEFAULT_POLICY_H

#include <linux/types.h>
#include <uapi/linux/pvsched.h>

struct pvsched_default_policy_config {
	/* 1..99; CS must be strictly greater than deadline and the guest cap. */
	unsigned int cs_rt_prio;
	unsigned int deadline_rt_prio;
	unsigned int guest_rt_cap;
};

enum pvsched_default_policy_source {
	PVSCHED_DEFAULT_POLICY_BASELINE,
	/* Request provenance; it does not imply generic-budget charging. */
	PVSCHED_DEFAULT_POLICY_TASK,
	PVSCHED_DEFAULT_POLICY_CS,
};

struct pvsched_default_policy_result {
	struct pvsched_prio_desc prio;
	enum pvsched_default_policy_source source;
};

/**
 * pvsched_default_policy_select() - select the built-in scheduling request
 * @config: host policy configuration
 * @baseline_nice: per-vCPU SCHED_NORMAL nice (-20..19) captured at registration
 * @guest: stable private 64-byte guest-area snapshot, not shared memory
 * @cs_throttled: whether the critical-section budget is throttled
 * @generic_throttled: whether the generic budget is throttled
 * @result: selected request, not applied scheduling state
 *
 * The trusted kernel caller supplies valid, nonoverlapping pointers.  Generic
 * throttling clamps current and pending requests above @baseline_nice to the
 * baseline, then permits pending to replace current only when it is strictly
 * more urgent.  This preserves valid positive-nice and SCHED_IDLE deboost
 * intent without letting a stale pending hint lower the clamped current
 * request.  On success @result receives the selected request.  On failure it
 * is unchanged.
 *
 * Return: 0 on success or -EINVAL for invalid configuration, baseline or guest
 * snapshot.
 */
int pvsched_default_policy_select(const struct pvsched_default_policy_config *config,
				  int baseline_nice,
				  const struct pvsched_default_guest_area *guest,
				  bool cs_throttled, bool generic_throttled,
				  struct pvsched_default_policy_result *result);

#endif /* _VIRT_PVSCHED_DEFAULT_POLICY_H */
