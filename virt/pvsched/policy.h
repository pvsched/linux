/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_POLICY_H
#define _VIRT_PVSCHED_POLICY_H

#include <linux/list.h>
#include <linux/pvsched_policy.h>
#include <linux/refcount.h>

/*
 * One registered policy.  The registry holds a reference while the entry is
 * listed, and each user of the ops holds another, so the entry outlives an
 * unregister for as long as someone still uses it.
 */
struct pvsched_policy_entry {
	struct list_head node;
	refcount_t ref;
	struct pvsched_policy_ops *ops;
	struct module *owner;
	/*
	 * Unregistered: unlisted, so no longer found by name.  The registry
	 * itself goes by the list; the flag tells a holder of a reference that
	 * its policy is gone.
	 */
	bool dead;
};

/* Nonempty, NUL-terminated, and zero-padded after the terminator. */
bool pvsched_policy_name_valid(const char *name);

/* Find a live policy by name and version, with a reference, or NULL. */
struct pvsched_policy_entry *pvsched_policy_lookup(const char *name,
						   u32 version);
void pvsched_policy_entry_get(struct pvsched_policy_entry *entry);
void pvsched_policy_entry_put(struct pvsched_policy_entry *entry);

/* Fill in the policy at @index in registration order, or -ENOENT. */
int pvsched_policy_query(u32 index, struct pvsched_policy_info *info);

/*
 * Pin a live policy for a session: a reference on its entry and on its
 * module, so that neither goes away while the session may still call it,
 * even after it is unregistered.  -ENOENT if it is not registered.
 */
int pvsched_policy_pin(const char *name, u32 version,
		       struct pvsched_policy_entry **pinned);
void pvsched_policy_unpin(struct pvsched_policy_entry *entry);

/*
 * Whether a private snapshot of the default guest area is well formed:
 * valid task tuples, a zero pending tuple unless flagged, and no reserved
 * flag, CS or area bits.  pvsched checks every fresh snapshot before a
 * policy sees it.
 */
bool pvsched_default_guest_valid(const struct pvsched_default_guest_area *guest);

/* Register and unregister the built-in default policy. */
int pvsched_policy_init(void);
void pvsched_policy_exit(void);

#endif /* _VIRT_PVSCHED_POLICY_H */
