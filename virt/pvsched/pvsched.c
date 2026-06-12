// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Google */

/*
 * Paravirt scheduling - host framework module.
 *
 * The framework is the central manager: policy modules (kernel modules or, later,
 * BPF struct_ops) register a named set of callbacks here; the VMM registers each
 * guest vCPU's shared page and the policy it wants (/dev/pvsched); and the
 * framework dispatches the vCPU's scheduling events to the matching policy's
 * callbacks.
 */

#define pr_fmt(fmt) "pvsched: " fmt

#include <linux/atomic.h>
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/build_bug.h>
#include <linux/capability.h>
#include <linux/debugfs.h>
#include <linux/hashtable.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/pvsched.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/prio.h>
#include <linux/sched/task.h>
#include <uapi/linux/sched/types.h>		/* struct sched_attr */
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <trace/events/kvm.h>
#include <trace/events/sched.h>

#include <uapi/linux/pvsched.h>

/*
 * DoS cap tunables, in nanoseconds (see /sys/module/pvsched/parameters/). A
 * boost may last at most @pvsched_max_boost_ns; a task that exceeds it is forced
 * back to baseline and may not be re-boosted for @pvsched_throttle_ns.
 */
static unsigned long pvsched_max_boost_ns = NSEC_PER_SEC;
static unsigned long pvsched_throttle_ns  = NSEC_PER_SEC;
module_param(pvsched_max_boost_ns, ulong, 0644);
module_param(pvsched_throttle_ns, ulong, 0644);

/* Per-task state: a hash keyed by task_struct *. */

enum pvsched_state {
	PVSCHED_NONE = 0,	/* not boosted */
	PVSCHED_BOOSTED,	/* boosted; cap timer armed */
	PVSCHED_THROTTLED,	/* capped; in cooldown, boosts refused */
};

/*
 * Per-task pvsched state. Created on first use (vCPU registration) and removed
 * when the task exits (the sched_process_exit probe) or the module unloads.
 * Holds a reference on @task so the pointer is always valid (the cap timer and
 * module teardown dereferences it).
 */
struct pvsched_task {
	struct hlist_node		hash;	/* pvsched_ht linkage (RCU) */
	struct task_struct		*task;	/* key; a ref is held */
	struct hrtimer			timer;
	atomic_t			state;
	const struct pvsched_policy_ops	*policy; /* policy bound to vCPU */
	struct page			*shm_page;	/* pinned shared page, or NULL */
	union pvsched_vcpu_page		*shm;		/* its kernel mapping */
	struct llist_node		teardown;
	struct rcu_head			rcu;
};

static DEFINE_HASHTABLE(pvsched_ht, 8);
static DEFINE_SPINLOCK(pvsched_ht_lock);	/* serializes hash add/del + binds */

/* Lookup, valid under rcu_read_lock() or while holding pvsched_ht_lock. */
static struct pvsched_task *pvsched_task_find(struct task_struct *t)
{
	struct pvsched_task *pt;

	hash_for_each_possible_rcu(pvsched_ht, pt, hash, (unsigned long)t,
				   lockdep_is_held(&pvsched_ht_lock))
		if (pt->task == t)
			return pt;
	return NULL;
}

static enum hrtimer_restart pvsched_cap_timer(struct hrtimer *timer);
static void pvsched_set_baseline(struct task_struct *p);

/*
 * Allocate an unlinked per-task state (sleepable; only the registration ioctl
 * creates entries). Takes a reference on @t that the entry owns.
 */
static struct pvsched_task *pvsched_task_alloc(struct task_struct *t)
{
	struct pvsched_task *pt = kzalloc(sizeof(*pt), GFP_KERNEL);

	if (pt) {
		hrtimer_setup(&pt->timer, pvsched_cap_timer, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_HARD);
		atomic_set(&pt->state, PVSCHED_NONE);
		pt->task = t;
		get_task_struct(t);
	}
	return pt;
}

/* Drop a binding's policy reference and page pin (no RCU grace period). */
static void pvsched_put_binding(const struct pvsched_policy_ops *policy,
				struct page *page)
{
	if (policy)
		bpf_module_put(policy, policy->owner);
	if (page)
		unpin_user_pages_dirty_lock(&page, 1, true);
}

