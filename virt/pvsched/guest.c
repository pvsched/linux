// SPDX-License-Identifier: GPL-2.0-only
/*
 * Paravirtualized scheduling: guest core.
 *
 * Owns one shared page per vCPU and the hooks that publish this guest's
 * scheduling intent into it, and exposes the interface a transport driver
 * uses to hand the pages to the VMM (see <linux/pvsched_guest.h>).
 * The core is built in because its hooks attach to unexported scheduler and
 * interrupt tracepoints and to the hardirq entry path.
 *
 * The host advertises the policy through the transport; the core echoes it
 * into each page's negotiation header, subject to the guest administrator's
 * acceptance list (pvsched_guest.allow=).  Acceptance concerns semantics and
 * overhead, not security: the host already controls vCPU scheduling.
 *
 * Published words, all whole aligned 64-bit stores (see
 * Documentation/virt/kvm/pvsched-v3.rst):
 *
 * - task_intent: the current task, and an optional more urgent pending
 *   (woken) task.  Every writer holds the target CPU's runqueue lock:
 *   sched_switch publishes the incoming task and clears the hint;
 *   sched_wakeup publishes a hint on the woken task's CPU when it is more
 *   urgent than both that CPU's current task and any earlier hint.
 * - cs_state: this CPU's NMI/hardirq/softirq state, written only by this
 *   CPU.  Hooks right after every hardirq and softirq count change recompute
 *   it from the preempt count, so a nested writer cannot leave a stale bit.
 * - interrupt_ack: in a hardirq, after cs_state, echo the host's
 *   outstanding interrupt ticket so the host knows the handoff arrived.
 *
 * The host boosts lazily at its own checkpoints.  Deboosts are signalled
 * promptly when the host sets PVSCHED_HINT_KICK_DEBOOST: the transport's kick
 * forces an exit so the host re-evaluates without waiting for a natural one.
 * The guest kicks when its most urgent published intent drops at a switch,
 * and when the last CS level ends while the host still applies a CS boost
 * that no published RT or deadline task needs, except on the way to idle:
 * the halt that follows is itself a host checkpoint.
 *
 * Not yet covered: NMI entry, preemption-disabled sections, a priority change
 * of the running task without a switch, pending hints of migrated tasks or
 * of a task that stays current, new tasks (sched_wakeup_new), and the idle
 * task, which publishes as NORMAL nice 0.
 *
 * Hooks run only after the host enabled at least one vCPU, and each checks its
 * own page's status, so a guest with this support runs unchanged elsewhere.
 * Pages are allocated once and never freed, so a renegotiation reuses them
 * and no hook can see a freed page.
 */

#define pr_fmt(fmt) "pvsched_guest: " fmt

#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pvsched_guest.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <trace/events/sched.h>
#include <uapi/linux/pvsched.h>

/* The guest administrator's acceptance list; "*" accepts any policy. */
static char pvsched_allow[256] = "*";
module_param_string(allow, pvsched_allow, sizeof(pvsched_allow), 0444);

static DEFINE_PER_CPU(union pvsched_vcpu_page *, pvsched_page);
static DEFINE_PER_CPU(u64, pvsched_kicks);
static DEFINE_PER_CPU(u64, pvsched_acks);

DEFINE_STATIC_KEY_FALSE(pvsched_guest_irq_key);

/* Serializes prepare, commit and teardown (transport bind/unbind). */
static DEFINE_MUTEX(pvsched_guest_mutex);
static bool pvsched_publishing;
/* The CPUs whose pages the last prepare reset: the set handed to the VMM. */
static struct cpumask pvsched_prepared_cpus;

static char pvsched_policy[PVSCHED_NAME_MAX];
static u32 pvsched_policy_version;
static void (*pvsched_kick)(void);

static bool pvsched_guest_active(const union pvsched_vcpu_page *page)
{
	/*
	 * Acquire: the host wrote its response and feedback before releasing
	 * this status.
	 */
	return page && le32_to_cpu(smp_load_acquire(&page->header.status)) ==
		       PVSCHED_STATUS_ENABLED;
}

/* Encode @p as a valid default-protocol task description. */
static struct pvsched_prio_desc pvsched_guest_desc(const struct task_struct *p)
{
	struct pvsched_prio_desc desc = { };

