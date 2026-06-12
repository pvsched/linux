// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Google */

/*
 * Paravirt scheduling - guest core.
 *
 * Owns the per-vCPU shared pages and the publishing hooks, and exposes the
 * three-call interface a transport driver uses to wire them to the VMM (see
 * include/linux/pvsched_guest.h). The transport is only a courier; everything
 * that touches kernel internals lives here, built-in -- the hooks attach to
 * unexported core tracepoints, which a module could not.
 *
 * The policy to request is ADVERTISED by the host through the transport (the
 * host alone knows what is loaded); the core echoes it into the negotiation
 * headers, subject to the guest admin's acceptance list (pvsched_guest.allow=;
 * acceptance is about semantics and overhead, not security -- pvsched grants
 * the host no power it does not already have over vCPU scheduling).
 *
 * Publishing: a probe on the sched_switch tracepoint writes the next task's
 * policy/nice/rt_prio into the vCPU's guest_area as one aligned 32-bit store.
 * Boosts are taken lazily -- while the vCPU runs it needs nothing from the
 * host -- but releases are signalled promptly: when the host policy sets
 * PVSCHED_HINT_KICK_DEBOOST in host_area and a published request drops in
 * priority, the probe induces an immediate exit through the
 * transport's kick op, so the host can deboost without waiting for a natural
 * vmexit (which a tick-less, CPU-bound vCPU might never take). Probes around
 * the hardirq and softirq handlers set/clear the matching kern_cs bits (the
 * NMI and PREEMPT_DISABLED bits are future work: NMIs have no bracketing
 * tracepoint and the preempt on/off tracepoints depend on debug Kconfigs).
 *
 * The gating costs nothing when pvsched is not negotiated: the hooks are only
 * attached when the host enabled at least one vCPU (an unattached tracepoint
 * is a patched-out static branch), and each event checks the per-CPU status,
 * so a guest with this support runs unchanged on a non-pvsched host.
 *
 * The pages persist once allocated (one per possible CPU): they are cheap,
 * and reuse keeps re-negotiation (transport rebind) trivial.
 */

#define pr_fmt(fmt) "pvsched_guest: " fmt

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/interrupt.h>	/* completes the irq trace event types */
#include <linux/io.h>		/* virt_to_phys */
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pvsched_guest.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>	/* MAX_RT_PRIO, MAX_PRIO, NICE_TO_PRIO */
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tracepoint.h>

#include <trace/events/irq.h>
#include <trace/events/sched.h>

#include <asm/smp.h>		/* x86_cpu_to_apicid */

#include <uapi/linux/pvsched.h>

/* The guest admin's acceptance list; "*" accepts whatever is advertised. */
static char pvsched_allow[256] = "*";
module_param_string(allow, pvsched_allow, sizeof(pvsched_allow), 0444);

static DEFINE_PER_CPU(union pvsched_vcpu_page *, pvsched_page);
static DEFINE_PER_CPU(u64, pvsched_kicks);

/* Serializes prepare/commit/teardown (transport bind/unbind paths). */
static DEFINE_MUTEX(pvsched_guest_mutex);
static struct pvsched_guest_entry *pvsched_entries;
static unsigned int pvsched_nr_entries;
static bool pvsched_hooks_on;

/* The accepted advertisement and the transport's kick op. */
static char pvsched_policy[PVSCHED_NAME_MAX];
static u32 pvsched_policy_version;
static void (*pvsched_kick)(void);

union pvsched_guest_word {
	struct pvsched_guest_area ga;
	u32 word;
};

/*
 * Scheduling priority of a published request in the kernel's convention: lower
 * value is higher priority (cf. __task_prio() in kernel/sched/core.c). The
 * switch probe uses it to tell a deboost (priority dropped versus the last
 * publish) from a boost.
 */
static int pvsched_guest_prio(const struct pvsched_guest_area *ga)
{
	switch (ga->sched_policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		return MAX_RT_PRIO - 1 - ga->rt_prio;	/* rt_prio 1..99 -> 98..0 */
	case SCHED_IDLE:
		return MAX_PRIO;			/* below any nice level */
	default:					/* SCHED_NORMAL, SCHED_BATCH */
		return NICE_TO_PRIO(ga->nice);		/* nice -20..19 -> 100..139 */
	}
}

/*
 * sched_switch probe: publish the parameters of the task this vCPU is about
 * to run. Runs under the rq lock with preemption disabled; one aligned 32-bit
 * store, so the host never reads a torn pair.
 *
 * Publish first, then kick: the host samples guest_area at the induced exit,
 * so it must already see the new (lower) request.
 */
