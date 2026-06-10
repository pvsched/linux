// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Google */

/*
 * Paravirt scheduling - host framework module.
 *
 * The framework is the central manager for pvsched policies: policy modules
 * (kernel modules or, later, BPF struct_ops) register a named set of callbacks
 * here, and the framework matches a guest's requested policy (by name and
 * version) against them. Per-vCPU registration and event dispatch are added by
 * later changes.
 */

#define pr_fmt(fmt) "pvsched: " fmt

#include <linux/build_bug.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pvsched.h>
#include <linux/seq_file.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include <uapi/linux/pvsched.h>

static LIST_HEAD(pvsched_policies);
static DEFINE_MUTEX(pvsched_mutex);

int pvsched_register_policy(struct pvsched_policy_ops *ops)
{
	struct pvsched_policy_ops *p;

	if (!ops->name[0] || !ops->vmentry || !ops->vmexit ||
	    !ops->halt || !ops->inject)
		return -EINVAL;
	if (ops->abi_version != PVSCHED_ABI_VERSION)
		return -EINVAL;

	mutex_lock(&pvsched_mutex);
	list_for_each_entry(p, &pvsched_policies, list) {
		if (!strncmp(p->name, ops->name, PVSCHED_NAME_MAX) &&
		    p->version == ops->version) {
			mutex_unlock(&pvsched_mutex);
			return -EEXIST;
		}
	}
	list_add_tail(&ops->list, &pvsched_policies);
	mutex_unlock(&pvsched_mutex);

	pr_info("registered policy '%s' (abi %u, version %u)\n",
		ops->name, ops->abi_version, ops->version);
	return 0;
}
EXPORT_SYMBOL_GPL(pvsched_register_policy);

void pvsched_unregister_policy(struct pvsched_policy_ops *ops)
{
	mutex_lock(&pvsched_mutex);
	list_del_init(&ops->list);
	mutex_unlock(&pvsched_mutex);

	pr_info("unregistered policy '%s'\n", ops->name);
}
EXPORT_SYMBOL_GPL(pvsched_unregister_policy);

/* debugfs + module lifecycle. */

static int pvsched_policies_show(struct seq_file *m, void *v)
{
	struct pvsched_policy_ops *p;

	mutex_lock(&pvsched_mutex);
	list_for_each_entry(p, &pvsched_policies, list)
		seq_printf(m, "%s abi=%u version=%u\n",
			   p->name, p->abi_version, p->version);
	mutex_unlock(&pvsched_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pvsched_policies);

static struct dentry *pvsched_debugfs;

static int __init pvsched_init(void)
{
	/* The shared-memory layout is a guest/host/VMM ABI; keep it fixed. */
	BUILD_BUG_ON(sizeof(struct pvsched_header) != 64);
	BUILD_BUG_ON(sizeof(struct pvsched_guest_area) != 32);
	BUILD_BUG_ON(sizeof(struct pvsched_host_area) != 32);
	BUILD_BUG_ON(sizeof(union pvsched_vcpu_page) != PVSCHED_VCPU_STRIDE);
	BUILD_BUG_ON(offsetof(union pvsched_vcpu_page, header) != 0);
	BUILD_BUG_ON(offsetof(union pvsched_vcpu_page, guest_area) != 64);
	BUILD_BUG_ON(offsetof(union pvsched_vcpu_page, host_area) != 96);

	pvsched_debugfs = debugfs_create_dir("pvsched", NULL);
	debugfs_create_file("policies", 0444, pvsched_debugfs, NULL,
			    &pvsched_policies_fops);

	pr_info("paravirt scheduling host framework (ABI v%d)\n",
		PVSCHED_ABI_VERSION);
	return 0;
}

static void __exit pvsched_exit(void)
{
	debugfs_remove_recursive(pvsched_debugfs);
}

module_init(pvsched_init);
module_exit(pvsched_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Paravirt scheduling host framework");
