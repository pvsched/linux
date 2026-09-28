/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Paravirtualized scheduling: guest core <-> transport interface.
 *
 * The built-in guest core owns the per-vCPU shared pages and the publishing
 * hooks.  A transport driver, such as the pvsched PCI binding, only carries
 * the page addresses to the VMM and reports back:
 *
 *   pvsched_guest_prepare()   hands the core the policy the device
 *                             advertises and the transport's kick op.  The
 *                             core checks the policy against the guest
 *                             administrator's acceptance list
 *                             (pvsched_guest.allow=, -EPERM on decline) and
 *                             writes a fresh negotiation header on the page
 *                             of every present CPU.
 *   pvsched_guest_page_gpa()  returns the page address of a CPU that
 *                             prepare reset, or 0 for any other CPU.  The
 *                             transport builds its {apic_id, gpa} set by
 *                             walking the possible CPUs, so the set is
 *                             exactly the pages prepare reset.
 *   ...the transport hands the set to the VMM, whose host writes each
 *      page's status before that returns...
 *   pvsched_guest_commit()    scans the statuses and, if at least one vCPU
 *                             is ENABLED, starts publishing.  Returns the
 *                             number enabled, or -errno.
 *   pvsched_guest_teardown()  stops publishing.  The transport then detaches
 *                             the pages host-side; the pages are kept for
 *                             reuse.
 */
#ifndef _LINUX_PVSCHED_GUEST_H
#define _LINUX_PVSCHED_GUEST_H

#include <linux/jump_label.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_PARAVIRT_SCHED_GUEST)

int pvsched_guest_prepare(const char *policy, u32 policy_version,
			  void (*kick)(void));
phys_addr_t pvsched_guest_page_gpa(unsigned int cpu);
int pvsched_guest_commit(void);
void pvsched_guest_teardown(void);

/*
 * The interrupt-context hook, enabled only while publishing.  It runs with
 * IRQs disabled, right after every hardirq or softirq count change: at
 * hardirq entry once the count is raised, and at hardirq exit and softirq
 * begin and end once it has changed.
 */
DECLARE_STATIC_KEY_FALSE(pvsched_guest_irq_key);
void __pvsched_guest_cs_update(void);

static __always_inline void pvsched_guest_cs_update(void)
{
	if (static_branch_unlikely(&pvsched_guest_irq_key))
		__pvsched_guest_cs_update();
}

#else /* !CONFIG_PARAVIRT_SCHED_GUEST */

/* Only the interrupt hook needs a stub: the transports require the core. */
static inline void pvsched_guest_cs_update(void) { }

#endif /* CONFIG_PARAVIRT_SCHED_GUEST */

#endif /* _LINUX_PVSCHED_GUEST_H */
