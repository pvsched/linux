/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_POLICY_H
#define _VIRT_PVSCHED_POLICY_H

#include <linux/jump_label.h>
#include <linux/list.h>
#include <linux/pvsched_external.h>
#include <linux/pvsched_policy.h>
#include <linux/refcount.h>

enum pvsched_policy_kind {
	/* pvsched runs its engine with the policy's map() and apply(). */
	PVSCHED_POLICY_MANAGED,
	/* pvsched only calls the policy's ops at each event. */
	PVSCHED_POLICY_EXTERNAL,
};

/*
 * One registered policy.  The registry holds a reference while the entry is
 * listed, and each user of the ops holds another, so the entry outlives an
 * unregister for as long as someone still uses it.
 */
struct pvsched_policy_entry {
	struct list_head node;
	refcount_t ref;
	enum pvsched_policy_kind kind;
	/* The ops of its kind; the other pointer is NULL. */
	struct pvsched_policy_ops *ops;
	struct pvsched_external_ops *ext_ops;
	struct module *owner;
	/*
	 * Unregistered: unlisted, so no longer found by name.  The registry
	 * itself goes by the list; the flag tells a holder of a reference that
	 * its policy is gone.
	 */
	bool dead;
};

/* What a guest negotiates for, and the registry looks up, for either kind. */
static inline const char *
pvsched_policy_name(const struct pvsched_policy_entry *entry)
{
	return entry->ops ? entry->ops->name : entry->ext_ops->name;
}

static inline u32
pvsched_policy_version(const struct pvsched_policy_entry *entry)
{
	return entry->ops ? entry->ops->version : entry->ext_ops->version;
}

/* The runner baseline's size; an external policy keeps none. */
static inline u32
pvsched_policy_params_size(const struct pvsched_policy_entry *entry)
{
	return entry->ops ? entry->ops->params_size : 0;
}

/*
 * Enabled while any external policy entry exists, registered or still
 * pinned by a session: only then may an injection into a nested target, or
 * one in an unsupported mode, find an attachment that wants it.
 */
DECLARE_STATIC_KEY_FALSE(pvsched_external_policies);

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

/* The built-in default, whose failures are kernel bugs worth a WARN. */
bool pvsched_policy_builtin(const struct pvsched_policy_ops *ops);
/* The registry entry of the built-in default, alive while pvsched is. */
struct pvsched_policy_entry *pvsched_policy_default_entry(void);

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
