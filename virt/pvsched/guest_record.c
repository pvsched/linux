// SPDX-License-Identifier: GPL-2.0-only
/*
 * Guest side of the "record" example external policy (see
 * <linux/pvsched_record.h>).  It counts this vCPU's context switches and
 * hardirq events in its area; the host records them.  Both hooks run on
 * the area's own CPU with IRQs off, so neither interrupts the other.
 */

#define pr_fmt(fmt) "pvsched_record: " fmt

#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/pvsched_guest.h>
#include <linux/pvsched_record.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

const struct pvsched_guest_ext_policy pvsched_guest_ext_policy = {
	.name = PVSCHED_RECORD_POLICY_NAME,
	.version = PVSCHED_RECORD_POLICY_VERSION,
	.cs_update = true,
	.sched_switch = true,
};

int pvsched_guest_ext_policy_start(void)
{
	return 0;
}

void pvsched_guest_ext_policy_stop(void)
{
}

static u64 pvsched_record_read(const __le64 *word)
{
	return le64_to_cpu(READ_ONCE(*word));
}

static void pvsched_record_write(__le64 *word, u64 val)
{
	WRITE_ONCE(*word, cpu_to_le64(val));
}

void pvsched_guest_ext_policy_sched_switch(void *area,
					   struct task_struct *prev,
					   struct task_struct *next)
{
	struct pvsched_record_guest *rec = area;

	pvsched_record_write(&rec->switches,
			     pvsched_record_read(&rec->switches) + 1);
	pvsched_record_write(&rec->last_pid, task_pid_nr(next));
}

void pvsched_guest_ext_policy_cs_update(void *area)
{
	struct pvsched_record_guest *rec = area;

	if (!in_hardirq())
		return;
	pvsched_record_write(&rec->irqs, pvsched_record_read(&rec->irqs) + 1);
}

static int pvsched_record_show(struct seq_file *m, void *v)
{
	unsigned int cpu;

	seq_puts(m, "cpu switches irqs last_pid seen_switches seen_irqs\n");
	for_each_present_cpu(cpu) {
		void *area = pvsched_guest_area(cpu);
		struct pvsched_record_guest *rec = area;
		struct pvsched_record_host *host;

		if (!area)
			continue;
		host = area + PVSCHED_RECORD_HOST_OFFSET;
		seq_printf(m, "%u %llu %llu %llu %llu %llu\n", cpu,
			   pvsched_record_read(&rec->switches),
			   pvsched_record_read(&rec->irqs),
			   pvsched_record_read(&rec->last_pid),
			   pvsched_record_read(&host->seen_switches),
			   pvsched_record_read(&host->seen_irqs));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pvsched_record);

static int __init pvsched_record_init(void)
{
	debugfs_create_file("pvsched_record", 0444, NULL, NULL,
			    &pvsched_record_fops);
	return 0;
}
late_initcall(pvsched_record_init);