static void pvsched_task_free(struct pvsched_task *pt)
{
	pvsched_put_binding(pt->policy, pt->shm_page);
	put_task_struct(pt->task);
	kfree(pt);
}

/*
 * Safety-net teardown: a task may exit without the VMM unregistering it. The
 * sched_process_exit probe runs preempt-disabled (cannot sleep), so it only
 * unhashes the entry and defers the sleepable teardown to a workqueue.
 */

static LLIST_HEAD(pvsched_teardown_list);

static void pvsched_teardown_fn(struct work_struct *w)
{
	struct llist_node *batch = llist_del_all(&pvsched_teardown_list);
	struct pvsched_task *pt, *n;

	if (!batch)
		return;

	/*
	 * One grace period covers all in-flight dispatch readers. The entries
	 * were unhashed before being queued here, so after it nothing can find
	 * them and no new boost can arm a cap timer: the cancel below is final.
	 */
	synchronize_rcu();

	llist_for_each_entry_safe(pt, n, batch, teardown) {
		hrtimer_cancel(&pt->timer);
		pvsched_task_free(pt);
	}
}
static DECLARE_WORK(pvsched_teardown_work, pvsched_teardown_fn);

/* sched_process_exit probe: detach the entry and queue its teardown. */
static void pvsched_sched_exit_probe(void *data, struct task_struct *t,
				     bool group_dead)
{
	struct pvsched_task *pt;
	unsigned long flags;

	spin_lock_irqsave(&pvsched_ht_lock, flags);
	pt = pvsched_task_find(t);
	if (pt)
		hash_del_rcu(&pt->hash);
	spin_unlock_irqrestore(&pvsched_ht_lock, flags);

	if (pt) {
		llist_add(&pt->teardown, &pvsched_teardown_list);
		schedule_work(&pvsched_teardown_work);
	}
}

/*
 * Force a task back to baseline. Baseline is hardcoded SCHED_NORMAL / nice 0,
 * which is right when the VMM runs vCPU threads at default priority (the common
 * case): restoring NORMAL/0 restores their true baseline, and it self-heals a
 * boost left stuck by a crashed or unloaded session. A vCPU the VMM placed at a
 * non-default policy/nice is reset to default here on throttle/unregister.
 *
 * TODO: Before upstream: snapshot the thread's sched params at registration and
 * restore those instead of a fixed baseline (v2 carried baseline handling of
 * this kind -- recover it from the v2 PoC).
 */
static void pvsched_set_baseline(struct task_struct *p)
{
	struct sched_attr attr = {
		.size		= sizeof(attr),
		.sched_policy	= SCHED_NORMAL,
	};

	sched_setattr_pi_nocheck(p, &attr, false);
}

/*
 * Cap timer, run in hard IRQ context so it cannot be starved by the boosted
 * task itself. First expiry (BOOSTED): the boost outlived its budget, force the
 * task to baseline and re-arm for the throttle cooldown. Second expiry
 * (THROTTLED): cooldown elapsed, allow boosting again. Both transitions are
 * cmpxchg, so a concurrent deboost simply wins and this becomes a no-op.
 */
static enum hrtimer_restart pvsched_cap_timer(struct hrtimer *timer)
{
	struct pvsched_task *pt = container_of(timer, struct pvsched_task, timer);

	switch (atomic_cmpxchg(&pt->state, PVSCHED_BOOSTED, PVSCHED_THROTTLED)) {
	case PVSCHED_BOOSTED:
		pvsched_set_baseline(pt->task);
		hrtimer_forward_now(timer, ns_to_ktime(pvsched_throttle_ns));
		return HRTIMER_RESTART;
	case PVSCHED_THROTTLED:
		/* Cooldown expiry; only this timer clears THROTTLED. */
		WARN_ON_ONCE(atomic_cmpxchg(&pt->state, PVSCHED_THROTTLED,
					    PVSCHED_NONE) != PVSCHED_THROTTLED);
		break;
	default:
		/* NONE: a concurrent deboost won the first expiry's race. */
		break;
	}
	return HRTIMER_NORESTART;
}

/*
 * A request is a "boost" (subject to the cap) if it raises priority above
 * baseline: any RT policy, or a fair policy with negative nice. nice is already
 * clamped to [MIN_NICE, MAX_NICE] by the caller.
 */
