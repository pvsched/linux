/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pvsched host policies.
 *
 * A host policy decides how a vCPU thread is scheduled while its guest
 * publishes intent through pvsched.  pvsched keeps the shared page and its
 * negotiation, validation of the default guest area, the attachment
 * lifetime, the KVM hooks, the budgets and the host reasons.  A policy maps
 * that input to parameters of its own, opaque to pvsched, and applies them
 * with its own setter.  The built-in "default" policy is registered first,
 * and a module can register more.
 */
#ifndef _LINUX_PVSCHED_POLICY_H
#define _LINUX_PVSCHED_POLICY_H

#include <linux/build_bug.h>
#include <linux/types.h>
#include <uapi/linux/pvsched.h>

struct module;
struct task_struct;

#define PVSCHED_POLICY_PARAMS_MAX	256
#define PVSCHED_POLICY_PRIV_MAX		256

enum pvsched_reconcile_event {
	/* RUN_ENTER: IRQs enabled, preemptible, setter permitted. */
	PVSCHED_RECONCILE_RUN_ENTER,
	/* IRQ-on VMEXIT: preemption disabled, setter permitted. */
	PVSCHED_RECONCILE_VMEXIT,
	/* VMENTRY_CANCEL: IRQs enabled, preemption disabled, setter permitted. */
	PVSCHED_RECONCILE_CANCEL,
	/* RUN_LEAVE: IRQs enabled, preemptible, setter permitted. */
	PVSCHED_RECONCILE_RUN_LEAVE,
};

/*
 * What an applied value is for, which selects the budgets it drains.  A
 * class says what the elevation is for, not how strong it is; each policy
 * decides what TASK or CS means in its scheduler.
 */
enum pvsched_boost_class {
	/* Not elevated, or a deboost: both budgets refill. */
	PVSCHED_CLASS_BASELINE,
	/* Elevated for guest task intent: drains the generic budget. */
	PVSCHED_CLASS_TASK,
	/* Elevated for a guest critical section: drains both budgets. */
	PVSCHED_CLASS_CS,
};

/* The guest sees the applied class as the applied-state boost. */
static_assert((int)PVSCHED_CLASS_BASELINE == PVSCHED_BOOST_BASELINE);
static_assert((int)PVSCHED_CLASS_TASK == PVSCHED_BOOST_TASK);
static_assert((int)PVSCHED_CLASS_CS == PVSCHED_BOOST_CS);

/* One per attached vCPU, passed to every callback. */
struct pvsched_policy_ctx {
	/* The vCPU thread; a policy acts on this task and nothing else. */
	struct task_struct *task;
	/* ops->priv_size bytes, zeroed before capture_baseline(). */
	void *priv;
	/* ops->params_size bytes: the value captured at attach. */
	const void *baseline;
	/* ops->params_size bytes: the value applied last. */
	const void *applied;
};

struct pvsched_map_input {
	/* RUN_ENTER, VMEXIT or CANCEL. */
	enum pvsched_reconcile_event event;
	/* Validated private snapshot of the default guest area. */
	const struct pvsched_default_guest_area *guest;
	bool cs_throttled;
	bool generic_throttled;
};

/**
 * struct pvsched_policy_ops - a host policy
 * @name: nonempty, NUL-terminated and zero-padded
 * @version: with @name, what the guest negotiates for
 * @protocol: guest-area protocol; only PVSCHED_PROTOCOL_DEFAULT for now
 * @params_size: size of the parameters, 1..PVSCHED_POLICY_PARAMS_MAX
 * @priv_size: size of the per-vCPU private state, 0..PVSCHED_POLICY_PRIV_MAX
 * @owner: set by pvsched_register_policy()
 * @capture_baseline: record the task's current state as the restore target
 * @map: turn the input into parameters and their class
 * @apply: apply parameters to ctx->task
 * @owned: optional; whether ctx->task still has the value applied last
 *
 * @capture_baseline runs in process context and may sleep.  pvsched zeroes
 * the baseline buffer before it; a nonzero return refuses the attach.
 *
 * @map, @apply and @owned run with the runner's raw state lock held and
 * IRQs off, at run entry, the IRQ-on VM exit, a cancelled entry and the
 * close and restore paths; never at the late VM entry.  They must not
 * sleep, and @apply must not take a lock that orders before the task's
 * pi_lock or its runqueue lock.
 *
 * pvsched zeroes @map's output first, so equal values compare equal byte
 * for byte, and calls @apply only when the bytes differ from the applied
 * value.  @map must leave *out_flags zero.  pvsched trusts the class: an
 * elevated value labelled BASELINE escapes the budgets.  An error from @map
 * or @apply, or a class the budgets forbid, restores the baseline.
 *
 * Without @owned, pvsched cannot see an external owner such as an
 * administrator's chrt(1); the next @apply overrides it.
 */
struct pvsched_policy_ops {
	char name[PVSCHED_NAME_MAX];
	u32 version;
	u32 protocol;
	u32 params_size;
	u32 priv_size;
	struct module *owner;

	int (*capture_baseline)(struct pvsched_policy_ctx *ctx, void *baseline);

	int (*map)(struct pvsched_policy_ctx *ctx,
		   const struct pvsched_map_input *in, void *out,
		   enum pvsched_boost_class *class, u32 *out_flags);
	int (*apply)(struct pvsched_policy_ctx *ctx, const void *params);
	bool (*owned)(struct pvsched_policy_ctx *ctx);
};

/*
 * Register last in module init and unregister first in module exit.
 * Unregistering only unlists the policy: a session that selected it keeps
 * it, and a reference on its module, until the session closes, so the
 * module cannot be removed meanwhile.
 */
int __pvsched_register_policy(struct pvsched_policy_ops *ops,
			      struct module *owner);
#define pvsched_register_policy(ops) \
	__pvsched_register_policy(ops, THIS_MODULE)
void pvsched_unregister_policy(struct pvsched_policy_ops *ops);

#endif /* _LINUX_PVSCHED_POLICY_H */
