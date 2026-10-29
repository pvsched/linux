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
static struct pvsched_policy_entry *pvsched_default_entry;

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
		if (pvsched_policy_version(entry) == version &&
		    !strncmp(pvsched_policy_name(entry), name, PVSCHED_NAME_MAX))
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

bool pvsched_policy_builtin(const struct pvsched_policy_ops *ops)
{
	return ops == &pvsched_default_policy_ops;
}

struct pvsched_policy_entry *pvsched_policy_default_entry(void)
{
	return pvsched_default_entry;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_default_entry);

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

int pvsched_policy_query(u32 index, struct pvsched_policy_info *info)
{
	struct pvsched_policy_entry *entry;

	guard(mutex)(&pvsched_policy_mutex);
	list_for_each_entry(entry, &pvsched_policies, node) {
		if (index--)
			continue;
		memcpy(info->name, pvsched_policy_name(entry), sizeof(info->name));
		info->version = pvsched_policy_version(entry);
		/* An external policy owns everything after the header. */
		info->protocol = entry->ops ? entry->ops->protocol :
				 PVSCHED_PROTOCOL_CUSTOM;
		info->params_size = pvsched_policy_params_size(entry);
		return 0;
	}
	return -ENOENT;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_query);

int pvsched_policy_pin(const char *name, u32 version,
		       struct pvsched_policy_entry **pinned)
{
	struct pvsched_policy_entry *entry;

	guard(mutex)(&pvsched_policy_mutex);
	entry = pvsched_policy_find_locked(name, version);
	if (!entry)
		return -ENOENT;
	/*
	 * A module still in its init can fail it and be freed whatever its
	 * reference count, so its policy is not selectable until init is done.
	 * A module on its way out is as good as unregistered.
	 */
	if ((entry->owner && module_is_coming(entry->owner)) ||
	    !try_module_get(entry->owner))
		return -ENOENT;
	pvsched_policy_entry_get(entry);
	*pinned = entry;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_pin);

void pvsched_policy_unpin(struct pvsched_policy_entry *entry)
{
	module_put(entry->owner);
	pvsched_policy_entry_put(entry);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_policy_unpin);

static bool pvsched_policy_ops_valid(const struct pvsched_policy_ops *ops)
{
	return pvsched_policy_name_valid(ops->name) &&
	       ops->params_size && ops->params_size <= PVSCHED_POLICY_PARAMS_MAX &&
	       ops->priv_size <= PVSCHED_POLICY_PRIV_MAX &&
	       ops->protocol == PVSCHED_PROTOCOL_DEFAULT &&
	       ops->capture_baseline && ops->map && ops->apply;
}

/* List a filled-in entry under its name and version, or -EEXIST. */
static int pvsched_policy_add(struct pvsched_policy_entry *entry)
{
	refcount_set(&entry->ref, 1);
	guard(mutex)(&pvsched_policy_mutex);
	if (pvsched_policy_find_locked(pvsched_policy_name(entry),
				       pvsched_policy_version(entry)))
		return -EEXIST;
	/* Set before listing: a session may pin the entry right after. */
	if (entry->ops)
		entry->ops->owner = entry->owner;
	else
		entry->ext_ops->owner = entry->owner;
	list_add_tail(&entry->node, &pvsched_policies);
	pr_info("registered %s host policy %s v%u\n",
		entry->kind == PVSCHED_POLICY_EXTERNAL ? "external" : "managed",
		pvsched_policy_name(entry), pvsched_policy_version(entry));
	return 0;
}

/* Unlist the entry registered with @ops and drop the registry's reference. */
static void pvsched_policy_remove(const void *ops)
{
	struct pvsched_policy_entry *entry, *found = NULL;

	scoped_guard(mutex, &pvsched_policy_mutex) {
		list_for_each_entry(entry, &pvsched_policies, node) {
			if (entry->ops == ops || entry->ext_ops == ops) {
				found = entry;
				break;
			}
		}
		if (!found)
			return;
		found->dead = true;
		list_del_init(&found->node);
	}
	pr_info("unregistered host policy %s v%u\n", pvsched_policy_name(found),
		pvsched_policy_version(found));
	pvsched_policy_entry_put(found);
}

int __pvsched_register_policy(struct pvsched_policy_ops *ops,
			      struct module *owner)
{
	struct pvsched_policy_entry *entry;
	int ret;

	if (!pvsched_policy_ops_valid(ops))
		return -EINVAL;
	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;
	entry->kind = PVSCHED_POLICY_MANAGED;
	entry->ops = ops;
	entry->owner = owner;
	ret = pvsched_policy_add(entry);
	if (ret)
		kfree(entry);
	return ret;
}
EXPORT_SYMBOL_GPL(__pvsched_register_policy);

void pvsched_unregister_policy(struct pvsched_policy_ops *ops)
{
	pvsched_policy_remove(ops);
}
EXPORT_SYMBOL_GPL(pvsched_unregister_policy);

static bool pvsched_external_ops_valid(const struct pvsched_external_ops *ops)
{
	return pvsched_policy_name_valid(ops->name) &&
	       (ops->attach || ops->detach || ops->run_enter ||
		ops->run_leave || ops->vmentry || ops->vmentry_cancel ||
		ops->vmexit_irqoff || ops->vmexit || ops->halt ||
		ops->unhalt || ops->inject);
}

int __pvsched_register_external_policy(struct pvsched_external_ops *ops,
				       struct module *owner)
{
	struct pvsched_policy_entry *entry;
	int ret;

	if (!pvsched_external_ops_valid(ops))
		return -EINVAL;
	entry = kzalloc_obj(*entry);
	if (!entry)
		return -ENOMEM;
	entry->kind = PVSCHED_POLICY_EXTERNAL;
	entry->ext_ops = ops;
	entry->owner = owner;
	ret = pvsched_policy_add(entry);
	if (ret)
		kfree(entry);
	return ret;
}
EXPORT_SYMBOL_GPL(__pvsched_register_external_policy);

void pvsched_unregister_external_policy(struct pvsched_external_ops *ops)
{
	pvsched_policy_remove(ops);
}
EXPORT_SYMBOL_GPL(pvsched_unregister_external_policy);

int pvsched_policy_init(void)
{
	int ret;

	ret = pvsched_register_policy(&pvsched_default_policy_ops);
	if (ret)
		return ret;
	pvsched_default_entry = pvsched_policy_lookup(PVSCHED_DEFAULT_POLICY_NAME,
						      PVSCHED_DEFAULT_POLICY_VERSION);
	return 0;
}

void pvsched_policy_exit(void)
{
	pvsched_unregister_policy(&pvsched_default_policy_ops);
	pvsched_policy_entry_put(pvsched_default_entry);
}