static bool pvsched_is_boost(u32 policy, s32 nice)
{
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		return true;
	case SCHED_NORMAL:
	case SCHED_BATCH:
		return nice < 0;
	default:
		return false;
	}
}

/**
 * pvsched_set_params - set a vCPU thread's scheduling parameters
 * @pid: pid of the vCPU thread, in the caller's pid namespace
 * @policy: SCHED_* policy to apply (SCHED_DEADLINE is not supported)
 * @nice: nice value, for fair policies
 * @rt_prio: real-time priority, for SCHED_FIFO / SCHED_RR
 *
 * Called from a pvsched policy's callbacks: exported for policy modules, and
 * registered as a kfunc for BPF policies. The change is applied with pi=false
 * so it is safe from the interrupt context of the injection event; SCHED_DEADLINE
 * is rejected because its setup can sleep. A boost (an RT policy, or a fair
 * policy with negative nice) is bounded by the per-task cap.
 *
 * Return: 0 if applied; 1 if throttled (held at baseline, boost refused); or a
 * negative error (-ESRCH, -EOPNOTSUPP, -EINVAL, -ENOMEM, or a scheduler error).
 */
__bpf_kfunc int pvsched_set_params(s32 pid, u32 policy, s32 nice, u32 rt_prio)
{
	struct sched_attr attr = {
		.size		= sizeof(attr),
		.sched_policy	= policy,
		.sched_priority	= rt_prio,
	};
	struct task_struct *p;
	struct pvsched_task *pt;
	bool boost;
	int ret;

	switch (policy) {
	case SCHED_NORMAL:
	case SCHED_FIFO:
	case SCHED_RR:
	case SCHED_BATCH:
	case SCHED_IDLE:
		break;
	case SCHED_DEADLINE:
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}

	/* __sched_setscheduler() does not clamp nice for fair policies. */
	attr.sched_nice = clamp_t(s32, nice, MIN_NICE, MAX_NICE);

	p = find_get_task_by_vpid(pid);
	if (!p)
		return -ESRCH;

	boost = pvsched_is_boost(policy, attr.sched_nice);

	/*
	 * The rcu_read_lock() pins the entry across use (a free waits out a
	 * grace period after unhashing). No entry: not a vCPU we track.
	 */
	rcu_read_lock();
	pt = pvsched_task_find(p);
	if (!pt) {
		ret = -ESRCH;
		goto out;
	}

	if (boost) {
		if (atomic_read(&pt->state) == PVSCHED_THROTTLED) {
			pvsched_set_baseline(p);
			ret = 1;
			goto out;
		}
		ret = sched_setattr_pi_nocheck(p, &attr, false);
		/*
		 * Apply-then-claim. The boost is applied above; now claim the cap
		 * state to decide what it meant. This block must stay AFTER the
		 * apply: on a lost race its set_baseline() has to be the last write
		 * to the task's priority.
		 *   NONE      first boost -- arm the cap timer.
		 *   THROTTLED the cap timer threw us out between the check above and
		 *             here -- undo the boost we just applied.
		 *   BOOSTED   already capped (a re-boost) -- nothing to do; a
		 *             concurrent throttle's set_baseline() is ordered after
		 *             our apply by the cmpxchg barrier, so it still wins.
		 */
		if (!ret) {
			switch (atomic_cmpxchg(&pt->state, PVSCHED_NONE,
					       PVSCHED_BOOSTED)) {
			case PVSCHED_NONE:
				hrtimer_start(&pt->timer,
					      ns_to_ktime(pvsched_max_boost_ns),
					      HRTIMER_MODE_REL_HARD);
				break;
			case PVSCHED_THROTTLED:
				pvsched_set_baseline(p);
				ret = 1;
				break;
			}
		}
	} else {
		/*
		 * Claim BOOSTED -> NONE before applying, so a cap timer firing
		 * concurrently loses its cmpxchg and skips its own deboost.
		 */
		if (atomic_cmpxchg(&pt->state, PVSCHED_BOOSTED,
				   PVSCHED_NONE) == PVSCHED_BOOSTED)
			hrtimer_try_to_cancel(&pt->timer);
		ret = sched_setattr_pi_nocheck(p, &attr, false);
	}
out:
	rcu_read_unlock();
	put_task_struct(p);
	return ret;
}
EXPORT_SYMBOL_GPL(pvsched_set_params);

