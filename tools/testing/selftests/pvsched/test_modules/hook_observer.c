// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kvm_host.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>

#include <asm/kvm_pvsched.h>
#include <trace/events/kvm_pvsched.h>

#include "hook_observer.h"

static_assert(PVSCHED_HOOK_MODE_SVM_AVIC == KVM_PVSCHED_MODE_SVM_AVIC);
static_assert(PVSCHED_HOOK_MODE_SVM_VNMI == KVM_PVSCHED_MODE_SVM_VNMI);

struct pvsched_hook_observer {
	raw_spinlock_t lock;
	struct pid *filter;
	bool active;
	bool pending;
	bool halt_seen;
	wait_queue_head_t wait;
	unsigned int count;
	unsigned int flags;
	u64 next_seq;
	struct pvsched_hook_record records[PVSCHED_HOOK_MAX_RECORDS];
};

static struct pvsched_hook_observer obs;
static atomic_t obs_opened = ATOMIC_INIT(0);

static bool obs_matches(void)
{
	return obs.active && task_pid(current) == obs.filter;
}

static bool obs_owner(void)
{
	unsigned long irqflags;
	bool owner;

	raw_spin_lock_irqsave(&obs.lock, irqflags);
	owner = obs_matches();
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
	return owner;
}

static void obs_record_locked(unsigned int event, struct kvm_vcpu *vcpu,
			     int ret, u32 reason, u32 mode_flags,
			     u32 event_flags)
{
	struct pvsched_hook_record *record;

	if (!obs_matches())
		return;
	if (event == PVSCHED_HOOK_ENTER) {
		if (obs.pending)
			obs.flags |= PVSCHED_HOOK_F_UNMATCHED;
		obs.pending = true;
	} else if (event == PVSCHED_HOOK_LEAVE) {
		if (!obs.pending)
			obs.flags |= PVSCHED_HOOK_F_UNMATCHED;
		obs.pending = false;
	}
	if (obs.count == PVSCHED_HOOK_MAX_RECORDS) {
		obs.flags |= PVSCHED_HOOK_F_OVERFLOW;
		return;
	}
	record = &obs.records[obs.count++];
	record->seq = obs.next_seq++;
	record->event = event;
	record->vcpu_id = vcpu->vcpu_id;
	record->ret = ret;
	record->exit_reason = reason;
	record->mode_flags = mode_flags;
	record->event_flags = event_flags;
}

static void obs_record(unsigned int event, struct kvm_vcpu *vcpu,
		      int ret, u32 reason, u32 mode_flags, u32 event_flags)
{
	unsigned long irqflags;

	raw_spin_lock_irqsave(&obs.lock, irqflags);
	obs_record_locked(event, vcpu, ret, reason, mode_flags, event_flags);
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
}

static void obs_enter(void *data, struct kvm_vcpu *vcpu, u32 mode_flags)
{
	obs_record(PVSCHED_HOOK_ENTER, vcpu, 0, 0, mode_flags, 0);
}

static void obs_leave(void *data, struct kvm_vcpu *vcpu, int ret,
			     u32 exit_reason)
{
	obs_record(PVSCHED_HOOK_LEAVE, vcpu, ret, exit_reason, 0, 0);
}

static void obs_vmentry(void *data, struct kvm_vcpu *vcpu, u32 mode_flags,
			bool interrupt_ready)
{
	obs_record(PVSCHED_HOOK_VMENTRY, vcpu, 0, 0, mode_flags,
		  interrupt_ready ? PVSCHED_HOOK_RECORD_F_INTERRUPT_READY : 0);
}

static void obs_vmexit_irqoff(void *data, struct kvm_vcpu *vcpu)
{
	obs_record(PVSCHED_HOOK_VMEXIT_IRQOFF, vcpu, 0, 0, 0, 0);
}

static void obs_vmexit(void *data, struct kvm_vcpu *vcpu, u32 mode_flags,
		       bool hlt_exit)
{
	obs_record(PVSCHED_HOOK_VMEXIT, vcpu, 0, 0, mode_flags,
		   hlt_exit ? PVSCHED_HOOK_RECORD_F_HLT_EXIT : 0);
}