static void pvsched_guest_switch_probe(void *data, bool preempt,
				       struct task_struct *prev,
				       struct task_struct *next,
				       unsigned int prev_state)
{
	union pvsched_vcpu_page *page = __this_cpu_read(pvsched_page);
	union pvsched_guest_word old, pub = { };
	void (*kick)(void);

	if (!page || READ_ONCE(page->header.status) != PVSCHED_STATUS_ENABLED)
		return;

	old.word = READ_ONCE(*(u32 *)&page->guest_area);

	pub.ga.kern_cs		= old.ga.kern_cs;
	pub.ga.sched_policy	= next->policy;
	pub.ga.nice		= task_nice(next);
	pub.ga.rt_prio		= next->rt_priority;
	WRITE_ONCE(*(u32 *)&page->guest_area, pub.word);

	/* Deboost (prio value went up): kick if the host asked to be told. */
	if (pvsched_guest_prio(&pub.ga) > pvsched_guest_prio(&old.ga) &&
	    (READ_ONCE(page->host_area.hints) & PVSCHED_HINT_KICK_DEBOOST)) {
		kick = READ_ONCE(pvsched_kick);
		if (kick) {
			__this_cpu_inc(pvsched_kicks);
			kick();
		}
	}
}

/*
 * In-kernel critical-section bits: set/cleared around the hardirq and softirq
 * handlers, single-byte RMW on this CPU's own page. Best-effort by design: a
 * hardirq nesting into the softirq probe's read-modify-write window can be
 * momentarily misrepresented, but the bits self-heal on the handler's exit
 * event -- acceptable for a scheduling hint, and it keeps the hot path free
 * of irq disabling or atomics.
 */
static __always_inline void pvsched_guest_kerncs(u8 bit, bool set)
{
	union pvsched_vcpu_page *page = __this_cpu_read(pvsched_page);
	u8 cs;

	if (!page || READ_ONCE(page->header.status) != PVSCHED_STATUS_ENABLED)
		return;

	cs = READ_ONCE(page->guest_area.kern_cs);
	WRITE_ONCE(page->guest_area.kern_cs, set ? (cs | bit) : (cs & ~bit));
}

static void pvsched_guest_irq_entry_probe(void *data, int irq,
					  struct irqaction *action)
{
	pvsched_guest_kerncs(PVSCHED_KERNCS_HARDIRQ, true);
}

static void pvsched_guest_irq_exit_probe(void *data, int irq,
					 struct irqaction *action, int ret)
{
	pvsched_guest_kerncs(PVSCHED_KERNCS_HARDIRQ, false);
}

static void pvsched_guest_softirq_entry_probe(void *data, unsigned int vec_nr)
{
	pvsched_guest_kerncs(PVSCHED_KERNCS_SOFTIRQ, true);
}

static void pvsched_guest_softirq_exit_probe(void *data, unsigned int vec_nr)
{
	pvsched_guest_kerncs(PVSCHED_KERNCS_SOFTIRQ, false);
}

static int pvsched_guest_attach_hooks(void)
{
	int ret;

	ret = register_trace_sched_switch(pvsched_guest_switch_probe, NULL);
	if (ret)
		return ret;
	ret = register_trace_irq_handler_entry(pvsched_guest_irq_entry_probe,
					       NULL);
	if (ret)
		goto err_irq_entry;
	ret = register_trace_irq_handler_exit(pvsched_guest_irq_exit_probe,
					      NULL);
	if (ret)
		goto err_irq_exit;
	ret = register_trace_softirq_entry(pvsched_guest_softirq_entry_probe,
					   NULL);
	if (ret)
		goto err_softirq_entry;
	ret = register_trace_softirq_exit(pvsched_guest_softirq_exit_probe,
					  NULL);
	if (ret)
		goto err_softirq_exit;
	return 0;

err_softirq_exit:
	unregister_trace_softirq_entry(pvsched_guest_softirq_entry_probe, NULL);
err_softirq_entry:
	unregister_trace_irq_handler_exit(pvsched_guest_irq_exit_probe, NULL);
err_irq_exit:
	unregister_trace_irq_handler_entry(pvsched_guest_irq_entry_probe, NULL);
err_irq_entry:
	unregister_trace_sched_switch(pvsched_guest_switch_probe, NULL);
	return ret;
}

static void pvsched_guest_detach_hooks(void)
{
	unregister_trace_softirq_exit(pvsched_guest_softirq_exit_probe, NULL);
	unregister_trace_softirq_entry(pvsched_guest_softirq_entry_probe, NULL);
	unregister_trace_irq_handler_exit(pvsched_guest_irq_exit_probe, NULL);
	unregister_trace_irq_handler_entry(pvsched_guest_irq_entry_probe, NULL);
	unregister_trace_sched_switch(pvsched_guest_switch_probe, NULL);
	tracepoint_synchronize_unregister();
}

/* "*" accepts anything; otherwise a comma list of <name> or <name:version>. */
static bool pvsched_guest_allowed(const char *policy, u32 version)
{
	char buf[sizeof(pvsched_allow)];
	char *cur = buf, *tok, *colon;

	if (!strcmp(pvsched_allow, "*"))
		return true;

	strscpy(buf, pvsched_allow, sizeof(buf));
	while ((tok = strsep(&cur, ",")) != NULL) {
		unsigned long v;

		colon = strchr(tok, ':');
		if (colon) {
			*colon = '\0';
			if (kstrtoul(colon + 1, 10, &v) || v != version)
				continue;
		}
		if (!strcmp(tok, policy))
			return true;
	}
	return false;
}

