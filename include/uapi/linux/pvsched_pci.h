/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_PVSCHED_PCI_H
#define _UAPI_LINUX_PVSCHED_PCI_H

#include <linux/types.h>

/*
 * pvsched PCI transport binding -- how the per-vCPU page addresses travel
 * from the guest to the VMM. This is one binding of the transport-independent
 * page contract in <linux/pvsched.h>; a virtio or platform-device binding can
 * replace it without touching that contract. Consumers: the guest pvsched
 * driver and the VMM's device model.
 *
 * The device exposes two BARs:
 *   BAR0 (PVSCHED_PCI_BAR_CTRL):  control registers; see PVSCHED_PCI_REG_*.
 *   BAR1 (PVSCHED_PCI_BAR_TABLE): a one-page array of up to
 *                                 PVSCHED_PCI_MAX_VCPUS struct
 *                                 pvsched_pci_entry, one per vCPU the guest
 *                                 opts in.
 *
 * Handshake: the guest fills each opted-in vCPU's page header + the matching
 * BAR1 entry, then writes the number of valid entries to
 * PVSCHED_PCI_REG_DOORBELL. That MMIO write traps to the VMM, which
 * translates each entry's GPA and registers the page with the host framework
 * (/dev/pvsched); the framework writes each header's status. A doorbell write
 * (re)publishes the full set: the VMM registers the listed pages and
 * unregisters any vCPU it had registered that is no longer listed, so writing
 * 0 tears everything down. The trap is synchronous -- the status-visibility
 * point this binding defines is the retiring doorbell write, so the guest
 * reads each header's status on return, no polling.
 */

struct pvsched_pci_entry {
	__u64 apic_id;	/* guest APIC id of the vCPU */
	__u64 gpa;	/* guest-physical address of that vCPU's pvsched page */
};

/* BAR1 (the entry table) is one page, regardless of guest/host page size. */
#define PVSCHED_PCI_TABLE_SIZE	4096
#define PVSCHED_PCI_MAX_VCPUS	\
	(PVSCHED_PCI_TABLE_SIZE / sizeof(struct pvsched_pci_entry))

#define PVSCHED_PCI_BAR_CTRL	0	/* BAR index: control registers */
#define PVSCHED_PCI_BAR_TABLE	1	/* BAR index: pvsched_pci_entry table */

/*
 * BAR0 (control) register offsets.
 *
 * The device ADVERTISES the policy this VM should request (the host knows
 * what is loaded; the guest echoes the advertisement into each page header,
 * subject to its own acceptance policy). The kick register lets the guest
 * induce a synchronous exit on demand: the trap itself is the event -- the
 * host policy samples guest_area at the resulting vmexit -- so the device
 * ignores the value written. Used for prompt deboost notification when the
 * host sets PVSCHED_HINT_KICK_DEBOOST.
 */
#define PVSCHED_PCI_REG_DOORBELL	0x00	/* u32 W: number of valid BAR1 entries */
/*
 * The device advertises a single {version, name} policy today; the guest
 * echoes it into each page header. A future multi-policy menu (the device
 * offers a set, the guest selects one) is expected to extend this region with
 * additional advertisement slots rather than redefine these registers.
 */
#define PVSCHED_PCI_REG_POLICY_VERSION	0x04	/* u32 RO: advertised policy version */
#define PVSCHED_PCI_REG_POLICY_NAME	0x08	/* 32 bytes RO: advertised name, NUL-padded */
#define PVSCHED_PCI_REG_KICK		0x28	/* u32 W: induce an exit; value ignored */

/*
 * PCI identity of the pvsched device. Prototype placeholders under the QEMU
 * (Red Hat) vendor id; revisit before any upstream submission.
 */
#define PVSCHED_PCI_VENDOR_ID	0x1b36
#define PVSCHED_PCI_DEVICE_ID	0x11f0

#endif /* _UAPI_LINUX_PVSCHED_PCI_H */
