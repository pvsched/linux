// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <uapi/linux/pvsched.h>

#include "internal.h"
#include "lifecycle.h"
#include "policy.h"

static DEFINE_SPINLOCK(pvsched_global_lock);
static DEFINE_HASHTABLE(pvsched_runner_hash, 14);
static atomic_t pvsched_nr_runners = ATOMIC_INIT(0);
static atomic_t pvsched_nr_sessions = ATOMIC_INIT(0);

static bool pvsched_authorized(struct file *file,
			       struct pvsched_session *session)
{
	if (!capable(CAP_SYS_NICE) ||
	    !file_ns_capable(file, &init_user_ns, CAP_SYS_NICE))
		return false;

	/*
	 * A TGID can survive exec, while the mm pointer identifies the address
	 * space that opened the session.  CLONE_VM can share an mm across thread
	 * groups, so both checks are required.  O_CLOEXEC is only VMM hygiene.
	 */
	return current->mm == session->owner_mm &&
	       task_tgid(current) == session->owner_tgid;
}

static struct pvsched_vcpu_runner *
pvsched_find_runner(struct pvsched_session *session, u64 runner_id)
{
	struct pvsched_vcpu_runner *runner;

	list_for_each_entry(runner, &session->runners, session_node) {
		if (runner->runner_id == runner_id)
			return runner;
	}

	return NULL;
}

static int
pvsched_runner_register_global(struct pvsched_vcpu_runner *new_runner)
{
	struct pvsched_vcpu_runner *runner;
	struct pid *pid = new_runner->pid;

	guard(spinlock)(&pvsched_global_lock);

	hash_for_each_possible(pvsched_runner_hash, runner, global_node,
			       (unsigned long)pid) {
		if (runner->pid == pid)
			return -EEXIST;
	}

	hash_add(pvsched_runner_hash, &new_runner->global_node,
		 (unsigned long)pid);
	return 0;
}

static long pvsched_get_info(void __user *argp)
{
	struct pvsched_info info = {
		.control_version = PVSCHED_CONTROL_VERSION,
		.max_runners_per_session = PVSCHED_MAX_RUNNERS_PER_SESSION,
		.max_runners_global = PVSCHED_MAX_RUNNERS_GLOBAL,
		.max_sessions_global = PVSCHED_MAX_SESSIONS_GLOBAL,
		.max_shm_pages_per_session = PVSCHED_MAX_SHM_PAGES_PER_SESSION,
		.max_shm_pages_global = PVSCHED_MAX_SHM_PAGES_GLOBAL,
	};

	return copy_to_user(argp, &info, sizeof(info)) ? -EFAULT : 0;
}

static long pvsched_query_policy(void __user *argp)
{
	struct pvsched_policy_info info;
	int ret;

	if (copy_from_user(&info, argp, sizeof(info)))
		return -EFAULT;
	if (info.flags || memchr_inv(info.name, 0, sizeof(info.name)) ||
	    info.version || info.protocol || info.params_size || info.reserved)
		return -EINVAL;
	ret = pvsched_policy_query(info.index, &info);
	if (ret)
		return ret;
	return copy_to_user(argp, &info, sizeof(info)) ? -EFAULT : 0;
}

static long pvsched_set_policy(struct pvsched_session *session,
			       void __user *argp)
{
	struct pvsched_set_policy input;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (input.flags || !pvsched_policy_name_valid(input.name))
		return -EINVAL;

	guard(mutex)(&session->lock);
	/* Runners and their baselines are sized for the first choice. */
	if (session->entry)
		return -EBUSY;
	return pvsched_policy_pin(input.name, input.version, &session->entry);
}

static long pvsched_create_runner(struct pvsched_session *session,
				  void __user *argp)
{
	struct pvsched_create_runner input, output;
	struct pvsched_vcpu_runner *runner = NULL;
	struct task_struct *task = NULL;
	struct mm_struct *target_mm = NULL;
	struct pid *target_tgid = NULL;
	struct pid *runner_pid = NULL;
	u64 runner_id;
	int ret;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (input.flags || input.tid <= 0 || input.runner_id ||
	    input.reserved[0] || input.reserved[1])
		return -EINVAL;

	guard(mutex)(&session->lock);
	/* A VMM must choose the session's policy first. */
	if (!session->entry)
		return -EINVAL;
	if (session->nr_runners >= PVSCHED_MAX_RUNNERS_PER_SESSION)
		return -ENOSPC;
	if (!session->next_runner_id)
		return -EOVERFLOW;
	runner_id = session->next_runner_id++;

	/* The bound includes creates that have reserved quota but not published. */
	if (!atomic_add_unless(&pvsched_nr_runners, 1,
			       PVSCHED_MAX_RUNNERS_GLOBAL))
		return -ENOSPC;

	/* Resolve the numeric TID exactly once and retain that identity. */
	runner_pid = find_get_pid(input.tid);
	if (!runner_pid) {
		ret = -ESRCH;
		goto out_cleanup;
	}
	task = get_pid_task(runner_pid, PIDTYPE_PID);
	if (!task) {
		ret = -ESRCH;
		goto out_cleanup;
	}

	target_mm = get_task_mm(task);
	if (!target_mm) {
		ret = -ESRCH;
		goto out_cleanup;
	}
	target_tgid = get_task_pid(task, PIDTYPE_TGID);
	if (!same_thread_group(task, current) ||
	    target_mm != session->owner_mm ||
	    target_tgid != session->owner_tgid) {
		ret = -EPERM;
		goto out_cleanup;
	}

	runner = kzalloc_obj(*runner);
	if (!runner) {
		ret = -ENOMEM;
		goto out_cleanup;
	}
	runner->pid = runner_pid;
	runner->runner_id = runner_id;
	ret = pvsched_runner_lifecycle_init(runner, session,
					    session->entry->ops->params_size);
	if (ret) {
		kfree(runner);
		runner = NULL;
		goto out_cleanup;
	}

	output = (struct pvsched_create_runner) {
		.tid = input.tid,
		.runner_id = runner_id,
	};
	if (copy_to_user(argp, &output, sizeof(output))) {
		ret = -EFAULT;
		goto out_cleanup;
	}

	ret = pvsched_runner_register_global(runner);
	if (ret)
		goto out_cleanup;

	list_add_tail(&runner->session_node, &session->runners);
	session->nr_runners++;
	mmput(target_mm);
	put_pid(target_tgid);
	put_task_struct(task);
	return 0;

out_cleanup:
	if (runner)
		pvsched_runner_lifecycle_destroy(runner);
	kfree(runner);
	put_pid(runner_pid);
	put_pid(target_tgid);
	if (target_mm)
		mmput(target_mm);
	if (task)
		put_task_struct(task);
	atomic_dec(&pvsched_nr_runners);
	return ret;
}