BTF_KFUNCS_START(pvsched_kfunc_ids)
BTF_ID_FLAGS(func, pvsched_set_params)
BTF_KFUNCS_END(pvsched_kfunc_ids)

static const struct btf_kfunc_id_set pvsched_kfunc_set = {
	.owner	= THIS_MODULE,
	.set	= &pvsched_kfunc_ids,
};

static LIST_HEAD(pvsched_policies);
static DEFINE_MUTEX(pvsched_mutex);

int pvsched_register_policy(struct pvsched_policy_ops *ops)
{
	struct pvsched_policy_ops *p;

	if (!ops->name[0] || !ops->vmentry || !ops->vmexit ||
	    !ops->halt || !ops->inject)
		return -EINVAL;
	if (ops->abi_version != PVSCHED_ABI_VERSION)
		return -EINVAL;

	mutex_lock(&pvsched_mutex);
	list_for_each_entry(p, &pvsched_policies, list) {
		if (!strncmp(p->name, ops->name, PVSCHED_NAME_MAX) &&
		    p->version == ops->version) {
			mutex_unlock(&pvsched_mutex);
			return -EEXIST;
		}
	}
	list_add_tail(&ops->list, &pvsched_policies);
	mutex_unlock(&pvsched_mutex);

	pr_info("registered policy '%s' (abi %u, version %u)\n",
		ops->name, ops->abi_version, ops->version);
	return 0;
}
EXPORT_SYMBOL_GPL(pvsched_register_policy);

void pvsched_unregister_policy(struct pvsched_policy_ops *ops)
{
	mutex_lock(&pvsched_mutex);
	list_del_init(&ops->list);
	mutex_unlock(&pvsched_mutex);

	pr_info("unregistered policy '%s'\n", ops->name);
}
EXPORT_SYMBOL_GPL(pvsched_unregister_policy);

/*
 * Match a guest header against the registry, in order abi -> name -> version,
 * each with its own status. On a full match a reference is taken on the policy
 * (bpf_try_module_get) and it is returned; otherwise NULL with *status set.
 */
static const struct pvsched_policy_ops *
pvsched_match_policy(const struct pvsched_header *hdr, u32 *status)
{
	const struct pvsched_policy_ops *found = NULL;
	struct pvsched_policy_ops *p;

	if (hdr->abi_version != PVSCHED_ABI_VERSION) {
		*status = PVSCHED_STATUS_ABI_MISMATCH;
		return NULL;
	}

	*status = PVSCHED_STATUS_UNKNOWN_POLICY;
	mutex_lock(&pvsched_mutex);
	list_for_each_entry(p, &pvsched_policies, list) {
		if (strncmp(p->name, hdr->policy_name, PVSCHED_NAME_MAX))
			continue;
		if (p->version != hdr->policy_version) {
			*status = PVSCHED_STATUS_POLICY_VERSION_MISMATCH;
			continue;
		}
		if (bpf_try_module_get(p, p->owner)) {
			found = p;
			*status = PVSCHED_STATUS_ENABLED;
		} else {
			/* Policy is going away (unload race): not enabled. */
			*status = PVSCHED_STATUS_DISABLED;
		}
		break;
	}
	mutex_unlock(&pvsched_mutex);
	return found;
}

/*
 * BPF struct_ops front-end: a BPF program implements pvsched_policy_ops and
 * registers it like a module policy would (precedent: tcp_congestion_ops).
 */

#if IS_ENABLED(CONFIG_BPF_SYSCALL)

/* Resolved at build time by resolve_btfids, against this module's BTF. */
BTF_ID_LIST_SINGLE(pvsched_vcpu_page_ids, union, pvsched_vcpu_page)

/* This module's BTF, saved at struct_ops registration. */
static const struct btf *pvsched_btf;

static int pvsched_bpf_init(struct btf *btf)
{
	pvsched_btf = btf;
	return 0;
}

/*
 * Writes through a shared-page pointer: only the host side of the page --
 * host_area -- is writable. The header is the framework's to write and
 * guest_area is the guest's; and no other BTF pointer (the vCPU and anything
 * reached through it) is writable at all. Reads are unrestricted.
 */