static void obs_vmentry_cancel(void *data, struct kvm_vcpu *vcpu,
			      u32 mode_flags)
{
	obs_record(PVSCHED_HOOK_VMENTRY_CANCEL, vcpu, 0, 0, mode_flags, 0);
}

static void obs_halt(void *data, struct kvm_vcpu *vcpu, u32 mode_flags)
{
	unsigned long irqflags;
	bool wake = false;

	raw_spin_lock_irqsave(&obs.lock, irqflags);
	if (obs_matches()) {
		obs_record_locked(PVSCHED_HOOK_HALT, vcpu, 0, 0, mode_flags, 0);
		WRITE_ONCE(obs.halt_seen, true);
		wake = true;
	}
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
	if (wake)
		wake_up_interruptible(&obs.wait);
}

static void obs_unhalt(void *data, struct kvm_vcpu *vcpu, u32 mode_flags)
{
	obs_record(PVSCHED_HOOK_UNHALT, vcpu, 0, 0, mode_flags, 0);
}

static void obs_inject_intr(void *data, struct kvm_vcpu *vcpu,
			   u32 mode_flags, bool source_guest_mode)
{
	obs_record(PVSCHED_HOOK_INJECT_INTR, vcpu, 0, 0, mode_flags, 0);
}

static void obs_reset_locked(void)
{
	obs.pending = false;
	obs.halt_seen = false;
	obs.count = 0;
	obs.flags = 0;
	obs.next_seq = 0;
	memset(obs.records, 0, sizeof(obs.records));
}

/* Stop recording, wake a halt waiter and return the filter to drop. */
static struct pid *obs_deactivate(void)
{
	unsigned long irqflags;
	struct pid *filter;

	raw_spin_lock_irqsave(&obs.lock, irqflags);
	obs.active = false;
	filter = obs.filter;
	obs.filter = NULL;
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
	wake_up_interruptible(&obs.wait);
	return filter;
}

static int obs_open(struct inode *inode, struct file *file)
{
	unsigned long irqflags;
	struct pid *filter;

	if (atomic_cmpxchg(&obs_opened, 0, 1))
		return -EBUSY;
	filter = get_task_pid(current, PIDTYPE_PID);
	raw_spin_lock_irqsave(&obs.lock, irqflags);
	obs.filter = filter;
	obs.active = true;
	obs_reset_locked();
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
	return 0;
}

static int obs_release(struct inode *inode, struct file *file)
{
	struct pid *filter = obs_deactivate();

	tracepoint_synchronize_unregister();
	put_pid(filter);
	atomic_set(&obs_opened, 0);
	return 0;
}

static long obs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned long irqflags;
	long ret;

	if (cmd == PVSCHED_HOOK_IOC_WAIT_HALT) {
		ret = wait_event_interruptible_timeout(
				obs.wait,
				READ_ONCE(obs.halt_seen) || !READ_ONCE(obs.active),
				5 * HZ);
		if (ret < 0)
			return ret;
		if (!READ_ONCE(obs.active))
			return -ENODEV;
		return ret ? 0 : -ETIMEDOUT;
	}
	if (!obs_owner())
		return -EPERM;
	if (cmd != PVSCHED_HOOK_IOC_RESET)
		return -ENOTTY;
	raw_spin_lock_irqsave(&obs.lock, irqflags);
	if (obs.pending) {
		raw_spin_unlock_irqrestore(&obs.lock, irqflags);
		return -EBUSY;
	}
	obs_reset_locked();
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
	return 0;
}

static ssize_t obs_read(struct file *file, char __user *buf, size_t len,
			loff_t *ppos)
{
	struct pvsched_hook_snapshot *snapshot;
	unsigned long irqflags;
	ssize_t ret;

	if (!obs_owner())
		return -EPERM;
	if (len < sizeof(*snapshot))
		return -EINVAL;
	snapshot = kmalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;
	raw_spin_lock_irqsave(&obs.lock, irqflags);
	snapshot->count = obs.count;
	snapshot->flags = obs.flags | (obs.pending ? PVSCHED_HOOK_F_UNMATCHED : 0);
	snapshot->next_seq = obs.next_seq;
	memcpy(snapshot->records, obs.records, sizeof(snapshot->records));
	raw_spin_unlock_irqrestore(&obs.lock, irqflags);
	ret = copy_to_user(buf, snapshot, sizeof(*snapshot)) ? -EFAULT :
		sizeof(*snapshot);
	kfree(snapshot);
	if (ret < 0)
		return ret;
	*ppos = 0;
	return ret;
}

