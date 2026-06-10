/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_PVSCHED_H
#define _UAPI_LINUX_PVSCHED_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Paravirt scheduling (pvsched) ABI -- version 1.
 *
 * pvsched lets a guest and the host cooperate on scheduling through memory
 * shared per vCPU. The guest allocates one page per vCPU it wants paravirt-
 * scheduled and hands the host the guest-physical address of each through a
 * discovery transport. Each page has three sections:
 *
 *   - header      negotiation: the guest advertises the ABI and the policy it
 *                 wants; the host writes back whether pvsched is enabled.
 *   - guest_area  the guest publishes its current scheduling intent (updated
 *                 from scheduler events: sched_switch, irq/softirq/nmi).
 *   - host_area   the host publishes the vCPU state and the params it applied.
 *
 * This header is the transport-independent contract shared by the guest
 * pvsched driver, the host pvsched framework + policy (kernel module or BPF),
 * and the VMM. How the page addresses travel from guest to VMM is a separate
 * transport binding (the first is a minimal PCI device; a virtio or
 * platform-device binding can replace it without touching this contract).
 * A binding must define the point at which the host-written status becomes
 * visible to the guest. Any incompatible change bumps PVSCHED_ABI_VERSION.
 */

#define PVSCHED_ABI_VERSION	1

/*
 * Fixed per-vCPU page stride: one 4 KiB page. This is an ABI constant and is
 * deliberately NOT the kernel PAGE_SIZE -- guest and host may use different
 * page sizes, so the stride must be identical on both sides.
 */
#define PVSCHED_VCPU_STRIDE	4096

#define PVSCHED_NAME_MAX	32

/*
 * Header status, written by the host during registration and read by the guest
 * to gate its scheduler hooks.
 */
enum pvsched_status {
	PVSCHED_STATUS_DISABLED			= 0, /* default; pvsched not active here */
	PVSCHED_STATUS_ENABLED			= 1, /* handshake ok; hooks may run */
	PVSCHED_STATUS_UNKNOWN_POLICY		= 2, /* no registered policy matches the name */
	PVSCHED_STATUS_ABI_MISMATCH		= 3, /* header abi_version != host pvsched ABI */
	PVSCHED_STATUS_POLICY_VERSION_MISMATCH	= 4, /* policy name found, but version differs */
};

/*
 * Per-vCPU page, section 1: negotiation header.
 *
 * The guest fills the guest-written fields before handing the page to the
 * host; the host fills the host-written fields during registration.
 */
struct pvsched_header {
	/* written by the guest */
	__u32 abi_version;			/* PVSCHED_ABI_VERSION the guest speaks */
	__u32 policy_version;			/* version of the policy the guest wants */
	char  policy_name[PVSCHED_NAME_MAX];	/* name of the policy the guest wants */
	/* written by the host */
	__u32 status;				/* enum pvsched_status */
	__u32 host_abi_version;			/* ABI the host/policy speaks */
	__u32 reserved[4];			/* pad to 64; reserved, must be zero */
};

/* Guest in-kernel critical-section flags (pvsched_guest_area.kern_cs). */
enum pvsched_kern_cs {
	PVSCHED_KERNCS_NMI		= 0x01,
	PVSCHED_KERNCS_HARDIRQ		= 0x02,
	PVSCHED_KERNCS_SOFTIRQ		= 0x04,
	PVSCHED_KERNCS_PREEMPT_DISABLED	= 0x08,
};

/*
 * vCPU status published by the host (pvsched_host_area.vcpu_status). The base
 * status is in the low nibble (mask with PVSCHED_VCPU_STATUS_MASK); interrupt
 * injection can happen from any base state and is OR'd into the high nibble.
 */
#define PVSCHED_VCPU_STATUS_MASK	0x0f
enum pvsched_vcpu_status {
	PVSCHED_VCPU_STATUS_VMENTERED		= 1,
	PVSCHED_VCPU_STATUS_VMEXITED		= 2,
	PVSCHED_VCPU_STATUS_HALTED		= 3,
	PVSCHED_VCPU_STATUS_INTR_INJECTED	= 0xf0,
};

