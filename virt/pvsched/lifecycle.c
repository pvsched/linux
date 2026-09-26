// SPDX-License-Identifier: GPL-2.0-only

#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/workqueue.h>
#include <kunit/visibility.h>

#include "internal.h"
#include "lifecycle.h"

static bool pvsched_baseline_equal(const struct sched_task_state *a,
				   const struct sched_task_state *b)
{
	return a->policy == b->policy && a->nice == b->nice &&
	       a->rt_priority == b->rt_priority &&
	       a->reset_on_fork == b->reset_on_fork &&
	       a->custom_slice == b->custom_slice && a->slice_ns == b->slice_ns &&
	       a->timer_slack_ns == b->timer_slack_ns &&
	       a->scx_active == b->scx_active;
}

#if IS_ENABLED(CONFIG_KUNIT)
static void
pvsched_lifecycle_test_hook(struct pvsched_vcpu_runner *runner,
			    enum pvsched_lifecycle_test_point point)
{
	if (runner->lifecycle_test_hook)
		runner->lifecycle_test_hook(runner, point,
					    runner->lifecycle_test_data);
}
#else
#define pvsched_lifecycle_test_hook(runner, point) do { } while (0)
#endif

static void pvsched_runner_copy_history_locked(struct pvsched_vcpu_runner *runner)
{
	struct pvsched_runner_runtime *runtime = runner->runtime;
	unsigned long flags;

	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	runner->exited |= runtime->attachment.exited;
	if (runtime->last_fault) {
		runner->last_fault = runtime->last_fault;
		runner->last_fault_valid = true;
	}
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
}

static bool
pvsched_runner_close_locked(struct pvsched_vcpu_runner *runner, bool final)
{
	struct pvsched_runner_runtime *runtime = runner->runtime;
	enum pvsched_runner_restore_disposition disposition;
	unsigned long flags;
	bool retained;

	if (!runtime)
		return false;
	pvsched_runner_runtime_disable(runtime);
	pvsched_lifecycle_test_hook(runner, PVSCHED_CLOSE_AFTER_DISABLE);
	pvsched_runner_runtime_drain_actions(runtime);
	disposition = pvsched_runner_runtime_finish_close(runtime, final);
	pvsched_lifecycle_test_hook(runner, PVSCHED_CLOSE_AFTER_FINISH);
	retained = !final && disposition == PVSCHED_RESTORE_OWNED_FAILURE;
	if (retained) {
		raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
		/*
		 * Consume the request that found the failure.  After disable,
		 * only exit can raise another, and that one is terminal.
		 */
		if (!runtime->attachment.exited)
			runtime->attachment.teardown_requested = false;
		raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	}
	pvsched_runner_copy_history_locked(runner);
	if (retained)
		return false;
	pvsched_runner_runtime_unhash(runtime);
	return true;
}

/* Free a closed, unhashed and drained runtime. */
static void pvsched_runner_free_runtime_locked(struct pvsched_vcpu_runner *runner)
{
	struct pvsched_runner_runtime *runtime = runner->runtime;

	pvsched_runner_runtime_release(runtime);
	kfree(runtime);
	runner->runtime = NULL;
}

bool pvsched_runner_cleanup_once_locked(struct pvsched_vcpu_runner *runner)
{
	struct pvsched_runner_runtime *runtime = runner->runtime;
	bool teardown_requested;
	unsigned long flags;

	if (!runtime || runner->session->closing)
		return false;
	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	teardown_requested = runtime->attachment.teardown_requested;
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	if (!teardown_requested ||
	    !pvsched_runner_close_locked(runner, false))
		return false;
	pvsched_attachment_drain();
	pvsched_runner_free_runtime_locked(runner);
	return true;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_cleanup_once_locked);

static void pvsched_runner_cleanup_work(struct work_struct *work)
{
	struct pvsched_vcpu_runner *runner =
		container_of(work, struct pvsched_vcpu_runner, cleanup_work);
	struct pvsched_session *session = runner->session;

	pvsched_lifecycle_test_hook(runner, PVSCHED_CLEANUP_WORK_ENTER);
	mutex_lock(&session->lock);
	pvsched_runner_cleanup_once_locked(runner);
	mutex_unlock(&session->lock);
}

void pvsched_runner_lifecycle_init(struct pvsched_vcpu_runner *runner,
				   struct pvsched_session *session)
{
	runner->session = session;
	INIT_WORK(&runner->cleanup_work, pvsched_runner_cleanup_work);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_lifecycle_init);

