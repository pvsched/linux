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
 *                             (pvsched_guest.allow=) and against a guest
 *                             external policy's own (-EPERM on decline)
 *                             and writes a fresh negotiation header on the
 *                             page of every present CPU.
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
 *
 * What the core publishes is chosen when the kernel is built: the default
 * policy's protocol, or the built-in guest side of one external host
 * policy.  A guest external policy negotiates only for its own name and
 * version, with the CUSTOM protocol and POLICY mode; against any other
 * host policy the guest stays inert.
 */
#ifndef _LINUX_PVSCHED_GUEST_H
#define _LINUX_PVSCHED_GUEST_H

#include <linux/jump_label.h>
#include <linux/types.h>

struct task_struct;

#if IS_ENABLED(CONFIG_PARAVIRT_SCHED_GUEST)

int pvsched_guest_prepare(const char *policy, u32 policy_version,
			  void (*kick)(void));
phys_addr_t pvsched_guest_page_gpa(unsigned int cpu);
int pvsched_guest_commit(void);
void pvsched_guest_teardown(void);

/*
 * A guest external policy defines this descriptor and the four functions
 * below, and the core calls them directly.  Each area is its part of a
 * page, bytes PVSCHED_OPAQUE_OFFSET onwards, whose layout it shares with
 * its host policy.  The hooks run only for a vCPU the host enabled.
 */
struct pvsched_guest_ext_policy {
	/* The host policy it negotiates for. */
	const char *name;
	u32 version;
	/* Whether to call pvsched_guest_ext_policy_cs_update() at all. */
	bool cs_update;
	/* Whether to call pvsched_guest_ext_policy_sched_switch() at all. */
	bool sched_switch;
};
extern const struct pvsched_guest_ext_policy pvsched_guest_ext_policy;
/* Publishing starts: the host enabled at least one vCPU.  May sleep. */
int pvsched_guest_ext_policy_start(void);
/* Publishing stopped, and no hook still runs.  May sleep. */
void pvsched_guest_ext_policy_stop(void);
/* On this CPU with IRQs off, after each hardirq or softirq count change. */
void pvsched_guest_ext_policy_cs_update(void *area);
/* At a context switch on this CPU, under its runqueue lock. */
void pvsched_guest_ext_policy_sched_switch(void *area,
					   struct task_struct *prev,
					   struct task_struct *next);

/* @cpu's area while the host has it enabled, otherwise NULL. */
void *pvsched_guest_area(unsigned int cpu);
/* Ask the host for a prompt exit through the transport; preemption off. */
void pvsched_guest_kick(void);

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
