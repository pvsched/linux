/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Paravirt scheduling - host policy interface.
 *
 * Defines struct pvsched_policy_ops, the set of callbacks a policy registers
 * with the pvsched host framework. The framework -- a module that depends on
 * KVM -- dispatches a registered vCPU's scheduling events to its policy's
 * callbacks and keeps all per-task state itself.
 */
#ifndef _LINUX_PVSCHED_H
#define _LINUX_PVSCHED_H

#include <linux/errno.h>
#include <linux/list.h>
#include <linux/types.h>

#include <uapi/linux/pvsched.h>

struct kvm_vcpu;
struct module;

/*
 * A pvsched policy: a named set of callbacks the framework invokes on a
 * registered vCPU's scheduling events.
 *
 * @name/@abi_version/@version form the identity a guest matches against (all
 * three must match to wire a vCPU to this policy). @shm passed to a callback is
 * the kernel mapping of that vCPU's pinned shared page (see
 * uapi/linux/pvsched.h), or NULL if none is bound; the pin keeps it usable
 * from any callback context for the binding's lifetime.
 */
struct pvsched_policy_ops {
	char		name[PVSCHED_NAME_MAX];
	__u32		abi_version;	/* PVSCHED_ABI_VERSION this policy speaks */
	__u32		version;	/* the policy's own version */
	struct module	*owner;

	void (*vmentry)(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm);
	void (*vmexit)(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm);
	void (*halt)(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm);
	void (*inject)(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm);

	struct list_head list;		/* framework registry linkage */
};

#if IS_ENABLED(CONFIG_PARAVIRT_SCHED_HOST)

int pvsched_register_policy(struct pvsched_policy_ops *ops);
void pvsched_unregister_policy(struct pvsched_policy_ops *ops);

/*
 * Set a vCPU thread's scheduling parameters (@pid is resolved in the caller's
 * pid namespace). Used by policy modules (BPF policy calls it as a kfunc).
 * Returns 0 if applied, 1 if the boost was refused (the task is throttled by
 * the boost cap), or a negative error.
 */
int pvsched_set_params(s32 pid, u32 policy, s32 nice, u32 rt_prio);

#else /* !CONFIG_PARAVIRT_SCHED_HOST */

static inline int pvsched_register_policy(struct pvsched_policy_ops *ops)
{
	return -EOPNOTSUPP;
}

static inline void pvsched_unregister_policy(struct pvsched_policy_ops *ops) { }

static inline int pvsched_set_params(s32 pid, u32 policy, s32 nice, u32 rt_prio)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_PARAVIRT_SCHED_HOST */

#endif /* _LINUX_PVSCHED_H */
