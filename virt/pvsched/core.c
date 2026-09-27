// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/cred.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <uapi/linux/pvsched.h>
#include <kunit/visibility.h>

#include "default_policy.h"
#include "internal.h"
#include "lifecycle.h"
#include "policy.h"

/*
 * Framework configuration: fixed at module load (or on the kernel command
 * line when built in) and validated once at initialization.  The default
 * policy's priorities are its own parameters.
 */
static unsigned int cs_budget_us = PVSCHED_DEFAULT_CS_BUDGET_NS / NSEC_PER_USEC;
module_param(cs_budget_us, uint, 0444);
MODULE_PARM_DESC(cs_budget_us, "Critical-section boost budget in microseconds");

static unsigned int generic_budget_us =
	PVSCHED_DEFAULT_GENERIC_BUDGET_NS / NSEC_PER_USEC;
module_param(generic_budget_us, uint, 0444);
MODULE_PARM_DESC(generic_budget_us, "Total elevated-runtime budget in microseconds");

static bool deboost_notify = true;
module_param(deboost_notify, bool, 0444);
MODULE_PARM_DESC(deboost_notify, "Ask guests to kick the host after a deboost");

static unsigned int idle_hold_us = 200;
module_param(idle_hold_us, uint, 0444);
MODULE_PARM_DESC(idle_hold_us,
		 "Longest boost kept for an idle guest on its way to a halt, in microseconds (at most cs_budget_us; 0: off)");

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

static long pvsched_attach_shm(struct pvsched_session *session,
			       void __user *argp)
{
	struct pvsched_attach_shm input, output;
	struct pvsched_runner_runtime *runtime;
	struct pvsched_attach_params params;
	struct pvsched_vcpu_runner *runner;
	struct task_struct *task;
	enum pvsched_status status;
	u64 end;
	int ret;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (!input.runner_id || input.flags || input.negotiation_status ||
	    input.size != PVSCHED_VCPU_STRIDE ||
	    !IS_ALIGNED(input.user_addr, PVSCHED_VCPU_STRIDE) ||
	    check_add_overflow(input.user_addr, input.size, &end))
		return -EINVAL;

	guard(mutex)(&session->lock);
	runner = pvsched_find_runner(session, input.runner_id);
	if (!runner)
		return -ENOENT;
	/* Finish requested cleanup so a revoked attachment does not linger. */
	pvsched_runner_cleanup_once_locked(runner);
	task = get_pid_task(runner->pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	/* Authorization proved the caller owns the session's address space. */
	params = (struct pvsched_attach_params) {
		.user_addr = input.user_addr,
		.entry = session->entry,
		.cs_budget_ns = (u64)cs_budget_us * NSEC_PER_USEC,
		.generic_budget_ns = (u64)generic_budget_us * NSEC_PER_USEC,
		.deboost_notify = deboost_notify,
		.idle_hold_ns = (u64)idle_hold_us * NSEC_PER_USEC,
		.cap_ipc_lock = capable(CAP_IPC_LOCK),
		.memlock_limit_pages = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT,
	};
	ret = pvsched_runner_prepare_attach_locked(runner, task, &params,
						   &runtime, &status);
	put_task_struct(task);
	if (ret)
		return ret;

	/* A failed copyout undoes the preparation without any page response. */
	output = input;
	output.negotiation_status = status;
	if (copy_to_user(argp, &output, sizeof(output))) {
		pvsched_runner_discard_prepared_locked(runtime);
		return -EFAULT;
	}
	if (status != PVSCHED_STATUS_ENABLED) {
		pvsched_runner_reject_prepared_locked(runtime);
		return -EPROTO;
	}
	ret = pvsched_runner_publish_prepared_locked(runner, runtime);
	if (ret)
		pvsched_runner_discard_prepared_locked(runtime);
	return ret;
}

static long pvsched_detach_shm(struct pvsched_session *session,
			       void __user *argp)
{
	struct pvsched_detach_shm input;
	struct pvsched_vcpu_runner *runner;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (!input.runner_id || input.reserved[0] || input.reserved[1] ||
	    input.reserved[2])
		return -EINVAL;

	guard(mutex)(&session->lock);
	runner = pvsched_find_runner(session, input.runner_id);
	if (!runner)
		return -ENOENT;
	/*
	 * Detach completes even when the final owned restore fails; QUERY
	 * reports that failure, and the VMM owns the thread from here on.
	 */
	pvsched_runner_detach_runtime_locked(runner);
	return 0;
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
	case PVSCHED_ATTACH_SHM:
		return pvsched_attach_shm(session, argp);
	case PVSCHED_DETACH_SHM:
		return pvsched_detach_shm(session, argp);
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

VISIBLE_IF_KUNIT const struct file_operations pvsched_fops = {
	.owner = THIS_MODULE,
	.open = pvsched_open,
	.release = pvsched_release,
	.unlocked_ioctl = pvsched_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

EXPORT_SYMBOL_IF_KUNIT(pvsched_fops);

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * CREATE_RUNNER cannot register a kthread, whose mm get_task_mm() refuses,
 * so tests register the KUnit thread through the same runner bookkeeping,
 * after SET_POLICY as a VMM must.
 */
int pvsched_session_add_current_runner(struct pvsched_session *session,
				       u64 *runner_id)
{
	struct pvsched_vcpu_runner *runner;
	int ret;

	runner = kzalloc_obj(*runner);
	if (!runner)
		return -ENOMEM;
	guard(mutex)(&session->lock);
	if (!session->entry) {
		kfree(runner);
		return -EINVAL;
	}
	if (!atomic_add_unless(&pvsched_nr_runners, 1,
			       PVSCHED_MAX_RUNNERS_GLOBAL)) {
		kfree(runner);
		return -ENOSPC;
	}
	ret = pvsched_runner_lifecycle_init(runner, session,
					    session->entry->ops->params_size);
	if (ret) {
		kfree(runner);
		atomic_dec(&pvsched_nr_runners);
		return ret;
	}
	runner->pid = get_task_pid(current, PIDTYPE_PID);
	runner->runner_id = session->next_runner_id++;
	ret = pvsched_runner_register_global(runner);
	if (ret) {
		put_pid(runner->pid);
		pvsched_runner_lifecycle_destroy(runner);
		kfree(runner);
		atomic_dec(&pvsched_nr_runners);
		return ret;
	}
	list_add_tail(&runner->session_node, &session->runners);
	session->nr_runners++;
	*runner_id = runner->runner_id;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_session_add_current_runner);
#endif

static struct miscdevice pvsched_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pvsched",
	.fops = &pvsched_fops,
	.mode = 0600,
};

static int __init pvsched_init(void)
{
	struct pvsched_default_policy_config config =
		pvsched_default_policy_get_config();
	int ret;

	/* Refuse to load with an invalid configuration; never clamp it. */
	if (!pvsched_default_policy_config_valid(&config) ||
	    !cs_budget_us || !generic_budget_us || idle_hold_us > cs_budget_us) {
		pr_err("invalid policy configuration\n");
		return -EINVAL;
	}
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
