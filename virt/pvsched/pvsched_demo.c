// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Google */

/*
 * Paravirt scheduling - demo policy module.
 *
 * A minimal in-tree pvsched policy that exercises the host framework end to
 * end, implementing a miniature of the latency policy on the framework's
 * callbacks:
 *
 *   vmexit  (vCPU thread ctx)  read the guest's requested scheduling
 *                              parameters from guest_area, apply them to the
 *                              vCPU thread via pvsched_set_params() and
 *                              publish the outcome in host_area.
 *   vmentry (vCPU thread ctx)  publish the VMENTERED status; must stay cheap,
 *                              the call site runs with IRQs disabled.
 *   inject  (injector ctx)     boost the vCPU thread so it gets on a CPU to
 *                              deliver the interrupt promptly, and flag
 *                              INTR_INJECTED in host_area -- the framework's
 *                              pinned kernel mapping is usable from any
 *                              context, including this one.
 *   halt                       counted only; kept as an extension point.
 *
 * The meaningful 4 bytes of guest_area and host_area are accessed as one
 * aligned 32-bit word, so neither side sees a torn {status, params} pair; the
 * status-byte updates in vmentry/inject are best-effort read-modify-writes (a
 * racing word publish wins -- a real policy would cmpxchg the word). Counters
 * are exposed at debugfs pvsched_demo/stats for tests to assert against.
 */

#define pr_fmt(fmt) "pvsched_demo: " fmt

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/debugfs.h>
#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pid.h>
#include <linux/pvsched.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/stddef.h>

#include <uapi/linux/pvsched.h>

/* RT priority of the boost applied on interrupt injection; 0 disables it. */
static unsigned int demo_inject_rt_prio = 50;
module_param(demo_inject_rt_prio, uint, 0644);

enum demo_stat {
	DEMO_STAT_VMENTRY,	/* vmentry callbacks */
	DEMO_STAT_VMEXIT,	/* vmexit callbacks */
	DEMO_STAT_HALT,		/* halt callbacks */
	DEMO_STAT_INJECT,	/* inject callbacks */
	DEMO_STAT_APPLIED,	/* pvsched_set_params() applied a request */
	DEMO_STAT_THROTTLED,	/* pvsched_set_params() refused a boost (cap) */
	DEMO_STAT_SET_ERR,	/* pvsched_set_params() returned an error */
	DEMO_STAT_INTR_PUB,	/* INTR_INJECTED published to host_area */
	DEMO_STAT_KERNCS,	/* vmexits that saw guest kern_cs bits set */
	DEMO_STAT_NO_SHM,	/* a callback ran with no shared page (must not happen) */
	DEMO_STAT_NR,
};

static const char * const demo_stat_names[DEMO_STAT_NR] = {
	"vmentry", "vmexit", "halt", "inject",
	"applied", "throttled", "set_err", "intr_pub", "kern_cs", "no_shm",
};

static atomic64_t demo_stats[DEMO_STAT_NR];

/* The meaningful first 4 bytes of an area, as one 32-bit word. */
union demo_guest_word {
	struct pvsched_guest_area ga;
	u32 word;
};

union demo_host_word {
	struct pvsched_host_area ha;
	u32 word;
};

static void demo_count_set(int ret)
{
	if (ret < 0)
		atomic64_inc(&demo_stats[DEMO_STAT_SET_ERR]);
	else if (ret == 1)
		atomic64_inc(&demo_stats[DEMO_STAT_THROTTLED]);
	else
		atomic64_inc(&demo_stats[DEMO_STAT_APPLIED]);
}

/*
 * vmexit: apply the guest's request to the vCPU thread (current) and publish
 * the outcome as one aligned 32-bit store. The fresh publish also clears a
 * pending INTR_INJECTED flag: the guest just ran, so the interrupt that flag
 * announced has been delivered.
 */
