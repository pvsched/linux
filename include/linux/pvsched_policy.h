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

#include <linux/types.h>
#include <uapi/linux/pvsched.h>

struct module;

#define PVSCHED_POLICY_PARAMS_MAX	256

/**
 * struct pvsched_policy_ops - a host policy
 * @name: nonempty, NUL-terminated and zero-padded
 * @version: with @name, what the guest negotiates for
 * @protocol: guest-area protocol; only PVSCHED_PROTOCOL_DEFAULT for now
 * @params_size: size of the parameters, 1..PVSCHED_POLICY_PARAMS_MAX
 * @owner: set by pvsched_register_policy()
 */
struct pvsched_policy_ops {
	char name[PVSCHED_NAME_MAX];
	u32 version;
	u32 protocol;
	u32 params_size;
	struct module *owner;
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
