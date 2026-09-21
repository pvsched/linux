/* SPDX-License-Identifier: GPL-2.0 */
/* Private test-only observer ABI; never install or export as UAPI. */
#ifndef _PVSCHED_HOOK_OBSERVER_H
#define _PVSCHED_HOOK_OBSERVER_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define PVSCHED_HOOK_OBSERVER_DEVICE "pvsched-hook-observer"
#define PVSCHED_HOOK_OBSERVER_MAGIC 0xf5
#define PVSCHED_HOOK_MAX_RECORDS 64

enum pvsched_hook_event {
	PVSCHED_HOOK_ENTER = 1,
	PVSCHED_HOOK_LEAVE = 2,
	PVSCHED_HOOK_VMENTRY = 3,
	PVSCHED_HOOK_VMEXIT_IRQOFF = 4,
	PVSCHED_HOOK_VMEXIT = 6,
	PVSCHED_HOOK_VMENTRY_CANCEL = 7,
	PVSCHED_HOOK_HALT = 8,
	PVSCHED_HOOK_UNHALT = 9,
	PVSCHED_HOOK_INJECT_INTR = 10,
};

#define PVSCHED_HOOK_RECORD_F_INTERRUPT_READY	(1U << 0)
#define PVSCHED_HOOK_RECORD_F_HLT_EXIT		(1U << 2)

/*
 * Mode bits that report host configuration rather than the VM under test:
 * with AVIC or virtual NMI enabled in kvm_amd, every VM reports them.
 */
#define PVSCHED_HOOK_MODE_SVM_AVIC		(1U << 2)
#define PVSCHED_HOOK_MODE_SVM_VNMI		(1U << 3)

struct pvsched_hook_record {
	__u64 seq;
	__u32 event;
	__u32 vcpu_id;
	__s32 ret;
	__u32 exit_reason;
	__u32 mode_flags;
	__u32 event_flags;
};

struct pvsched_hook_snapshot {
	__u32 count;
	__u32 flags;
	__u64 next_seq;
	struct pvsched_hook_record records[PVSCHED_HOOK_MAX_RECORDS];
};

#define PVSCHED_HOOK_F_OVERFLOW (1U << 0)
#define PVSCHED_HOOK_F_UNMATCHED (1U << 1)

#define PVSCHED_HOOK_IOC_RESET _IO(PVSCHED_HOOK_OBSERVER_MAGIC, 0)
#define PVSCHED_HOOK_IOC_WAIT_HALT _IO(PVSCHED_HOOK_OBSERVER_MAGIC, 2)

#endif