/* Per-vCPU page, section 2: written by the guest, read by the host. */
struct pvsched_guest_area {
	__u8 kern_cs;		/* bitmask of enum pvsched_kern_cs */
	__u8 sched_policy;	/* requested SCHED_* policy */
	__s8 nice;		/* requested nice value, if CFS */
	__u8 rt_prio;		/* requested RT priority, if RT */
	__u8 reserved[28];	/* pad to 32; reserved, must be zero */
};

/*
 * Publishing hints (pvsched_host_area.hints), written by the host policy so
 * the guest core can adapt its publishing to what the policy wants. A guest
 * that ignores them merely loses the optimization.
 */
#define PVSCHED_HINT_KICK_DEBOOST 0x1	/* induce an immediate exit when a
					 * published boost-class request drops
					 * to non-boost, so the host can
					 * deboost promptly */

/* Per-vCPU page, section 3: written by the host, read by the guest. */
struct pvsched_host_area {
	__u8 vcpu_status;	/* enum pvsched_vcpu_status */
	__u8 sched_policy;	/* policy applied before VMENTER */
	__s8 nice;		/* applied nice value, if CFS */
	__u8 rt_prio;		/* applied RT priority, if RT */
	__u32 hints;		/* PVSCHED_HINT_* publishing hints */
	__u8 reserved[24];	/* pad to 32; reserved, must be zero */
};

/*
 * The per-vCPU page: header @0, guest_area @64, host_area @96; the rest is
 * reserved for growth (covered by PVSCHED_ABI_VERSION).
 */
union pvsched_vcpu_page {
	struct {
		struct pvsched_header		header;
		struct pvsched_guest_area	guest_area;
		struct pvsched_host_area	host_area;
	};
	__u8 reserved[PVSCHED_VCPU_STRIDE];
};

/*
 * Framework registration interface (/dev/pvsched).
 *
 * After translating a guest-published {vCPU id, GPA} entry from the discovery
 * transport, the VMM registers one vCPU's shared page with the host framework:
 * @tid is the vCPU thread (in the initial pid namespace) and @shm_hva is the
 * host VA of that vCPU's page in the VMM's own address space.
 *
 * @shm_hva is an opaque handle, not a pointer the VMM dereferences: the page is
 * guest<->host memory, and the framework pins it and touches it only through its
 * own kernel mapping. The VMM is a courier -- it must not read or write the page
 * itself. On REGISTER the framework reads the page header, matches the requested
 * policy, and either binds the vCPU (returns 0, status ENABLED) or declines
 * (returns -ENOENT, with the precise reason -- ABI / name / version -- left in
 * the status byte); a second REGISTER for an already-bound vCPU returns -EEXIST.
 * UNREGISTER is a full teardown that returns the vCPU thread to baseline.
 *
 * Both ioctls require CAP_SYS_NICE: registering a vCPU delegates to the bound
 * host policy the authority to raise that thread's scheduling priority -- the
 * authority that capability already governs -- and unregistering severs a
 * binding.
 */
struct pvsched_reg_vcpu {
	__u32 tid;
	__u32 pad;
	__u64 shm_hva;
};

#define PVSCHED_IOC_MAGIC		0xB7
#define PVSCHED_IOC_REGISTER_VCPU	_IOW(PVSCHED_IOC_MAGIC, 1, struct pvsched_reg_vcpu)
/* Unbind a vCPU (arg: its thread tid). The VMM calls this when the guest tears
 * down pvsched for the vCPU, before freeing/repurposing its page. */
#define PVSCHED_IOC_UNREGISTER_VCPU	_IOW(PVSCHED_IOC_MAGIC, 2, __u32)

#endif /* _UAPI_LINUX_PVSCHED_H */
