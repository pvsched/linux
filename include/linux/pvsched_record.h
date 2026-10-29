/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The "record" example of an external pvsched policy, a reference for
 * writing one.  It changes no scheduling: the guest counts, the host
 * records, and each side shows what it has in debugfs, so a test can
 * compare them.
 *
 * Guest side (virt/pvsched/guest_record.c, PVSCHED_GUEST_RECORD): it
 * negotiates for "record" v1 with the CUSTOM protocol, and on each vCPU
 * its sched_switch and interrupt hooks keep totals of context switches
 * and hardirq events, and the pid switched in last, in struct
 * pvsched_record_guest.  /sys/kernel/debug/pvsched_record shows each
 * CPU's totals next to what the host last wrote back.
 *
 * Host policy (tools/testing/selftests/pvsched/test_modules/record.c,
 * pvsched-record.ko):
 *   attach   allocates its per-vCPU state in v->priv;
 *   vmexit   at each slow exit, copies the totals if a count grew.
 *            Fast exits do not call it; the next one catches up;
 *   vmentry  writes the copied totals back, in struct
 *            pvsched_record_host;
 *   detach   scrubs those words and frees v->priv.
 * /sys/kernel/debug/pvsched_record_host shows each vCPU's last copy and
 * how many detaches it saw.
 *
 * In a running VM, the guest's seen_switches and seen_irqs trail its live
 * totals and never pass them, both grow as the VM runs, and the host's
 * rows match what the guest was told.
 *
 * Offsets are within the policy's area, which starts at byte
 * PVSCHED_OPAQUE_OFFSET of a vCPU page: the guest writes from offset 0,
 * the host from PVSCHED_RECORD_HOST_OFFSET.  Every word is accessed whole.
 */
#ifndef _LINUX_PVSCHED_RECORD_H
#define _LINUX_PVSCHED_RECORD_H

#include <linux/types.h>

#define PVSCHED_RECORD_POLICY_NAME	"record"
#define PVSCHED_RECORD_POLICY_VERSION	1

/*
 * Written by the guest at offset 0: totals since this vCPU started
 * publishing.  Each word is written whole, only by its own vCPU, and the
 * counts only grow.
 */
struct pvsched_record_guest {
	/* Context switches on this vCPU. */
	__le64 switches;
	/* Hardirq count changes that leave the vCPU in a hardirq. */
	__le64 irqs;
	/* The pid switched in last; 0 for the idle task. */
	__le64 last_pid;
};

#define PVSCHED_RECORD_HOST_OFFSET	64

/* Written by the host at PVSCHED_RECORD_HOST_OFFSET, at VM entry only. */
struct pvsched_record_host {
	/* The totals the host copied last. */
	__le64 seen_switches;
	__le64 seen_irqs;
};

#endif /* _LINUX_PVSCHED_RECORD_H */