static const struct file_operations obs_fops = {
	.owner = THIS_MODULE,
	.open = obs_open,
	.release = obs_release,
	.unlocked_ioctl = obs_ioctl,
	.read = obs_read,
	.llseek = noop_llseek,
};

static struct miscdevice obs_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = PVSCHED_HOOK_OBSERVER_DEVICE,
	.fops = &obs_fops,
	.mode = 0600,
};

struct obs_probe {
	struct tracepoint *tp;
	void *probe;
};

static const struct obs_probe obs_probes[] = {
	{ &__tracepoint_kvm_pvsched_run_enter_tp, obs_enter },
	{ &__tracepoint_kvm_pvsched_run_leave_tp, obs_leave },
	{ &__tracepoint_kvm_pvsched_vmentry_tp, obs_vmentry },
	{ &__tracepoint_kvm_pvsched_vmexit_irqoff_tp, obs_vmexit_irqoff },
	{ &__tracepoint_kvm_pvsched_vmexit_tp, obs_vmexit },
	{ &__tracepoint_kvm_pvsched_vmentry_cancel_tp, obs_vmentry_cancel },
	{ &__tracepoint_kvm_pvsched_vcpu_halt_tp, obs_halt },
	{ &__tracepoint_kvm_pvsched_vcpu_unhalt_tp, obs_unhalt },
	{ &__tracepoint_kvm_pvsched_vcpu_inject_intr_tp, obs_inject_intr },
};

/* Check each probe in the untyped table against its tracepoint. */
static void __maybe_unused obs_probe_types(void)
{
	check_trace_callback_type_kvm_pvsched_run_enter_tp(obs_enter);
	check_trace_callback_type_kvm_pvsched_run_leave_tp(obs_leave);
	check_trace_callback_type_kvm_pvsched_vmentry_tp(obs_vmentry);
	check_trace_callback_type_kvm_pvsched_vmexit_irqoff_tp(obs_vmexit_irqoff);
	check_trace_callback_type_kvm_pvsched_vmexit_tp(obs_vmexit);
	check_trace_callback_type_kvm_pvsched_vmentry_cancel_tp(obs_vmentry_cancel);
	check_trace_callback_type_kvm_pvsched_vcpu_halt_tp(obs_halt);
	check_trace_callback_type_kvm_pvsched_vcpu_unhalt_tp(obs_unhalt);
	check_trace_callback_type_kvm_pvsched_vcpu_inject_intr_tp(obs_inject_intr);
}

static void obs_unregister(unsigned int count)
{
	while (count--)
		tracepoint_probe_unregister(obs_probes[count].tp,
					    obs_probes[count].probe, NULL);
}

static int __init obs_init(void)
{
	unsigned int i;
	int ret;

	raw_spin_lock_init(&obs.lock);
	init_waitqueue_head(&obs.wait);
	for (i = 0; i < ARRAY_SIZE(obs_probes); i++) {
		ret = tracepoint_probe_register(obs_probes[i].tp,
						obs_probes[i].probe, NULL);
		if (ret)
			goto unregister;
	}
	ret = misc_register(&obs_device);
	if (!ret)
		return 0;
unregister:
	if (i) {
		obs_unregister(i);
		tracepoint_synchronize_unregister();
	}
	return ret;
}

static void __exit obs_exit(void)
{
	struct pid *filter;

	misc_deregister(&obs_device);
	filter = obs_deactivate();
	obs_unregister(ARRAY_SIZE(obs_probes));
	tracepoint_synchronize_unregister();
	put_pid(filter);
	atomic_set(&obs_opened, 0);
}

module_init(obs_init);
module_exit(obs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pvsched test-only KVM hook observer");