static int pvsched_bpf_btf_struct_access(struct bpf_verifier_log *log,
					 const struct bpf_reg_state *reg,
					 int off, int size)
{
	if (reg->btf != pvsched_btf || reg->btf_id != pvsched_vcpu_page_ids[0]) {
		bpf_log(log, "only writes to pvsched_vcpu_page are supported\n");
		return -EACCES;
	}

	if (off < offsetof(union pvsched_vcpu_page, host_area) ||
	    off + size > offsetofend(union pvsched_vcpu_page, host_area)) {
		bpf_log(log, "only host_area of pvsched_vcpu_page is writable\n");
		return -EACCES;
	}

	return 0;
}

static const struct bpf_verifier_ops pvsched_bpf_verifier_ops = {
	.get_func_proto		= bpf_base_func_proto,
	.is_valid_access	= bpf_tracing_btf_ctx_access,
	.btf_struct_access	= pvsched_bpf_btf_struct_access,
};

static int pvsched_bpf_init_member(const struct btf_type *t,
				   const struct btf_member *member,
				   void *kdata, const void *udata)
{
	const struct pvsched_policy_ops *uops = udata;
	struct pvsched_policy_ops *ops = kdata;
	u32 moff;

	moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct pvsched_policy_ops, name):
		if (bpf_obj_name_cpy(ops->name, uops->name,
				     sizeof(ops->name)) <= 0)
			return -EINVAL;
		return 1;
	case offsetof(struct pvsched_policy_ops, abi_version):
		ops->abi_version = uops->abi_version;
		return 1;
	case offsetof(struct pvsched_policy_ops, version):
		ops->version = uops->version;
		return 1;
	}
	return 0;
}

static int pvsched_bpf_reg(void *kdata, struct bpf_link *link)
{
	return pvsched_register_policy(kdata);
}

static void pvsched_bpf_unreg(void *kdata, struct bpf_link *link)
{
	pvsched_unregister_policy(kdata);
}

static void pvsched_bpf_vmentry(struct kvm_vcpu *vcpu,
				union pvsched_vcpu_page *shm)
{
}

static void pvsched_bpf_vmexit(struct kvm_vcpu *vcpu,
			       union pvsched_vcpu_page *shm)
{
}

static void pvsched_bpf_halt(struct kvm_vcpu *vcpu,
			     union pvsched_vcpu_page *shm)
{
}

static void pvsched_bpf_inject(struct kvm_vcpu *vcpu,
			       union pvsched_vcpu_page *shm)
{
}

static struct pvsched_policy_ops __bpf_pvsched_policy_ops = {
	.vmentry	= pvsched_bpf_vmentry,
	.vmexit		= pvsched_bpf_vmexit,
	.halt		= pvsched_bpf_halt,
	.inject		= pvsched_bpf_inject,
};

static struct bpf_struct_ops bpf_pvsched_policy_ops = {
	.verifier_ops	= &pvsched_bpf_verifier_ops,
	.init		= pvsched_bpf_init,
	.init_member	= pvsched_bpf_init_member,
	.reg		= pvsched_bpf_reg,
	.unreg		= pvsched_bpf_unreg,
	.name		= "pvsched_policy_ops",
	.cfi_stubs	= &__bpf_pvsched_policy_ops,
	.owner		= THIS_MODULE,
};

static int pvsched_bpf_struct_ops_init(void)
{
	int ret;

	/* So a struct_ops policy can call pvsched_set_params(). */
	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					&pvsched_kfunc_set);
	if (ret)
		return ret;
	return register_bpf_struct_ops(&bpf_pvsched_policy_ops,
				       pvsched_policy_ops);
}

#else /* !CONFIG_BPF_SYSCALL */

static int pvsched_bpf_struct_ops_init(void)
{
	return 0;
}

#endif /* CONFIG_BPF_SYSCALL */

/* Per-vCPU registration (/dev/pvsched). */

