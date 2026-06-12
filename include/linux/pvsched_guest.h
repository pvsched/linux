/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Paravirt scheduling - guest core <-> transport interface.
 *
 * The built-in guest core owns the per-vCPU shared pages and the publishing
 * hooks (it attaches to unexported core tracepoints, which a module cannot).
 * A transport driver -- e.g. the pvsched PCI device binding -- only moves the
 * page addresses to the VMM and reports back:
 *
 *   pvsched_guest_prepare()   hand the core the policy the device ADVERTISES
 *                             (the host names the policy; the core validates
 *                             it against the guest admin's acceptance list,
 *                             pvsched_guest.allow=, and returns -EPERM on
 *                             decline) plus the transport's kick op (induce a
 *                             synchronous exit; used for prompt deboost when
 *                             the host policy asks for it). Returns the
 *                             {apic_id, gpa} set to publish; headers
 *                             re-filled, statuses reset, pages allocated once
 *                             and reused.
 *   ...the transport hands the set to the VMM (e.g. BAR table + doorbell;
 *      the host writes each page's status before that returns)...
 *   pvsched_guest_commit()    the core scans the statuses and, if at least
 *                             one vCPU is ENABLED, attaches the publishing
 *                             hooks. Returns the number enabled, or -errno.
 *   pvsched_guest_teardown()  detach the hooks. The transport unregisters
 *                             the pages host-side afterwards; the pages
 *                             themselves persist for reuse.
 */
#ifndef _LINUX_PVSCHED_GUEST_H
#define _LINUX_PVSCHED_GUEST_H

#include <linux/errno.h>
#include <linux/types.h>

struct pvsched_guest_entry {
	u64 apic_id;
	u64 gpa;
};

#if IS_ENABLED(CONFIG_PARAVIRT_SCHED_GUEST)

int pvsched_guest_prepare(const char *policy, u32 policy_version,
			  void (*kick)(void),
			  const struct pvsched_guest_entry **entries,
			  unsigned int *nr);
int pvsched_guest_commit(void);
void pvsched_guest_teardown(void);

#else /* !CONFIG_PARAVIRT_SCHED_GUEST */

static inline int
pvsched_guest_prepare(const char *policy, u32 policy_version,
		      void (*kick)(void),
		      const struct pvsched_guest_entry **entries,
		      unsigned int *nr)
{
	return -EOPNOTSUPP;
}

static inline int pvsched_guest_commit(void)
{
	return 0;
}

static inline void pvsched_guest_teardown(void) { }

#endif /* CONFIG_PARAVIRT_SCHED_GUEST */

#endif /* _LINUX_PVSCHED_GUEST_H */
