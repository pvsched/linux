// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
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

struct pvsched_hook_observer {
	spinlock_t lock;
	struct pid *filter;
	bool active;
	bool pending;
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

	spin_lock_irqsave(&obs.lock, irqflags);
	owner = obs_matches();
	spin_unlock_irqrestore(&obs.lock, irqflags);
	return owner;
}

static void obs_record(unsigned int event, struct kvm_vcpu *vcpu,
			     int ret, u32 reason)
{
	unsigned long irqflags;
	struct pvsched_hook_record *record;

	spin_lock_irqsave(&obs.lock, irqflags);
	if (!obs_matches())
		goto out;
	if (event == PVSCHED_HOOK_ENTER) {
		if (obs.pending)
			obs.flags |= PVSCHED_HOOK_F_UNMATCHED;
		obs.pending = true;
	} else {
		if (!obs.pending)
			obs.flags |= PVSCHED_HOOK_F_UNMATCHED;
		obs.pending = false;
	}
	if (obs.count == PVSCHED_HOOK_MAX_RECORDS) {
		obs.flags |= PVSCHED_HOOK_F_OVERFLOW;
		goto out;
	}
	record = &obs.records[obs.count++];
	record->seq = obs.next_seq++;
	record->event = event;
	record->vcpu_id = vcpu->vcpu_id;
	record->ret = ret;
	record->exit_reason = reason;
out:
	spin_unlock_irqrestore(&obs.lock, irqflags);
}

static void obs_enter(void *data, struct kvm_vcpu *vcpu)
{
	obs_record(PVSCHED_HOOK_ENTER, vcpu, 0, 0);
}

static void obs_leave(void *data, struct kvm_vcpu *vcpu, int ret,
			     u32 exit_reason)
{
	obs_record(PVSCHED_HOOK_LEAVE, vcpu, ret, exit_reason);
}

static int obs_open(struct inode *inode, struct file *file)
{
	struct pid *filter;

	if (atomic_cmpxchg(&obs_opened, 0, 1))
		return -EBUSY;
	filter = get_task_pid(current, PIDTYPE_PID);
	spin_lock(&obs.lock);
	obs.filter = filter;
	obs.active = true;
	obs.pending = false;
	obs.count = 0;
	obs.flags = 0;
	obs.next_seq = 0;
	memset(obs.records, 0, sizeof(obs.records));
	spin_unlock(&obs.lock);
	return 0;
}

static int obs_release(struct inode *inode, struct file *file)
{
	struct pid *filter;

	spin_lock(&obs.lock);
	obs.active = false;
	filter = obs.filter;
	obs.filter = NULL;
	spin_unlock(&obs.lock);
	tracepoint_synchronize_unregister();
	put_pid(filter);
	atomic_set(&obs_opened, 0);
	return 0;
}

static long obs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned long irqflags;

	if (!obs_owner())
		return -EPERM;
	if (cmd != PVSCHED_HOOK_IOC_RESET)
		return -ENOTTY;
	spin_lock_irqsave(&obs.lock, irqflags);
	if (obs.pending) {
		spin_unlock_irqrestore(&obs.lock, irqflags);
		return -EBUSY;
	}
	obs.pending = false;
	obs.count = 0;
	obs.flags = 0;
	obs.next_seq = 0;
	memset(obs.records, 0, sizeof(obs.records));
	spin_unlock_irqrestore(&obs.lock, irqflags);
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
	spin_lock_irqsave(&obs.lock, irqflags);
	snapshot->count = obs.count;
	snapshot->flags = obs.flags | (obs.pending ? PVSCHED_HOOK_F_UNMATCHED : 0);
	snapshot->next_seq = obs.next_seq;
	memcpy(snapshot->records, obs.records, sizeof(snapshot->records));
	spin_unlock_irqrestore(&obs.lock, irqflags);
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

static int __init obs_init(void)
{
	int ret;

	spin_lock_init(&obs.lock);
	ret = register_trace_kvm_pvsched_run_enter_tp(obs_enter, NULL);
	if (ret)
		return ret;
	ret = register_trace_kvm_pvsched_run_leave_tp(obs_leave, NULL);
	if (ret) {
		unregister_trace_kvm_pvsched_run_enter_tp(obs_enter, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}
	ret = misc_register(&obs_device);
	if (ret) {
		unregister_trace_kvm_pvsched_run_leave_tp(obs_leave, NULL);
		unregister_trace_kvm_pvsched_run_enter_tp(obs_enter, NULL);
		tracepoint_synchronize_unregister();
	}
	return ret;
}

static void __exit obs_exit(void)
{
	struct pid *filter;

	misc_deregister(&obs_device);
	spin_lock(&obs.lock);
	obs.active = false;
	filter = obs.filter;
	obs.filter = NULL;
	spin_unlock(&obs.lock);
	unregister_trace_kvm_pvsched_run_leave_tp(obs_leave, NULL);
	unregister_trace_kvm_pvsched_run_enter_tp(obs_enter, NULL);
	tracepoint_synchronize_unregister();
	put_pid(filter);
	atomic_set(&obs_opened, 0);
}

module_init(obs_init);
module_exit(obs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("pvsched test-only KVM hook observer");
