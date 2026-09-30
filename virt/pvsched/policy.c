// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <kunit/visibility.h>
#include <asm/byteorder.h>

#include "default_policy.h"
#include "policy.h"

/* Registered policies in registration order; the default comes first. */
static DEFINE_MUTEX(pvsched_policy_mutex);
static LIST_HEAD(pvsched_policies);

bool pvsched_policy_name_valid(const char *name)
{
	size_t len = strnlen(name, PVSCHED_NAME_MAX);

	if (!len || len == PVSCHED_NAME_MAX)
		return false;
	return !memchr_inv(name + len, 0, PVSCHED_NAME_MAX - len);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_name_valid);

static struct pvsched_policy_entry *
pvsched_policy_find_locked(const char *name, u32 version)
{
	struct pvsched_policy_entry *entry;

	lockdep_assert_held(&pvsched_policy_mutex);
	list_for_each_entry(entry, &pvsched_policies, node) {
		if (entry->ops->version == version &&
		    !strncmp(entry->ops->name, name, PVSCHED_NAME_MAX))
			return entry;
	}
	return NULL;
}

void pvsched_policy_entry_get(struct pvsched_policy_entry *entry)
{
	refcount_inc(&entry->ref);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_entry_get);

void pvsched_policy_entry_put(struct pvsched_policy_entry *entry)
{
	/* The registry's own reference is gone, so the entry is unlisted. */
	if (refcount_dec_and_test(&entry->ref))
		kfree(entry);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_entry_put);

struct pvsched_policy_entry *pvsched_policy_lookup(const char *name,
						   u32 version)
{
	struct pvsched_policy_entry *entry;

	guard(mutex)(&pvsched_policy_mutex);
	entry = pvsched_policy_find_locked(name, version);
	if (entry)
		pvsched_policy_entry_get(entry);
	return entry;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_lookup);

static bool pvsched_prio_desc_valid(const struct pvsched_prio_desc *prio)
{
	switch (prio->sched_policy) {
	case SCHED_NORMAL:
	case SCHED_BATCH:
		return prio->nice >= MIN_NICE && prio->nice <= MAX_NICE &&
		       !prio->rt_prio;
	case SCHED_FIFO:
	case SCHED_RR:
		return !prio->nice && prio->rt_prio &&
		       prio->rt_prio < MAX_RT_PRIO;
	case SCHED_IDLE:
	case SCHED_DEADLINE:
		return !prio->nice && !prio->rt_prio;
	default:
		return false;
	}
}

bool pvsched_default_guest_valid(const struct pvsched_default_guest_area *guest)
{
	const union pvsched_task_intent *intent = &guest->task_intent;

	if (!pvsched_prio_desc_valid(&intent->current_task))
		return false;
	if (intent->flags & ~PVSCHED_INTENT_FLAGS_VALID ||
	    intent->reserved)
		return false;
	if (intent->flags & PVSCHED_INTENT_FLAG_PENDING_VALID) {
		if (!pvsched_prio_desc_valid(&intent->pending_task))
			return false;
	} else if (intent->pending_task.sched_policy ||
		   intent->pending_task.nice || intent->pending_task.rt_prio) {
		return false;
	}
	if (le64_to_cpu(guest->cs_state) & PVSCHED_CS_RESERVED_MASK)
		return false;
	return !memchr_inv(guest->reserved, 0, sizeof(guest->reserved));
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_default_guest_valid);

static bool pvsched_policy_ops_valid(const struct pvsched_policy_ops *ops)
{
	return pvsched_policy_name_valid(ops->name) &&
	       ops->params_size && ops->params_size <= PVSCHED_POLICY_PARAMS_MAX &&
	       ops->protocol == PVSCHED_PROTOCOL_DEFAULT;
}

int __pvsched_register_policy(struct pvsched_policy_ops *ops,
			      struct module *owner)
{
	struct pvsched_policy_entry *entry;

	if (!pvsched_policy_ops_valid(ops))
		return -EINVAL;
	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;
	refcount_set(&entry->ref, 1);
	entry->ops = ops;
	entry->owner = owner;

	guard(mutex)(&pvsched_policy_mutex);
	if (pvsched_policy_find_locked(ops->name, ops->version)) {
		kfree(entry);
		return -EEXIST;
	}
	ops->owner = owner;
	list_add_tail(&entry->node, &pvsched_policies);
	pr_info("registered host policy %s v%u\n", ops->name, ops->version);
	return 0;
}
EXPORT_SYMBOL_GPL(__pvsched_register_policy);

void pvsched_unregister_policy(struct pvsched_policy_ops *ops)
{
	struct pvsched_policy_entry *entry, *found = NULL;

	scoped_guard(mutex, &pvsched_policy_mutex) {
		list_for_each_entry(entry, &pvsched_policies, node) {
			if (entry->ops == ops) {
				found = entry;
				break;
			}
		}
		if (!found)
			return;
		found->dead = true;
		list_del_init(&found->node);
	}
	pr_info("unregistered host policy %s v%u\n", ops->name, ops->version);
	pvsched_policy_entry_put(found);
}
EXPORT_SYMBOL_GPL(pvsched_unregister_policy);

int pvsched_policy_init(void)
{
	return pvsched_register_policy(&pvsched_default_policy_ops);
}

void pvsched_policy_exit(void)
{
	pvsched_unregister_policy(&pvsched_default_policy_ops);
}