static long pvsched_register_vcpu(void __user *uarg)
{
	const struct pvsched_policy_ops *policy;
	struct pvsched_task *pt, *newpt = NULL;
	struct page *shm_page = NULL;
	union pvsched_vcpu_page *shm;
	struct pvsched_reg_vcpu reg;
	struct pvsched_header hdr;
	struct task_struct *p;
	unsigned long flags;
	u32 status = PVSCHED_STATUS_DISABLED;
	long ret = 0;

	if (copy_from_user(&reg, uarg, sizeof(reg)))
		return -EFAULT;

	/* The ABI hands out whole pages; a misaligned VA is a VMM bug. */
	if (reg.shm_hva & (PVSCHED_VCPU_STRIDE - 1))
		return -EINVAL;

	p = find_get_task_by_vpid(reg.tid);
	if (!p)
		return -ESRCH;

	/*
	 * The shared page is guest<->host memory; the VMM only couriers an
	 * opaque handle (shm_hva) and never dereferences it. Pin it and reach
	 * it solely through the kernel mapping -- the same way the dispatch
	 * path does at runtime -- instead of copy_*_user via the VMM's view.
	 */
	if (pin_user_pages_fast(reg.shm_hva, 1, FOLL_WRITE | FOLL_LONGTERM,
				&shm_page) != 1) {
		put_task_struct(p);
		return -EFAULT;
	}
	shm = page_address(shm_page);

	memcpy(&hdr, &shm->header, sizeof(hdr));		/* guest-written */
	hdr.policy_name[PVSCHED_NAME_MAX - 1] = '\0';		/* bound guest input */

	policy = pvsched_match_policy(&hdr, &status);
	if (!policy) {
		/* status already holds the precise reason (abi/name/version). */
		ret = -ENOENT;
		goto update_shm;
	}

	/* Allocate the per-task state outside the lock (it may sleep). */
	newpt = pvsched_task_alloc(p);
	if (!newpt) {
		status = PVSCHED_STATUS_DISABLED;
		ret = -ENOMEM;
		goto update_shm;
	}

	/*
	 * Bind under the hash lock. An entry exists only while bound (unregister
	 * frees it), so finding one means this vCPU is already registered -- a
	 * VMM bug, rejected with -EEXIST rather than silently rebinding.
	 */
	spin_lock_irqsave(&pvsched_ht_lock, flags);
	pt = pvsched_task_find(p);
	if (pt) {
		ret = -EEXIST;
		spin_unlock_irqrestore(&pvsched_ht_lock, flags);
		pvsched_task_free(newpt);
		goto out;
	}
	hash_add_rcu(pvsched_ht, &newpt->hash, (unsigned long)p);
	newpt->shm_page = shm_page;		/* entry owns the pin (lock-only) */
	WRITE_ONCE(newpt->shm, shm);		/* published to dispatch */
	WRITE_ONCE(newpt->policy, policy);
	shm_page = NULL;
	policy = NULL;
	/*
	 * Publish the negotiation result while the lock still holds off a
	 * concurrent unregister/exit teardown: once one can run, the binding's
	 * pin may be dropped and the page freed under us.
	 */
	WRITE_ONCE(shm->header.host_abi_version, PVSCHED_ABI_VERSION);
	WRITE_ONCE(shm->header.status, status);
	spin_unlock_irqrestore(&pvsched_ht_lock, flags);
	goto out;

update_shm:
	/* Declined: publish why; our transient pin still holds the page. */
	WRITE_ONCE(shm->header.host_abi_version, PVSCHED_ABI_VERSION);
	WRITE_ONCE(shm->header.status, status);

out:
	if (policy)			/* -ENOMEM / -EEXIST: drop the unused ref */
		bpf_module_put(policy, policy->owner);
	if (shm_page)			/* our transient pin (bind took it otherwise) */
		unpin_user_pages_dirty_lock(&shm_page, 1, ret != -EEXIST);

	put_task_struct(p);
	return ret;
}

/*
 * Unregister a vCPU: unhash its entry, then -- once in-flight dispatch readers
 * are done (grace period) -- cancel its cap timer, return the still-live vCPU
 * thread to baseline, drop the binding and free it. An entry exists only while
 * bound, so this is a full teardown; a later re-register starts fresh.
 */
