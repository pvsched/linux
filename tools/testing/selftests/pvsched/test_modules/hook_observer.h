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
};

struct pvsched_hook_record {
	__u64 seq;
	__u32 event;
	__u32 vcpu_id;
	__s32 ret;
	__u32 exit_reason;
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

#endif