	switch (p->policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		if (p->rt_priority >= 1 && p->rt_priority < MAX_RT_PRIO) {
			desc.sched_policy = p->policy;
			desc.rt_prio = p->rt_priority;
			return desc;
		}
		break;
	case SCHED_IDLE:
	case SCHED_DEADLINE:
		desc.sched_policy = p->policy;
		return desc;
	case SCHED_BATCH:
		desc.sched_policy = SCHED_BATCH;
		desc.nice = task_nice(p);
		return desc;
	}
	/* NORMAL, and anything the protocol cannot express (such as EXT). */
	desc.sched_policy = SCHED_NORMAL;
	desc.nice = task_nice(p);
	return desc;
}

/* Kernel-style urgency: a lower value is more urgent. */
static int pvsched_guest_urgency(const struct pvsched_prio_desc *desc)
{
	switch (desc->sched_policy) {
	case SCHED_DEADLINE:
		return 0;
	case SCHED_FIFO:
	case SCHED_RR:
		return MAX_RT_PRIO - desc->rt_prio;
	case SCHED_IDLE:
		return MAX_PRIO;
	default:
		return NICE_TO_PRIO(desc->nice);
	}
}

/* Urgency of the most urgent task described by @intent. */
static int pvsched_guest_best(const union pvsched_task_intent *intent)
{
	int best = pvsched_guest_urgency(&intent->current_task);

	if (intent->flags & PVSCHED_INTENT_FLAG_PENDING_VALID)
		best = min(best, pvsched_guest_urgency(&intent->pending_task));
	return best;
}

static void pvsched_guest_do_kick(void)
{
	void (*kick)(void) = READ_ONCE(pvsched_kick);

	if (kick) {
		__this_cpu_inc(pvsched_kicks);
		kick();
	}
}

/* sched_switch: under this CPU's runqueue lock, with IRQs disabled. */
static void pvsched_guest_switch_probe(void *data, bool preempt,
				       struct task_struct *prev,
				       struct task_struct *next,
				       unsigned int prev_state)
{
	union pvsched_vcpu_page *page = __this_cpu_read(pvsched_page);
	union pvsched_task_intent old, new = { };
	union pvsched_applied_state applied;

	if (!pvsched_guest_active(page))
		return;
	old.raw = READ_ONCE(page->guest_area.default_area.task_intent.raw);
	new.current_task = pvsched_guest_desc(next);
	if (is_idle_task(next))
		new.flags = PVSCHED_INTENT_FLAG_IDLE;
	WRITE_ONCE(page->guest_area.default_area.task_intent.raw, new.raw);

	/*
	 * Publish first: the host samples the page at the forced exit.  A switch
	 * to idle needs no kick: the vCPU is about to halt, which the host
	 * already re-evaluates, and a deboost just before it would let host
	 * tasks preempt the vCPU on its way to the halt.
	 */
	if ((new.flags & PVSCHED_INTENT_FLAG_IDLE) ||
	    pvsched_guest_best(&new) <= pvsched_guest_best(&old))
		return;
	applied.raw = READ_ONCE(page->host_area.applied_state.raw);
	if (applied.hints & PVSCHED_HINT_KICK_DEBOOST)
		pvsched_guest_do_kick();
}

/* sched_wakeup: under the woken task's runqueue lock. */
static void pvsched_guest_wakeup_probe(void *data, struct task_struct *p)
{
	union pvsched_vcpu_page *page = per_cpu(pvsched_page, task_cpu(p));
	union pvsched_task_intent intent;
	struct pvsched_prio_desc desc;
	int urgency;

	/* A running task waking itself holds no runqueue lock. */
	if (p == current || !pvsched_guest_active(page))
		return;
	intent.raw = READ_ONCE(page->guest_area.default_area.task_intent.raw);
	desc = pvsched_guest_desc(p);
	urgency = pvsched_guest_urgency(&desc);
	if (urgency >= pvsched_guest_urgency(&intent.current_task))
		return;
	if ((intent.flags & PVSCHED_INTENT_FLAG_PENDING_VALID) &&
	    urgency >= pvsched_guest_urgency(&intent.pending_task))
		return;
	intent.pending_task = desc;
	intent.flags |= PVSCHED_INTENT_FLAG_PENDING_VALID;
	WRITE_ONCE(page->guest_area.default_area.task_intent.raw, intent.raw);
}

static u64 pvsched_guest_cs_now(void)
{
	u64 cs = 0;

	if (in_nmi())
		cs |= PVSCHED_CS_NMI;
	if (in_hardirq())
		cs |= PVSCHED_CS_HARDIRQ;
	if (in_serving_softirq())
		cs |= PVSCHED_CS_SOFTIRQ;
	return cs;
}

static void pvsched_guest_publish_cs(union pvsched_vcpu_page *page, u64 cs)
{
	WRITE_ONCE(page->guest_area.default_area.cs_state, cpu_to_le64(cs));
}

