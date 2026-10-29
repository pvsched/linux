// SPDX-License-Identifier: GPL-2.0
/*
 * The "record" example external pvsched policy (see
 * <linux/pvsched_record.h>): at each slow VM exit it takes a copy of the
 * totals the guest side counted, and at VM entry it tells the guest which
 * totals it took.  It never changes any scheduling.
 *
 * /sys/kernel/debug/pvsched_record_host lists the last copy of each
 * attached vCPU by KVM vcpu_id, for vcpu_ids below RECORD_MAX_VCPUS.  It
 * is meant for one VM at a time: the vCPUs of two VMs share rows.
 */
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/pvsched_external.h>
#include <linux/pvsched_record.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define RECORD_MAX_VCPUS	64

/* Per-vCPU private state in v->priv, under the attachment's lock. */
struct record_priv {
	/* The totals copied last. */
	u64 switches;
	u64 irqs;
	/* Copied but not yet written back to the guest. */
	bool unpublished;
	/* detach holds a raw spinlock: free after a grace period. */
	struct rcu_head rcu;
};

/*
 * Each vCPU's last copy, for debugfs, which cannot reach v->priv.  Rows
 * outlive attachments and may tear.
 */
struct record_slot {
	/* The VMM process; nonzero once the slot is in use. */
	u64 tgid;
	u64 switches;
	u64 irqs;
	u64 last_pid;
	/* detach calls, which must match the attachments that ended. */
	u64 detaches;
};

static struct record_slot record_slots[RECORD_MAX_VCPUS];

/* The words are little endian; the helpers move them as they are. */
static u64 record_read(struct pvsched_vcpu *v, u32 off)
{
	u64 word;

	/* Zero if it fails, which only an unmapped page can make it do. */
	pvsched_area_read_u64(v, off, &word);
	return le64_to_cpu((__force __le64)word);
}

static int record_write(struct pvsched_vcpu *v, u32 off, u64 val)
{
	return pvsched_area_write_u64(v, off, (__force u64)cpu_to_le64(val));
}

#define RECORD_GUEST(field) offsetof(struct pvsched_record_guest, field)
#define RECORD_HOST(field) \
	(PVSCHED_RECORD_HOST_OFFSET + \
	 offsetof(struct pvsched_record_host, field))

static int record_attach(struct pvsched_vcpu *v)
{
	v->priv = kzalloc_obj(struct record_priv, GFP_KERNEL_ACCOUNT);
	return v->priv ? 0 : -ENOMEM;
}

static void record_vmexit(struct pvsched_vcpu *v, bool hlt_exit)
{
	struct record_priv *priv = v->priv;
	u64 switches, irqs, last_pid;
	struct record_slot *slot;

	/*
	 * The vCPU is out of the guest, so nothing changes its words now.  It
	 * may have exited between two of them, which the next copy catches
	 * up on.  The words are guest input, and only recorded.
	 */
	switches = record_read(v, RECORD_GUEST(switches));
	irqs = record_read(v, RECORD_GUEST(irqs));
	last_pid = record_read(v, RECORD_GUEST(last_pid));
	/* The counts only grow: nothing new unless one did. */
	if (switches <= priv->switches && irqs <= priv->irqs)
		return;
	priv->switches = switches;
	priv->irqs = irqs;
	priv->unpublished = true;

	if (v->vcpu_id < 0 || v->vcpu_id >= RECORD_MAX_VCPUS)
		return;
	slot = &record_slots[v->vcpu_id];
	WRITE_ONCE(slot->tgid, task_tgid_nr(v->task));
	WRITE_ONCE(slot->switches, switches);
	WRITE_ONCE(slot->irqs, irqs);
	WRITE_ONCE(slot->last_pid, last_pid);
}

static void record_vmentry(struct pvsched_vcpu *v, bool interrupt_ready)
{
	struct record_priv *priv = v->priv;

	/* Write only after a new copy. */
	if (!priv->unpublished)
		return;
	/* Word by word: no buffer to fill and copy. */
	if (record_write(v, RECORD_HOST(seen_switches), priv->switches) ||
	    record_write(v, RECORD_HOST(seen_irqs), priv->irqs))
		return;
	priv->unpublished = false;
}

/* No scheduling was changed, so there is nothing to restore. */
static void record_detach(struct pvsched_vcpu *v, bool exited)
{
	struct record_priv *priv = v->priv;
	struct pvsched_record_host zero = { };

	/* Leave nothing of ours behind in guest memory: one block write. */
	pvsched_area_write(v, PVSCHED_RECORD_HOST_OFFSET, &zero, sizeof(zero));
	if (v->vcpu_id >= 0 && v->vcpu_id < RECORD_MAX_VCPUS)
		WRITE_ONCE(record_slots[v->vcpu_id].detaches,
			   READ_ONCE(record_slots[v->vcpu_id].detaches) + 1);
	/* IRQs are off here: no console output. */
	pr_debug("vcpu %d (tgid %d): %llu switches, %llu irqs\n", v->vcpu_id,
		 task_tgid_nr(v->task), priv->switches, priv->irqs);
	kfree_rcu(priv, rcu);
}

static struct pvsched_external_ops record_ops = {
	.name = PVSCHED_RECORD_POLICY_NAME,
	.version = PVSCHED_RECORD_POLICY_VERSION,
	.attach = record_attach,
	.vmexit = record_vmexit,
	.vmentry = record_vmentry,
	.detach = record_detach,
};

static int record_show(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_puts(m, "vcpu tgid switches irqs last_pid detaches\n");
	for (i = 0; i < RECORD_MAX_VCPUS; i++) {
		struct record_slot *slot = &record_slots[i];

		if (!READ_ONCE(slot->tgid))
			continue;
		seq_printf(m, "%u %llu %llu %llu %llu %llu\n", i,
			   READ_ONCE(slot->tgid), READ_ONCE(slot->switches),
			   READ_ONCE(slot->irqs), READ_ONCE(slot->last_pid),
			   READ_ONCE(slot->detaches));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(record);

static struct dentry *record_dentry;

static int __init record_init(void)
{
	int ret;

	record_dentry = debugfs_create_file("pvsched_record_host", 0444, NULL,
					    NULL, &record_fops);
	/* Register last: a session may select the policy right after. */
	ret = pvsched_register_external_policy(&record_ops);
	if (ret)
		debugfs_remove(record_dentry);
	return ret;
}
module_init(record_init);

static void __exit record_exit(void)
{
	pvsched_unregister_external_policy(&record_ops);
	debugfs_remove(record_dentry);
}
module_exit(record_exit);

MODULE_DESCRIPTION("pvsched example external policy that records guest counters");
MODULE_LICENSE("GPL");