int pvsched_runner_publish_runtime_locked(struct pvsched_vcpu_runner *runner,
					  struct pvsched_runner_runtime *runtime,
					  pvsched_attachment_commit_fn commit,
					  void *data)
{
	int ret;

	if (runner->runtime)
		return -EEXIST;
	if (runner->baseline_valid &&
	    !pvsched_baseline_equal(&runner->baseline, &runtime->baseline))
		return -EBUSY;
	runtime->attachment.runner = runner;
	if (runner->last_fault_valid)
		runtime->last_fault = runner->last_fault;
	ret = pvsched_runner_runtime_publish(runtime, commit, data);
	if (ret)
		return ret;
	if (!runner->baseline_valid) {
		runner->baseline = runtime->baseline;
		runner->baseline_valid = true;
	}
	runner->runtime = runtime;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_publish_runtime_locked);

void pvsched_runner_request_cleanup_locked(struct pvsched_attachment *attachment)
{
	struct pvsched_vcpu_runner *runner = attachment->runner;

	if (!runner)
		return;
	attachment->teardown_requested = true;
	/* Final session close forbids any later enqueue. */
	if (!attachment->cleanup_enqueue_closed)
		queue_work(system_dfl_wq, &runner->cleanup_work);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_request_cleanup_locked);

void pvsched_session_release_runtimes(struct pvsched_session *session)
{
	struct pvsched_vcpu_runner *runner;
	struct pvsched_runner_runtime *runtime;
	unsigned long flags;
	bool unhashed = false;

	mutex_lock(&session->lock);
	list_for_each_entry(runner, &session->runners, session_node) {
		runtime = runner->runtime;
		if (!runtime)
			continue;
		raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
		runtime->attachment.cleanup_enqueue_closed = true;
		raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
		pvsched_runner_runtime_disable(runtime);
		pvsched_lifecycle_test_hook(runner, PVSCHED_CLOSE_FINAL_DISABLE);
	}
	mutex_unlock(&session->lock);

	lockdep_assert_not_held(&session->lock);
	list_for_each_entry(runner, &session->runners, session_node)
		cancel_work_sync(&runner->cleanup_work);

	mutex_lock(&session->lock);
	list_for_each_entry(runner, &session->runners, session_node)
		unhashed |= pvsched_runner_close_locked(runner, true);
	if (unhashed)
		pvsched_attachment_drain();
	list_for_each_entry(runner, &session->runners, session_node)
		if (runner->runtime)
			pvsched_runner_free_runtime_locked(runner);
	mutex_unlock(&session->lock);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_session_release_runtimes);

void pvsched_runner_query_locked(struct pvsched_vcpu_runner *runner,
				 u32 *state, u32 *flags, int *last_fault)
{
	struct pvsched_runner_runtime *runtime = runner->runtime;
	struct task_struct *task;
	unsigned long irqflags;

	*flags = 0;
	*last_fault = 0;
	/* Serve a pending automatic teardown before reporting. */
	if (runtime && pvsched_runner_cleanup_once_locked(runner))
		runtime = NULL;
	if (runtime) {
		raw_spin_lock_irqsave(&runtime->attachment.state_lock, irqflags);
		runner->exited |= runtime->attachment.exited;
		*state = runtime->attachment.exited ? PVSCHED_RUNNER_EXITED :
			(runtime->attachment.active ? PVSCHED_RUNNER_ACTIVE :
			 PVSCHED_RUNNER_INACTIVE);
		if (runtime->last_fault) {
			*flags = PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID;
			*last_fault = runtime->last_fault;
		}
		raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, irqflags);
		return;
	}
	task = get_pid_task(runner->pid, PIDTYPE_PID);
	if (!task || (READ_ONCE(task->flags) & PF_EXITING))
		runner->exited = true;
	if (task)
		put_task_struct(task);
	*state = runner->exited ? PVSCHED_RUNNER_EXITED :
		PVSCHED_RUNNER_INACTIVE;
	if (runner->last_fault_valid) {
		*flags = PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID;
		*last_fault = runner->last_fault;
	}
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_query_locked);

void pvsched_runner_detach_runtime_locked(struct pvsched_vcpu_runner *runner)
{
	if (!runner->runtime)
		return;
	pvsched_runner_close_locked(runner, true);
	pvsched_attachment_drain();
	pvsched_runner_free_runtime_locked(runner);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_detach_runtime_locked);