/* Echo the host's outstanding interrupt ticket, once cs_state is published. */
static void pvsched_guest_ack(union pvsched_vcpu_page *page)
{
	__le64 ticket;

	ticket = READ_ONCE(page->host_area.default_area.interrupt_ticket);
	if (ticket && ticket !=
	    READ_ONCE(page->guest_area.default_area.interrupt_ack)) {
		/* The host acquire-reads this, then expects the CS state above. */
		smp_store_release(&page->guest_area.default_area.interrupt_ack,
				  ticket);
		__this_cpu_inc(pvsched_acks);
	}
}

/*
 * Called after every hardirq entry and exit and softirq begin and end, once
 * the preempt count reflects the change.
 */
void __pvsched_guest_cs_update(void)
{
	union pvsched_vcpu_page *page = __this_cpu_read(pvsched_page);
	union pvsched_applied_state applied;
	union pvsched_task_intent intent;
	u64 cs;

	if (!pvsched_guest_active(page))
		return;
	cs = pvsched_guest_cs_now();
	pvsched_guest_publish_cs(page, cs);

	/*
	 * Softirqs never begin or end inside a hardirq, so this is a hardirq
	 * entry or a nested hardirq's exit.  An ack at the latter is harmless:
	 * cs_state was just published with the hardirq bit set, before the
	 * ack's release store, so the host sees the ticket answered with the
	 * vCPU in hardirq context, as it is.
	 */
	if (cs & PVSCHED_CS_HARDIRQ) {
		pvsched_guest_ack(page);
		return;
	}

	/*
	 * Pending softirqs run next, so CS has not ended yet.  The idle task
	 * halts next, which the host re-evaluates without a kick.
	 */
	if (cs || local_softirq_pending() || is_idle_task(current))
		return;

	/* The last CS level ended: tell a host still boosting for it. */
	applied.raw = READ_ONCE(page->host_area.applied_state.raw);
	intent.raw = READ_ONCE(page->guest_area.default_area.task_intent.raw);
	if ((applied.hints & PVSCHED_HINT_KICK_DEBOOST) &&
	    applied.boost == PVSCHED_BOOST_CS &&
	    pvsched_guest_best(&intent) >= MAX_RT_PRIO)
		pvsched_guest_do_kick();
}