int pvsched_guest_prepare(const char *policy, u32 policy_version,
			  void (*kick)(void),
			  const struct pvsched_guest_entry **entries,
			  unsigned int *nr)
{
	unsigned int cpu, n = 0;
	int ret = 0;

	/* The shared-page stride is one guest page (x86-64 only for now). */
	BUILD_BUG_ON(PVSCHED_VCPU_STRIDE != PAGE_SIZE);

	if (!policy || !policy[0] ||
	    strnlen(policy, PVSCHED_NAME_MAX) >= PVSCHED_NAME_MAX)
		return -EINVAL;

	mutex_lock(&pvsched_guest_mutex);
	if (pvsched_hooks_on) {
		ret = -EBUSY;
		goto out;
	}

	if (!pvsched_guest_allowed(policy, policy_version)) {
		pr_info("advertised policy '%s' v%u not accepted (pvsched_guest.allow=%s)\n",
			policy, policy_version, pvsched_allow);
		ret = -EPERM;
		goto out;
	}
	strscpy(pvsched_policy, policy, sizeof(pvsched_policy));
	pvsched_policy_version = policy_version;
	WRITE_ONCE(pvsched_kick, kick);

	if (!pvsched_entries) {
		pvsched_entries = kcalloc(num_possible_cpus(),
					  sizeof(*pvsched_entries),
					  GFP_KERNEL);
		if (!pvsched_entries) {
			ret = -ENOMEM;
			goto out;
		}
	}

	for_each_possible_cpu(cpu) {
		union pvsched_vcpu_page *page = per_cpu(pvsched_page, cpu);

		if (!page) {
			page = (void *)get_zeroed_page(GFP_KERNEL);
			if (!page) {
				ret = -ENOMEM;
				goto out;
			}
			per_cpu(pvsched_page, cpu) = page;
		}

		/* (Re)negotiate from a clean slate. */
		page->header.abi_version = PVSCHED_ABI_VERSION;
		page->header.policy_version = pvsched_policy_version;
		strscpy(page->header.policy_name, pvsched_policy,
			PVSCHED_NAME_MAX);
		WRITE_ONCE(page->header.status, PVSCHED_STATUS_DISABLED);

		pvsched_entries[n].apic_id = per_cpu(x86_cpu_to_apicid, cpu);
		pvsched_entries[n].gpa = virt_to_phys(page);
		n++;
	}
	pvsched_nr_entries = n;

	*entries = pvsched_entries;
	*nr = n;
out:
	mutex_unlock(&pvsched_guest_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(pvsched_guest_prepare);

int pvsched_guest_commit(void)
{
	unsigned int cpu, enabled = 0;
	int ret;

	mutex_lock(&pvsched_guest_mutex);
	for_each_possible_cpu(cpu) {
		union pvsched_vcpu_page *page = per_cpu(pvsched_page, cpu);

		if (page && READ_ONCE(page->header.status) ==
			    PVSCHED_STATUS_ENABLED)
			enabled++;
	}

	if (enabled && !pvsched_hooks_on) {
		ret = pvsched_guest_attach_hooks();
		if (ret) {
			mutex_unlock(&pvsched_guest_mutex);
			return ret;
		}
		pvsched_hooks_on = true;
	}
	mutex_unlock(&pvsched_guest_mutex);

	pr_info("policy '%s' v%u: %u/%u vCPUs enabled\n", pvsched_policy,
		pvsched_policy_version, enabled, pvsched_nr_entries);
	return enabled;
}
EXPORT_SYMBOL_GPL(pvsched_guest_commit);

void pvsched_guest_teardown(void)
{
	mutex_lock(&pvsched_guest_mutex);
	if (pvsched_hooks_on) {
		pvsched_guest_detach_hooks();
		pvsched_hooks_on = false;
	}
	/* No probe can run past the detach's synchronization. */
	WRITE_ONCE(pvsched_kick, NULL);
	mutex_unlock(&pvsched_guest_mutex);
}
EXPORT_SYMBOL_GPL(pvsched_guest_teardown);

static int pvsched_kicks_get(void *data, u64 *val)
{
	unsigned int cpu;
	u64 sum = 0;

	for_each_possible_cpu(cpu)
		sum += per_cpu(pvsched_kicks, cpu);
	*val = sum;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(pvsched_kicks_fops, pvsched_kicks_get, NULL, "%llu\n");

static int __init pvsched_guest_init(void)
{
	struct dentry *dir = debugfs_create_dir("pvsched_guest", NULL);

	debugfs_create_file("kicks", 0444, dir, NULL, &pvsched_kicks_fops);
	return 0;
}
late_initcall(pvsched_guest_init);