static long pvsched_unregister_vcpu(void __user *uarg)
{
	struct task_struct *p;
	struct pvsched_task *pt;
	unsigned long flags;
	u32 tid;

	if (copy_from_user(&tid, uarg, sizeof(tid)))
		return -EFAULT;

	p = find_get_task_by_vpid(tid);
	if (!p)
		return -ESRCH;

	spin_lock_irqsave(&pvsched_ht_lock, flags);
	pt = pvsched_task_find(p);
	if (pt)
		hash_del_rcu(&pt->hash);
	spin_unlock_irqrestore(&pvsched_ht_lock, flags);

	if (pt) {
		/*
		 * Unhashed above: once readers drain, no boost can arm the timer,
		 * so the cancel is final. Then deboost the still-live vCPU thread.
		 */
		synchronize_rcu();
		hrtimer_cancel(&pt->timer);
		pvsched_set_baseline(pt->task);
		pvsched_task_free(pt);
	}

	put_task_struct(p);
	return 0;
}

static long pvsched_dev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	/*
	 * Registering delegates to the bound policy the authority to raise the
	 * thread's priority: require the capability that governs granting it.
	 */
	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	switch (cmd) {
	case PVSCHED_IOC_REGISTER_VCPU:
		return pvsched_register_vcpu((void __user *)arg);
	case PVSCHED_IOC_UNREGISTER_VCPU:
		return pvsched_unregister_vcpu((void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations pvsched_dev_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= pvsched_dev_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice pvsched_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "pvsched",
	.fops	= &pvsched_dev_fops,
};

/*
 * Event dispatch: the binding is read under RCU so a concurrent unbind cannot
 * free the policy mid-call. vmentry/vmexit/halt all run in the vCPU thread
 * itself; injection does not (see pvsched_inject_probe()).
 */
#define pvsched_dispatch_current(vcpu, cb)				\
do {									\
	struct pvsched_task *pt;					\
	const struct pvsched_policy_ops *policy;			\
									\
	rcu_read_lock();						\
	pt = pvsched_task_find(current);				\
	if (pt) {							\
		policy = READ_ONCE(pt->policy);				\
		if (policy)						\
			policy->cb(vcpu, READ_ONCE(pt->shm));		\
	}								\
	rcu_read_unlock();						\
} while (0)

static void pvsched_vmentry_probe(void *data, struct kvm_vcpu *vcpu)
{
	pvsched_dispatch_current(vcpu, vmentry);
}

static void pvsched_vmexit_probe(void *data, struct kvm_vcpu *vcpu)
{
	pvsched_dispatch_current(vcpu, vmexit);
}

static void pvsched_halt_probe(void *data, struct kvm_vcpu *vcpu)
{
	pvsched_dispatch_current(vcpu, halt);
}

/*
 * Injection runs in the injector's context, not the vCPU thread; resolve the
 * target via vcpu->pid. The pinned kernel mapping keeps @shm usable from this
 * context too.
 */
static void pvsched_inject_probe(void *data, struct kvm_vcpu *vcpu)
{
	struct pvsched_task *pt;
	const struct pvsched_policy_ops *policy;
	struct task_struct *t;

	rcu_read_lock();
	t = pid_task(rcu_dereference(vcpu->pid), PIDTYPE_PID);
	if (t) {
		pt = pvsched_task_find(t);
		if (pt) {
			policy = READ_ONCE(pt->policy);
			if (policy)
				policy->inject(vcpu, READ_ONCE(pt->shm));
		}
	}
	rcu_read_unlock();
}

static int pvsched_register_probes(void)
{
	int ret;

	ret = register_trace_kvm_pvsched_vmentry_tp(pvsched_vmentry_probe, NULL);
	if (ret)
		return ret;
	ret = register_trace_kvm_pvsched_vmexit_tp(pvsched_vmexit_probe, NULL);
	if (ret)
		goto err_vmexit;
	ret = register_trace_kvm_pvsched_vcpu_halt_tp(pvsched_halt_probe, NULL);
	if (ret)
		goto err_halt;
	ret = register_trace_kvm_pvsched_vcpu_inject_intr_tp(pvsched_inject_probe,
							     NULL);
	if (ret)
		goto err_inject;
	ret = register_trace_sched_process_exit(pvsched_sched_exit_probe, NULL);
	if (ret)
		goto err_exit;
	return 0;

err_exit:
	unregister_trace_kvm_pvsched_vcpu_inject_intr_tp(pvsched_inject_probe,
							 NULL);
err_inject:
	unregister_trace_kvm_pvsched_vcpu_halt_tp(pvsched_halt_probe, NULL);
err_halt:
	unregister_trace_kvm_pvsched_vmexit_tp(pvsched_vmexit_probe, NULL);
err_vmexit:
	unregister_trace_kvm_pvsched_vmentry_tp(pvsched_vmentry_probe, NULL);
	return ret;
}

static void pvsched_unregister_probes(void)
{
	unregister_trace_sched_process_exit(pvsched_sched_exit_probe, NULL);
	unregister_trace_kvm_pvsched_vcpu_inject_intr_tp(pvsched_inject_probe,
							 NULL);
	unregister_trace_kvm_pvsched_vcpu_halt_tp(pvsched_halt_probe, NULL);
	unregister_trace_kvm_pvsched_vmexit_tp(pvsched_vmexit_probe, NULL);
	unregister_trace_kvm_pvsched_vmentry_tp(pvsched_vmentry_probe, NULL);
	tracepoint_synchronize_unregister();
}

static int pvsched_policies_show(struct seq_file *m, void *v)
{
	struct pvsched_policy_ops *p;

	mutex_lock(&pvsched_mutex);
	list_for_each_entry(p, &pvsched_policies, list)
		seq_printf(m, "%s abi=%u version=%u\n",
			   p->name, p->abi_version, p->version);
	mutex_unlock(&pvsched_mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pvsched_policies);

static struct dentry *pvsched_debugfs;

static int __init pvsched_init(void)
{
	int ret;

	/* The shared-memory layout is a guest/host/VMM ABI; keep it fixed. */
	BUILD_BUG_ON(sizeof(struct pvsched_header) != 64);
	BUILD_BUG_ON(sizeof(struct pvsched_guest_area) != 32);
	BUILD_BUG_ON(sizeof(struct pvsched_host_area) != 32);
	BUILD_BUG_ON(sizeof(union pvsched_vcpu_page) != PVSCHED_VCPU_STRIDE);
	BUILD_BUG_ON(offsetof(union pvsched_vcpu_page, header) != 0);
	BUILD_BUG_ON(offsetof(union pvsched_vcpu_page, guest_area) != 64);
	BUILD_BUG_ON(offsetof(union pvsched_vcpu_page, host_area) != 96);

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING, &pvsched_kfunc_set);
	if (ret)
		return ret;

	ret = pvsched_bpf_struct_ops_init();
	if (ret)
		return ret;

	ret = pvsched_register_probes();
	if (ret)
		goto err_probes;

	ret = misc_register(&pvsched_dev);
	if (ret)
		goto err_misc;

	pvsched_debugfs = debugfs_create_dir("pvsched", NULL);
	debugfs_create_file("policies", 0444, pvsched_debugfs, NULL,
			    &pvsched_policies_fops);

	pr_info("paravirt scheduling host framework (ABI v%d)\n",
		PVSCHED_ABI_VERSION);
	return 0;

err_misc:
	pvsched_unregister_probes();
err_probes:
	/* The kfunc set is dropped automatically when the module unloads. */
	return ret;
}

static void __exit pvsched_exit(void)
{
	struct pvsched_task *pt;
	struct hlist_node *tmp;
	int bkt;

	debugfs_remove_recursive(pvsched_debugfs);
	misc_deregister(&pvsched_dev);

	/* No more events or exit notifications after this. */
	pvsched_unregister_probes();
	flush_work(&pvsched_teardown_work);

	/*
	 * Tear down any remaining vCPUs. Nothing can race with us: the device
	 * is gone, the probes are quiesced, the teardown work is flushed, and
	 * BPF users of the kfunc hold a module reference, so none are loaded
	 * -- the hash is ours alone, no lock or RCU needed.
	 */
	hash_for_each_safe(pvsched_ht, bkt, tmp, pt, hash) {
		hash_del(&pt->hash);
		hrtimer_cancel(&pt->timer);
		pvsched_set_baseline(pt->task);
		pvsched_task_free(pt);
	}
}

module_init(pvsched_init);
module_exit(pvsched_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Paravirt scheduling host framework");