static int pvsched_guest_start(void)
{
	int ret;

	ret = register_trace_sched_switch(pvsched_guest_switch_probe, NULL);
	if (ret)
		return ret;
	ret = register_trace_sched_wakeup(pvsched_guest_wakeup_probe, NULL);
	if (ret) {
		unregister_trace_sched_switch(pvsched_guest_switch_probe, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}
	static_branch_enable(&pvsched_guest_irq_key);
	return 0;
}

static void pvsched_guest_stop(void)
{
	static_branch_disable(&pvsched_guest_irq_key);
	unregister_trace_sched_wakeup(pvsched_guest_wakeup_probe, NULL);
	unregister_trace_sched_switch(pvsched_guest_switch_probe, NULL);
	tracepoint_synchronize_unregister();
	/*
	 * The interrupt-context hooks run with IRQs disabled; wait for any that
	 * saw the key enabled, so none can still call the transport's kick.
	 */
	synchronize_rcu();
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

/* Start a clean negotiation: fresh header, zero guest and host areas. */
static void pvsched_guest_reset_page(union pvsched_vcpu_page *page)
{
	struct pvsched_header *header = &page->header;

	clear_page(page);
	header->abi_version = cpu_to_le32(PVSCHED_ABI_VERSION);
	header->policy_version = cpu_to_le32(pvsched_policy_version);
	strscpy_pad(header->policy_name, pvsched_policy,
		    sizeof(header->policy_name));
	header->protocol_id = cpu_to_le32(PVSCHED_PROTOCOL_DEFAULT);
	header->requested_mode = cpu_to_le32(PVSCHED_MODE_FRAMEWORK);
}

int pvsched_guest_prepare(const char *policy, u32 policy_version,
			  void (*kick)(void))
{
	unsigned int cpu;
	int ret = 0;

	/* The shared-page stride is one guest page (x86-64 only for now). */
	BUILD_BUG_ON(PVSCHED_VCPU_STRIDE != PAGE_SIZE);

	if (!policy || !policy[0] ||
	    strnlen(policy, PVSCHED_NAME_MAX) >= PVSCHED_NAME_MAX)
		return -EINVAL;

	mutex_lock(&pvsched_guest_mutex);
	if (pvsched_publishing) {
		ret = -EBUSY;
		goto out;
	}
	if (!pvsched_guest_allowed(policy, policy_version)) {
		pr_info("advertised policy '%s' v%u not accepted (pvsched_guest.allow=%s)\n",
			policy, policy_version, pvsched_allow);
		ret = -EPERM;
		goto out;
	}
	strscpy_pad(pvsched_policy, policy, sizeof(pvsched_policy));
	pvsched_policy_version = policy_version;
	WRITE_ONCE(pvsched_kick, kick);

	cpumask_clear(&pvsched_prepared_cpus);
	for_each_present_cpu(cpu) {
		union pvsched_vcpu_page *page = per_cpu(pvsched_page, cpu);

		if (!page) {
			page = (void *)get_zeroed_page(GFP_KERNEL);
			if (!page) {
				ret = -ENOMEM;
				goto out;
			}
			per_cpu(pvsched_page, cpu) = page;
		}
		pvsched_guest_reset_page(page);
		cpumask_set_cpu(cpu, &pvsched_prepared_cpus);
	}
out:
	mutex_unlock(&pvsched_guest_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(pvsched_guest_prepare);

/*
 * The guest-physical address of @cpu's page, or 0 if the last prepare did
 * not reset it: a CPU that became present after prepare may still hold a
 * page from an earlier prepare, with a stale header.  A CPU removed after
 * prepare keeps its address, as pages are never freed.  Only valid between
 * a successful prepare and the commit that follows it.
 */
phys_addr_t pvsched_guest_page_gpa(unsigned int cpu)
{
	if (!cpumask_test_cpu(cpu, &pvsched_prepared_cpus))
		return 0;
	return virt_to_phys(per_cpu(pvsched_page, cpu));
}
EXPORT_SYMBOL_GPL(pvsched_guest_page_gpa);

int pvsched_guest_commit(void)
{
	unsigned int cpu, enabled = 0, prepared;
	int ret;

	mutex_lock(&pvsched_guest_mutex);
	prepared = cpumask_weight(&pvsched_prepared_cpus);
	for_each_present_cpu(cpu)
		enabled += pvsched_guest_active(per_cpu(pvsched_page, cpu));
	if (enabled && !pvsched_publishing) {
		ret = pvsched_guest_start();
		if (ret) {
			mutex_unlock(&pvsched_guest_mutex);
			return ret;
		}
		pvsched_publishing = true;
	}
	mutex_unlock(&pvsched_guest_mutex);

	pr_info("policy '%s' v%u: %u/%u vCPUs enabled\n", pvsched_policy,
		pvsched_policy_version, enabled, prepared);
	return enabled;
}
EXPORT_SYMBOL_GPL(pvsched_guest_commit);

void pvsched_guest_teardown(void)
{
	mutex_lock(&pvsched_guest_mutex);
	if (pvsched_publishing) {
		pvsched_guest_stop();
		pvsched_publishing = false;
	}
	/* No hook runs past the stop's synchronization. */
	WRITE_ONCE(pvsched_kick, NULL);
	mutex_unlock(&pvsched_guest_mutex);
}
EXPORT_SYMBOL_GPL(pvsched_guest_teardown);

static int pvsched_guest_state_show(struct seq_file *m, void *v)
{
	unsigned int cpu;

	seq_puts(m, "cpu status intent cs_state ack hints flags boost ticket kicks acks\n");
	for_each_present_cpu(cpu) {
		union pvsched_vcpu_page *page = per_cpu(pvsched_page, cpu);
		struct pvsched_default_guest_area *ga;
		union pvsched_applied_state applied;

		if (!page)
			continue;
		ga = &page->guest_area.default_area;
		applied.raw = READ_ONCE(page->host_area.applied_state.raw);
		seq_printf(m, "%u %u %016llx %llx %llx %x %x %u %llx %llu %llu\n",
			   cpu, le32_to_cpu(READ_ONCE(page->header.status)),
			   le64_to_cpu(READ_ONCE(ga->task_intent.raw)),
			   le64_to_cpu(READ_ONCE(ga->cs_state)),
			   le64_to_cpu(READ_ONCE(ga->interrupt_ack)),
			   applied.hints, applied.flags, applied.boost,
			   le64_to_cpu(READ_ONCE(page->host_area.default_area.interrupt_ticket)),
			   per_cpu(pvsched_kicks, cpu), per_cpu(pvsched_acks, cpu));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pvsched_guest_state);

static int __init pvsched_guest_init(void)
{
	struct dentry *dir = debugfs_create_dir("pvsched_guest", NULL);

	debugfs_create_file("state", 0444, dir, NULL, &pvsched_guest_state_fops);
	return 0;
}
late_initcall(pvsched_guest_init);