static long pvsched_query_runner(struct pvsched_session *session,
				 void __user *argp)
{
	struct pvsched_query_runner input, output;
	struct pvsched_vcpu_runner *runner;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (!input.runner_id || input.flags || input.state ||
	    input.last_fault_errno || input.reserved0 || input.reserved1)
		return -EINVAL;

	guard(mutex)(&session->lock);
	runner = pvsched_find_runner(session, input.runner_id);
	if (!runner)
		return -ENOENT;

	output = (struct pvsched_query_runner) {
		.runner_id = input.runner_id,
	};
	pvsched_runner_query_locked(runner, &output.state, &output.flags,
				    &output.last_fault_errno);

	return copy_to_user(argp, &output, sizeof(output)) ? -EFAULT : 0;
}

static long pvsched_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct pvsched_session *session = file->private_data;
	void __user *argp = (void __user *)arg;

	if (!pvsched_authorized(file, session))
		return -EPERM;

	switch (cmd) {
	case PVSCHED_GET_INFO:
		return pvsched_get_info(argp);
	case PVSCHED_CREATE_RUNNER:
		return pvsched_create_runner(session, argp);
	case PVSCHED_QUERY_RUNNER:
		return pvsched_query_runner(session, argp);
	case PVSCHED_QUERY_POLICY:
		return pvsched_query_policy(argp);
	case PVSCHED_SET_POLICY:
		return pvsched_set_policy(session, argp);
	default:
		return -ENOTTY;
	}
}

static int pvsched_open(struct inode *inode, struct file *file)
{
	struct pvsched_session *session;

	if ((file->f_flags & O_ACCMODE) != O_RDWR || !current->mm)
		return -EINVAL;
	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	/* The session bound includes opens that have reserved quota. */
	if (!atomic_add_unless(&pvsched_nr_sessions, 1,
			       PVSCHED_MAX_SESSIONS_GLOBAL))
		return -ENOSPC;

	session = kzalloc_obj(*session);
	if (!session) {
		atomic_dec(&pvsched_nr_sessions);
		return -ENOMEM;
	}

	mutex_init(&session->lock);
	INIT_LIST_HEAD(&session->runners);
	/*
	 * mmgrab pins the mm_struct identity without keeping its userspace
	 * mappings alive.  It detects later exec even when the TGID survives.
	 */
	session->owner_mm = current->mm;
	mmgrab(session->owner_mm);
	session->owner_tgid = get_pid(task_tgid(current));
	session->next_runner_id = 1;
	file->private_data = session;
	return 0;
}

static int pvsched_release(struct inode *inode, struct file *file)
{
	struct pvsched_session *session = file->private_data;
	struct pvsched_vcpu_runner *runner, *next;

	mutex_lock(&session->lock);
	session->closing = true;
	mutex_unlock(&session->lock);
	pvsched_session_release_runtimes(session);
	mutex_lock(&session->lock);
	spin_lock(&pvsched_global_lock);
	list_for_each_entry(runner, &session->runners, session_node)
		hash_del(&runner->global_node);
	spin_unlock(&pvsched_global_lock);
	mutex_unlock(&session->lock);

	list_for_each_entry_safe(runner, next, &session->runners, session_node) {
		put_pid(runner->pid);
		pvsched_runner_lifecycle_destroy(runner);
		kfree(runner);
	}
	/* No runtime can call the policy any more. */
	if (session->entry)
		pvsched_policy_unpin(session->entry);
	put_pid(session->owner_tgid);
	mmdrop(session->owner_mm);
	atomic_sub(session->nr_runners, &pvsched_nr_runners);
	kfree(session);
	atomic_dec(&pvsched_nr_sessions);
	return 0;
}

static const struct file_operations pvsched_fops = {
	.owner = THIS_MODULE,
	.open = pvsched_open,
	.release = pvsched_release,
	.unlocked_ioctl = pvsched_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice pvsched_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pvsched",
	.fops = &pvsched_fops,
	.mode = 0600,
};

static int __init pvsched_init(void)
{
	int ret;

	/* The default policy is listed before the device can open a session. */
	ret = pvsched_policy_init();
	if (ret)
		return ret;
	ret = misc_register(&pvsched_device);
	if (ret)
		pvsched_policy_exit();
	return ret;
}
module_init(pvsched_init);

static void __exit pvsched_exit(void)
{
	misc_deregister(&pvsched_device);
	pvsched_policy_exit();
}
module_exit(pvsched_exit);

MODULE_DESCRIPTION("Paravirtualized scheduling host framework");
MODULE_LICENSE("GPL");