static void demo_vmexit(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm)
{
	union demo_guest_word req;
	union demo_host_word pub = { };
	int ret;

	atomic64_inc(&demo_stats[DEMO_STAT_VMEXIT]);
	if (!shm) {
		atomic64_inc(&demo_stats[DEMO_STAT_NO_SHM]);
		return;
	}

	req.word = READ_ONCE(*(u32 *)&shm->guest_area);
	if (req.ga.kern_cs)
		atomic64_inc(&demo_stats[DEMO_STAT_KERNCS]);

	ret = pvsched_set_params(task_pid_vnr(current), req.ga.sched_policy,
				 req.ga.nice, req.ga.rt_prio);
	demo_count_set(ret);
	if (ret < 0)
		return;

	pub.ha.vcpu_status = PVSCHED_VCPU_STATUS_VMEXITED;
	if (ret == 1) {
		/* Throttled: the task is held at baseline. */
		pub.ha.sched_policy = SCHED_NORMAL;
	} else {
		pub.ha.sched_policy	= req.ga.sched_policy;
		pub.ha.nice		= req.ga.nice;
		pub.ha.rt_prio		= req.ga.rt_prio;
	}
	WRITE_ONCE(*(u32 *)&shm->host_area, pub.word);
	/* This policy boosts: ask the guest for prompt deboost notification. */
	WRITE_ONCE(shm->host_area.hints, PVSCHED_HINT_KICK_DEBOOST);
}

/* vmentry: flip the status byte, preserving a pending INTR_INJECTED flag the
 * guest has not had a chance to read yet. One store; the site is IRQs-off. */
static void demo_vmentry(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm)
{
	u8 status;

	atomic64_inc(&demo_stats[DEMO_STAT_VMENTRY]);
	if (!shm) {
		atomic64_inc(&demo_stats[DEMO_STAT_NO_SHM]);
		return;
	}

	status = READ_ONCE(shm->host_area.vcpu_status);
	status = (status & ~PVSCHED_VCPU_STATUS_MASK) |
		 PVSCHED_VCPU_STATUS_VMENTERED;
	WRITE_ONCE(shm->host_area.vcpu_status, status);
}

static void demo_halt(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm)
{
	atomic64_inc(&demo_stats[DEMO_STAT_HALT]);
}

/*
 * inject: runs in the injector's context, not the vCPU thread. Flag the
 * injection in host_area and boost the vCPU thread to deliver the interrupt.
 * The dispatch probe holds rcu_read_lock(), which protects vcpu->pid; the pid
 * is resolved in the caller's namespace, consistent with pvsched_set_params().
 */
static void demo_inject(struct kvm_vcpu *vcpu, union pvsched_vcpu_page *shm)
{
	struct pid *pid;
	pid_t nr;

	atomic64_inc(&demo_stats[DEMO_STAT_INJECT]);

	if (shm) {
		u8 status = READ_ONCE(shm->host_area.vcpu_status);

		WRITE_ONCE(shm->host_area.vcpu_status,
			   status | PVSCHED_VCPU_STATUS_INTR_INJECTED);
		atomic64_inc(&demo_stats[DEMO_STAT_INTR_PUB]);
	} else {
		atomic64_inc(&demo_stats[DEMO_STAT_NO_SHM]);
	}

	if (!demo_inject_rt_prio)
		return;

	pid = rcu_dereference(vcpu->pid);
	if (!pid)
		return;
	nr = pid_vnr(pid);
	if (!nr)
		return;

	demo_count_set(pvsched_set_params(nr, SCHED_FIFO, 0,
					  demo_inject_rt_prio));
}

static struct pvsched_policy_ops demo_ops = {
	.name		= "demo",
	.abi_version	= PVSCHED_ABI_VERSION,
	.version	= 1,
	.owner		= THIS_MODULE,
	.vmentry	= demo_vmentry,
	.vmexit		= demo_vmexit,
	.halt		= demo_halt,
	.inject		= demo_inject,
};

static int demo_stats_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < DEMO_STAT_NR; i++)
		seq_printf(m, "%s %lld\n", demo_stat_names[i],
			   (long long)atomic64_read(&demo_stats[i]));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(demo_stats);

static struct dentry *demo_debugfs;

static int __init pvsched_demo_init(void)
{
	int ret;

	/* The word accesses above cover exactly the meaningful bytes. */
	BUILD_BUG_ON(offsetof(struct pvsched_guest_area, rt_prio) != 3);
	BUILD_BUG_ON(offsetof(struct pvsched_host_area, rt_prio) != 3);

	demo_debugfs = debugfs_create_dir("pvsched_demo", NULL);
	debugfs_create_file("stats", 0444, demo_debugfs, NULL,
			    &demo_stats_fops);

	ret = pvsched_register_policy(&demo_ops);
	if (ret)
		debugfs_remove_recursive(demo_debugfs);
	return ret;
}

static void __exit pvsched_demo_exit(void)
{
	pvsched_unregister_policy(&demo_ops);
	debugfs_remove_recursive(demo_debugfs);
}

module_init(pvsched_demo_init);
module_exit(pvsched_demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Paravirt scheduling demo policy");
