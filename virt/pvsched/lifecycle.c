// SPDX-License-Identifier: GPL-2.0-only

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sched/task.h>
#include <linux/workqueue.h>
#include <kunit/visibility.h>

#include "internal.h"
#include "event_service.h"
#include "lifecycle.h"
#include "negotiation.h"
#include "shm_bridge.h"

/* A reattach must restore the original baseline, not adopt a new one. */
static bool pvsched_runner_baseline_differs(const struct pvsched_vcpu_runner *runner,
					    const struct pvsched_runner_runtime *runtime)
{
	return runner->baseline_valid &&
	       memcmp(runner->baseline, runtime->baseline,
		      runner->baseline_size);
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

/*
 * Free a closed, unhashed and drained runtime.  Only a published runtime is
 * ever installed in runner->runtime, and publishing took the event service.
 */
static void pvsched_runner_free_runtime_locked(struct pvsched_vcpu_runner *runner)
{
	struct pvsched_runner_runtime *runtime = runner->runtime;

	pvsched_event_service_put();
	pvsched_shm_release(&runtime->attachment.shm);
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

/*
 * Allocate the runner's persistent baseline, @baseline_size bytes of the
 * session policy's parameters, here rather than at the first attachment,
 * so that its size is fixed for the runner's life.
 */
int pvsched_runner_lifecycle_init(struct pvsched_vcpu_runner *runner,
				  struct pvsched_session *session,
				  u32 baseline_size)
{
	runner->baseline = kzalloc(baseline_size, GFP_KERNEL_ACCOUNT);
	if (!runner->baseline)
		return -ENOMEM;
	runner->baseline_size = baseline_size;
	runner->session = session;
	INIT_WORK(&runner->cleanup_work, pvsched_runner_cleanup_work);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_lifecycle_init);

void pvsched_runner_lifecycle_destroy(struct pvsched_vcpu_runner *runner)
{
	kfree(runner->baseline);
	runner->baseline = NULL;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_lifecycle_destroy);

int pvsched_runner_publish_runtime_locked(struct pvsched_vcpu_runner *runner,
					  struct pvsched_runner_runtime *runtime,
					  pvsched_attachment_commit_fn commit,
					  void *data)
{
	int ret;

	if (runner->runtime)
		return -EEXIST;
	if (WARN_ON_ONCE(runtime->ops->params_size != runner->baseline_size))
		return -EINVAL;
	if (pvsched_runner_baseline_differs(runner, runtime))
		return -EBUSY;
	runtime->attachment.runner = runner;
	if (runner->last_fault_valid)
		runtime->last_fault = runner->last_fault;
	ret = pvsched_event_service_get();
	if (ret)
		return ret;
	ret = pvsched_runner_runtime_publish(runtime, commit, data);
	if (ret) {
		pvsched_event_service_put();
		return ret;
	}
	if (!runner->baseline_valid) {
		memcpy(runner->baseline, runtime->baseline, runner->baseline_size);
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

/*
 * Prepare one ATTACH without making anything visible: bind the policy and
 * capture its baseline, admit and pin the shared page, and decide the
 * negotiation from one private snapshot.  The page is not written here.
 * The caller copies its output first, then either publishes an ENABLED
 * preparation with pvsched_runner_publish_prepared_locked() or rejects any
 * other with pvsched_runner_reject_prepared_locked().
 *
 * The session mutex must be held from prepare through the publish, reject
 * or discard that ends it.  runner->runtime is only set at publish, so this
 * is what keeps a second ATTACH of the same runner from pinning another
 * page meanwhile, and so bounds a session's pages by its runner limit.
 * The mutex is therefore taken before mmap_lock here and before page locks
 * when the page is later unpinned.  None of this runs under an attachment
 * state_lock.
 */
int pvsched_runner_prepare_attach_locked(struct pvsched_vcpu_runner *runner,
					 struct task_struct *task,
					 const struct pvsched_attach_params *params,
					 struct pvsched_runner_runtime **prepared,
					 enum pvsched_status *status)
{
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;
	struct pvsched_runner_runtime *runtime;
	struct pvsched_shm *shm;
	int ret;

	lockdep_assert_held(&runner->session->lock);
	if (runner->runtime)
		return -EEXIST;
	runtime = kzalloc(sizeof(*runtime), GFP_KERNEL_ACCOUNT);
	if (!runtime)
		return -ENOMEM;
	ret = pvsched_runner_runtime_prepare(runtime, task,
					     params->entry, params->cs_budget_ns,
					     params->generic_budget_ns,
					     &runner->ticket_owner,
					     params->deboost_notify);
	if (ret) {
		/* A failed prepare has already undone itself. */
		kfree(runtime);
		return ret;
	}
	runtime->idle_hold_ns = params->idle_hold_ns;
	/*
	 * The capture is compared here, before a page is pinned, and copied into
	 * the runner only at publish.
	 */
	if (runtime->ops->params_size != runner->baseline_size) {
		ret = -EINVAL;
		goto err_discard;
	}
	if (pvsched_runner_baseline_differs(runner, runtime)) {
		ret = -EBUSY;
		goto err_discard;
	}
	/*
	 * The page is guest memory in the VMM, which is the vCPU thread's own
	 * address space; pin and charge there, whoever issued the ATTACH.
	 */
	shm = &runtime->attachment.shm;
	ret = pvsched_shm_prepare(shm, runtime->attachment.owner_mm,
				  params->user_addr, params->cap_ipc_lock,
				  params->memlock_limit_pages);
	if (ret)
		goto err_discard;
	pvsched_shm_bridge_read_negotiation(shm, &request, &guest);
	runtime->negotiation = pvsched_negotiate(&request, &guest, runtime->ops);
	*status = runtime->negotiation;
	*prepared = runtime;
	return 0;

err_discard:
	/* The page is not pinned yet, or its failed prepare released it. */
	pvsched_runner_discard_prepared_locked(runtime);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_prepare_attach_locked);

/* publish() commit callback: the response and ENABLED become visible last. */
static void pvsched_runner_commit_enabled(struct pvsched_attachment *attachment,
					  void *data)
{
	pvsched_shm_bridge_publish_response(&attachment->shm,
					    PVSCHED_STATUS_ENABLED);
}

/* Publish a prepared runtime; only an ENABLED negotiation may serve. */
int pvsched_runner_publish_prepared_locked(struct pvsched_vcpu_runner *runner,
					   struct pvsched_runner_runtime *runtime)
{
	if (WARN_ON_ONCE(runtime->negotiation != PVSCHED_STATUS_ENABLED))
		return -EINVAL;
	return pvsched_runner_publish_runtime_locked(runner, runtime,
						     pvsched_runner_commit_enabled,
						     NULL);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_publish_prepared_locked);

/* Undo a prepared, never-published runtime. */
void pvsched_runner_discard_prepared_locked(struct pvsched_runner_runtime *runtime)
{
	pvsched_shm_release(&runtime->attachment.shm);
	pvsched_runner_runtime_release(runtime);
	kfree(runtime);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_discard_prepared_locked);

/* Publish the completed rejection on the page, then undo the preparation. */
void pvsched_runner_reject_prepared_locked(struct pvsched_runner_runtime *runtime)
{
	if (!WARN_ON_ONCE(runtime->negotiation == PVSCHED_STATUS_ENABLED))
		pvsched_shm_bridge_publish_response(&runtime->attachment.shm,
						    runtime->negotiation);
	pvsched_runner_discard_prepared_locked(runtime);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_runner_reject_prepared_locked);
