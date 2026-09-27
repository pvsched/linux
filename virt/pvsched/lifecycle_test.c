// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <kunit/static_stub.h>
#include <linux/completion.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/string.h>

#include "event_service.h"
#include "internal.h"
#include "lifecycle.h"
#include "shm.h"

struct pvsched_lifecycle_test_ctx {
	struct pvsched_session session;
	struct pvsched_vcpu_runner runner;
	void *test_state;
	/* The KUnit thread's address space, standing in for the VMM's. */
	struct mm_struct *mm;
};

struct pvsched_lifecycle_test_state {
	u32 drain_calls;
	u32 owner_snapshots;
	/* A local event run from inside publication, and its result. */
	int visit_key;
	int visit_ret;
	bool fail_setter;
	struct pvsched_vcpu_runner *checkpoint_runner;
	enum pvsched_lifecycle_test_point checkpoint;
	u32 checkpoint_calls;
	bool checkpoint_exit;
	bool block_checkpoint;
	bool block_work;
	struct completion checkpoint_entered;
	struct completion work_entered;
	struct completion allow_work;
	struct completion final_disabled;
	/* Shared-page stubs for the ATTACH preparation tests. */
	union pvsched_vcpu_page *shm_page;
	long pin_ret;
	u32 unpin_calls;
	bool unpin_dirty;
};

struct pvsched_lifecycle_release_ctx {
	struct pvsched_session *session;
	struct completion done;
};

static int pvsched_lifecycle_test_thread(void *unused)
{
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

static int pvsched_lifecycle_release_thread(void *data)
{
	struct pvsched_lifecycle_release_ctx *ctx = data;

	mutex_lock(&ctx->session->lock);
	ctx->session->closing = true;
	mutex_unlock(&ctx->session->lock);
	pvsched_session_release_runtimes(ctx->session);
	complete(&ctx->done);
	return 0;
}

static void pvsched_lifecycle_test_drain(void)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_lifecycle_test_state *state = test->priv;

	state->drain_calls++;
	synchronize_rcu();
}

static void pvsched_lifecycle_test_owner_exit(struct task_struct *task,
					      struct mm_struct **mm,
					      struct pid **tgid,
					      bool *exiting)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_lifecycle_test_state *state = test->priv;

	state->owner_snapshots++;
	pvsched_attachment_mark_exited(task, NULL, NULL);
	*mm = task->mm;
	*tgid = task_tgid(task);
	*exiting = true;
}

/* Run a local event after task-hash insertion, before admission opens. */
static void pvsched_lifecycle_test_owner_visit(struct task_struct *task,
					       struct mm_struct **mm,
					       struct pid **tgid,
					       bool *exiting)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_lifecycle_test_state *state = test->priv;

	state->visit_ret = pvsched_runner_local_event(&state->visit_key,
		PVSCHED_RECONCILE_CANCEL, 0, false, false);
	task_lock(task);
	*mm = task->mm;
	*tgid = task_tgid(task);
	*exiting = false;
	task_unlock(task);
}

static int pvsched_lifecycle_test_setattr(struct task_struct *task,
					  const struct sched_attr *attr, void *data)
{
	struct pvsched_lifecycle_test_state *state = data;

	if (state->fail_setter)
		return -EIO;
	return sched_setattr_nocheck_nopi(task, attr);
}

static void
pvsched_lifecycle_test_checkpoint(struct pvsched_vcpu_runner *runner,
				  enum pvsched_lifecycle_test_point checkpoint,
				  void *data)
{
	struct pvsched_lifecycle_test_state *state = data;

	if (runner != state->checkpoint_runner || checkpoint != state->checkpoint)
		goto concurrency;
	if (state->checkpoint_calls)
		return;
	state->checkpoint_calls++;
	if (state->checkpoint_exit)
		pvsched_attachment_mark_exited(runner->runtime->attachment.task,
					       NULL, NULL);
	if (state->block_checkpoint) {
		complete(&state->checkpoint_entered);
		wait_for_completion(&state->allow_work);
	}
	return;
concurrency:
	if (checkpoint == PVSCHED_CLEANUP_WORK_ENTER && state->block_work) {
		complete(&state->work_entered);
		wait_for_completion(&state->allow_work);
	}
	if (checkpoint == PVSCHED_CLOSE_FINAL_DISABLE)
		complete(&state->final_disabled);
}

static int pvsched_lifecycle_test_init(struct kunit *test)
{
	test->priv = kunit_kzalloc(test, sizeof(struct pvsched_lifecycle_test_state),
				   GFP_KERNEL);
	if (test->priv) {
		struct pvsched_lifecycle_test_state *state = test->priv;

		init_completion(&state->work_entered);
		init_completion(&state->checkpoint_entered);
		init_completion(&state->allow_work);
		init_completion(&state->final_disabled);
	}
	return test->priv ? 0 : -ENOMEM;
}

/* Published runtimes need a mapped page; these tests never inspect it. */
static union pvsched_vcpu_page pvsched_lifecycle_page;

static struct pvsched_default_policy_config pvsched_lifecycle_config(void)
{
	return (struct pvsched_default_policy_config) {
		.cs_rt_prio = 60,
		.deadline_rt_prio = 50,
		.guest_rt_cap = 50,
	};
}

static int pvsched_lifecycle_attach(struct pvsched_lifecycle_test_ctx *ctx,
				    struct pvsched_vcpu_runner *runner,
				    struct task_struct *task)
{
	struct pvsched_default_policy_config config = pvsched_lifecycle_config();
	struct pvsched_runner_runtime *runtime;
	int ret;

	runtime = kzalloc_obj(*runtime);
	if (!runtime)
		return -ENOMEM;
	ret = pvsched_runner_runtime_prepare(runtime, task, &config,
					     NSEC_PER_SEC, NSEC_PER_SEC,
					     &runner->ticket_owner, true);
	if (ret)
		goto free;
	runtime->test_setattr = pvsched_lifecycle_test_setattr;
	runtime->test_setattr_data = ctx->test_state;
	runtime->attachment.shm.addr = &pvsched_lifecycle_page;
	mutex_lock(&ctx->session.lock);
	ret = pvsched_runner_publish_runtime_locked(runner, runtime,
						    NULL, NULL);
	mutex_unlock(&ctx->session.lock);
	if (!ret)
		return 0;
	pvsched_runner_runtime_release(runtime);
free:
	kfree(runtime);
	return ret;
}

static void pvsched_lifecycle_cleanup(void *data)
{
	struct pvsched_lifecycle_test_ctx *ctx = data;

	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	cancel_work_sync(&ctx->runner.cleanup_work);
	put_pid(ctx->runner.pid);
}

static struct pvsched_lifecycle_test_ctx *
pvsched_lifecycle_setup(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	mutex_init(&ctx->session.lock);
	INIT_LIST_HEAD(&ctx->session.runners);
	ctx->test_state = test->priv;
	ctx->runner.pid = get_task_pid(current, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&ctx->runner, &ctx->session);
	list_add(&ctx->runner.session_node, &ctx->session.runners);
	if (kunit_add_action_or_reset(test, pvsched_lifecycle_cleanup, ctx))
		return NULL;
	ret = pvsched_lifecycle_attach(ctx, &ctx->runner, current);
	if (ret) {
		KUNIT_FAIL(test, "failed to attach private runtime: %d", ret);
		return NULL;
	}
	return ctx;
}

static void pvsched_lifecycle_history_reattach_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	u32 state, flags;
	int fault;

	if (!ctx)
		return;
	ctx->runner.runtime->last_fault = -EIO;
	mutex_lock(&ctx->session.lock);
	pvsched_runner_query_locked(&ctx->runner, &state, &flags, &fault);
	KUNIT_EXPECT_EQ(test, state, (u32)PVSCHED_RUNNER_ACTIVE);
	KUNIT_EXPECT_EQ(test, fault, -EIO);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test,
			pvsched_lifecycle_attach(ctx, &ctx->runner, current), 0);
	mutex_lock(&ctx->session.lock);
	pvsched_runner_query_locked(&ctx->runner, &state, &flags, &fault);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, state, (u32)PVSCHED_RUNNER_ACTIVE);
	KUNIT_EXPECT_EQ(test, flags, (u32)PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID);
	KUNIT_EXPECT_EQ(test, fault, -EIO);
}

static void pvsched_lifecycle_binding_work_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	int first, second;

	if (!ctx)
		return;
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_local_visit(&first,
							 PVSCHED_RUNNER_HOST, 0,
							 NULL, NULL));
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_local_visit(&second,
							 PVSCHED_RUNNER_HOST, 0,
							 NULL, NULL));
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void pvsched_lifecycle_exit_work_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);

	if (!ctx)
		return;
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_mark_exited(current,
							      NULL, NULL));
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
}

static void pvsched_lifecycle_stale_work_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	unsigned long flags;

	if (!ctx)
		return;
	mutex_lock(&ctx->session.lock);
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock, flags);
	pvsched_runner_request_cleanup_locked(&ctx->runner.runtime->attachment);
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   flags);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void pvsched_lifecycle_mode_work_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_default_guest_area guest = { };
	struct pvsched_runner_event_input input = {
		.guest = &guest,
		.mode_flags = PVSCHED_RUNNER_MODE_UNSUPPORTED,
	};

	if (!ctx)
		return;
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(ctx->runner.runtime,
						       PVSCHED_RECONCILE_RUN_ENTER,
						       &input), -EOPNOTSUPP);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void pvsched_lifecycle_external_owner_work_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_default_guest_area guest = { };
	struct pvsched_runner_event_input input = { .guest = &guest };
	int baseline_nice;

	if (!ctx)
		return;
	baseline_nice = ctx->runner.runtime->baseline.nice;
	set_user_nice(current, min(baseline_nice + 1, MAX_NICE));
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(ctx->runner.runtime,
						       PVSCHED_RECONCILE_RUN_ENTER,
						       &input), -EOWNERDEAD);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	set_user_nice(current, baseline_nice);
}

static void pvsched_lifecycle_final_close_one_drain_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_vcpu_runner second = { };
	struct task_struct *task;
	unsigned int holders;
	int ret;

	if (!ctx)
		return;
	task = kthread_run(pvsched_lifecycle_test_thread, NULL,
			   "pvsched-lifecycle");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	second.pid = get_task_pid(task, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&second, &ctx->session);
	list_add(&second.session_node, &ctx->session.runners);
	ret = pvsched_lifecycle_attach(ctx, &second, task);
	if (ret) {
		KUNIT_FAIL(test, "failed to attach second runtime: %d", ret);
		list_del(&second.session_node);
		put_pid(second.pid);
		kthread_stop(task);
		return;
	}
	kunit_activate_static_stub(test, pvsched_attachment_drain,
				   pvsched_lifecycle_test_drain);
	holders = pvsched_event_service_holders();
	KUNIT_EXPECT_GE(test, holders, 2U);
	ctx->session.closing = true;
	pvsched_session_release_runtimes(&ctx->session);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_PTR_EQ(test, second.runtime, NULL);
	KUNIT_EXPECT_EQ(test, state->drain_calls, 1U);
	/* Final close gave back both runtimes' references. */
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders - 2);
	list_del(&second.session_node);
	cancel_work_sync(&second.cleanup_work);
	put_pid(second.pid);
	kthread_stop(task);
}

static void pvsched_lifecycle_publish_exit_stale_work_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_default_policy_config config = pvsched_lifecycle_config();
	struct pvsched_session session = { };
	struct pvsched_vcpu_runner runner = { };
	struct pvsched_runner_runtime runtime;
	unsigned int holders = pvsched_event_service_holders();
	int ret;

	mutex_init(&session.lock);
	INIT_LIST_HEAD(&session.runners);
	runner.pid = get_task_pid(current, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&runner, &session);
	list_add(&runner.session_node, &session.runners);
	ret = pvsched_runner_runtime_prepare(&runtime, current, &config,
					     NSEC_PER_SEC, NSEC_PER_SEC,
					     &runner.ticket_owner, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	kunit_activate_static_stub(test, pvsched_attachment_drain,
				   pvsched_lifecycle_test_drain);
	kunit_activate_static_stub(test, pvsched_attachment_owner_snapshot,
				   pvsched_lifecycle_test_owner_exit);
	mutex_lock(&session.lock);
	runtime.attachment.shm.addr = &pvsched_lifecycle_page;
	ret = pvsched_runner_publish_runtime_locked(&runner, &runtime, NULL,
						    NULL);
	mutex_unlock(&session.lock);
	KUNIT_EXPECT_EQ(test, ret, -ESRCH);
	KUNIT_EXPECT_PTR_EQ(test, runner.runtime, NULL);
	KUNIT_EXPECT_EQ(test, state->drain_calls, 1U);
	/* The failed publication gave its event-service reference back. */
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders);
	pvsched_runner_runtime_release(&runtime);
	flush_work(&runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, runner.runtime, NULL);
	list_del(&runner.session_node);
	put_pid(runner.pid);
}

/*
 * A local event that finds the task hashed but not yet admitted must not
 * request teardown: publication completes and the runner stays served.
 */
static void pvsched_lifecycle_publish_insert_visit_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_default_policy_config config = pvsched_lifecycle_config();
	struct pvsched_session session = { };
	struct pvsched_vcpu_runner runner = { };
	struct pvsched_runner_runtime *runtime;
	unsigned int holders = pvsched_event_service_holders();
	int ret;

	mutex_init(&session.lock);
	INIT_LIST_HEAD(&session.runners);
	runner.pid = get_task_pid(current, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&runner, &session);
	list_add(&runner.session_node, &session.runners);
	runtime = kzalloc_obj(*runtime);
	KUNIT_ASSERT_NOT_NULL(test, runtime);
	ret = pvsched_runner_runtime_prepare(runtime, current, &config,
					     NSEC_PER_SEC, NSEC_PER_SEC,
					     &runner.ticket_owner, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	kunit_activate_static_stub(test, pvsched_attachment_owner_snapshot,
				   pvsched_lifecycle_test_owner_visit);
	runtime->attachment.shm.addr = &pvsched_lifecycle_page;
	mutex_lock(&session.lock);
	ret = pvsched_runner_publish_runtime_locked(&runner, runtime, NULL,
						    NULL);
	mutex_unlock(&session.lock);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->visit_ret, -ESHUTDOWN);
	flush_work(&runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, runner.runtime, runtime);
	KUNIT_EXPECT_TRUE(test, runtime->attachment.active);
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders + 1);

	mutex_lock(&session.lock);
	pvsched_runner_detach_runtime_locked(&runner);
	mutex_unlock(&session.lock);
	/* Detach gave back the reference that publication took. */
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders);
	cancel_work_sync(&runner.cleanup_work);
	list_del(&runner.session_node);
	put_pid(runner.pid);
}

static void pvsched_lifecycle_publish_without_page_test(struct kunit *test)
{
	struct pvsched_default_policy_config config = pvsched_lifecycle_config();
	unsigned int holders = pvsched_event_service_holders();
	struct pvsched_session session = { };
	struct pvsched_vcpu_runner runner = { };
	struct pvsched_runner_runtime runtime;
	int ret;

	mutex_init(&session.lock);
	INIT_LIST_HEAD(&session.runners);
	runner.pid = get_task_pid(current, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&runner, &session);
	list_add(&runner.session_node, &session.runners);
	ret = pvsched_runner_runtime_prepare(&runtime, current, &config,
					     NSEC_PER_SEC, NSEC_PER_SEC,
					     &runner.ticket_owner, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* Expected WARN: the probes would read a page that is not mapped. */
	mutex_lock(&session.lock);
	ret = pvsched_runner_publish_runtime_locked(&runner, &runtime, NULL,
						    NULL);
	mutex_unlock(&session.lock);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, runner.runtime, NULL);
	KUNIT_EXPECT_FALSE(test, runtime.attachment.task_hashed);
	KUNIT_EXPECT_FALSE(test, runtime.attachment.vcpu_hashed);
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders);
	pvsched_runner_runtime_release(&runtime);
	list_del(&runner.session_node);
	put_pid(runner.pid);
}

static void pvsched_lifecycle_duplicate_zero_drain_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_default_policy_config config = pvsched_lifecycle_config();
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_vcpu_runner second = { };
	struct pvsched_runner_runtime duplicate;
	int ret;

	if (!ctx)
		return;
	second.pid = get_task_pid(current, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&second, &ctx->session);
	list_add(&second.session_node, &ctx->session.runners);
	ret = pvsched_runner_runtime_prepare(&duplicate, current, &config,
					     NSEC_PER_SEC, NSEC_PER_SEC,
					     &second.ticket_owner, true);
	if (ret) {
		KUNIT_FAIL(test, "failed to prepare duplicate runtime: %d", ret);
		list_del(&second.session_node);
		cancel_work_sync(&second.cleanup_work);
		put_pid(second.pid);
		return;
	}
	kunit_activate_static_stub(test, pvsched_attachment_drain,
				   pvsched_lifecycle_test_drain);
	mutex_lock(&ctx->session.lock);
	duplicate.attachment.shm.addr = &pvsched_lifecycle_page;
	ret = pvsched_runner_publish_runtime_locked(&second, &duplicate,
						    NULL, NULL);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, ret, -EEXIST);
	KUNIT_EXPECT_EQ(test, state->drain_calls, 0U);
	KUNIT_EXPECT_PTR_EQ(test, second.runtime, NULL);
	KUNIT_EXPECT_FALSE(test, duplicate.attachment.task_hashed);
	pvsched_runner_runtime_release(&duplicate);
	list_del(&second.session_node);
	cancel_work_sync(&second.cleanup_work);
	put_pid(second.pid);
}

static void pvsched_lifecycle_query_converges_exit_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	u32 state, flags;
	int fault;

	if (!ctx)
		return;
	ctx->runner.runtime->last_fault = -EIO;
	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_mark_exited(current,
							      NULL, NULL));
	pvsched_runner_query_locked(&ctx->runner, &state, &flags, &fault);
	mutex_unlock(&ctx->session.lock);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_EQ(test, state, (u32)PVSCHED_RUNNER_EXITED);
	KUNIT_EXPECT_EQ(test, flags, (u32)PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID);
	KUNIT_EXPECT_EQ(test, fault, -EIO);
}

static void pvsched_lifecycle_target_state_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_default_guest_area guest = { };
	struct pvsched_runner_event_input input = { .guest = &guest };
	struct pvsched_vcpu_runner runner = { };
	struct sched_task_state before, boosted, restored;
	struct task_struct *task;
	int ret;

	if (!ctx)
		return;
	task = kthread_run(pvsched_lifecycle_test_thread, NULL,
			   "pvsched-lifecycle-state");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	runner.pid = get_task_pid(task, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&runner, &ctx->session);
	list_add(&runner.session_node, &ctx->session.runners);
	sched_get_task_state(task, &before);
	ret = pvsched_lifecycle_attach(ctx, &runner, task);
	if (ret)
		goto out;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	ret = pvsched_runner_reconcile(runner.runtime,
				       PVSCHED_RECONCILE_RUN_ENTER, &input);
	sched_get_task_state(task, &boosted);
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&runner);
	mutex_unlock(&ctx->session.lock);
	sched_get_task_state(task, &restored);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, boosted.policy, SCHED_FIFO);
	KUNIT_EXPECT_EQ(test, boosted.rt_priority, 60);
	KUNIT_EXPECT_EQ(test, restored.policy, before.policy);
	KUNIT_EXPECT_EQ(test, restored.nice, before.nice);
out:
	if (ret)
		KUNIT_FAIL(test, "target runtime setup/reconcile failed: %d", ret);
	list_del(&runner.session_node);
	cancel_work_sync(&runner.cleanup_work);
	put_pid(runner.pid);
	kthread_stop(task);
}

static void pvsched_lifecycle_failed_restore_then_exit_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *test_state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_default_guest_area guest = { };
	struct pvsched_runner_event_input input = { .guest = &guest };
	unsigned long flags;
	bool released;

	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(ctx->runner.runtime,
						      PVSCHED_RECONCILE_RUN_ENTER,
						      &input), 0);
	test_state->fail_setter = true;
	mutex_lock(&ctx->session.lock);
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock, flags);
	pvsched_runner_request_cleanup_locked(&ctx->runner.runtime->attachment);
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   flags);
	released = pvsched_runner_cleanup_once_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_FALSE(test, released);
	KUNIT_ASSERT_NOT_NULL(test, ctx->runner.runtime);
	KUNIT_EXPECT_TRUE(test, ctx->runner.runtime->restore_failed);
	test_state->fail_setter = false;
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_mark_exited(current,
							      NULL, NULL));
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
	KUNIT_EXPECT_EQ(test, ctx->runner.last_fault, -EIO);
}

static void pvsched_lifecycle_exit_during_detach_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);

	if (!ctx)
		return;
	state->checkpoint_runner = &ctx->runner;
	state->checkpoint = PVSCHED_CLOSE_AFTER_DISABLE;
	state->checkpoint_exit = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_EQ(test, state->checkpoint_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
}

static void pvsched_lifecycle_exit_during_failed_restore_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_default_guest_area guest = { };
	struct pvsched_runner_event_input input = { .guest = &guest };
	unsigned long flags;

	if (!ctx)
		return;
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(ctx->runner.runtime,
						      PVSCHED_RECONCILE_RUN_ENTER,
						      &input), 0);
	state->fail_setter = true;
	state->checkpoint_runner = &ctx->runner;
	state->checkpoint = PVSCHED_CLOSE_AFTER_FINISH;
	state->checkpoint_exit = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	mutex_lock(&ctx->session.lock);
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock, flags);
	pvsched_runner_request_cleanup_locked(&ctx->runner.runtime->attachment);
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   flags);
	KUNIT_EXPECT_FALSE(test,
			   pvsched_runner_cleanup_once_locked(&ctx->runner));
	mutex_unlock(&ctx->session.lock);
	state->fail_setter = false;
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_EQ(test, state->checkpoint_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
	KUNIT_EXPECT_EQ(test, ctx->runner.last_fault, -EIO);
}

static void pvsched_lifecycle_exit_across_final_disable_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);

	if (!ctx)
		return;
	state->checkpoint_runner = &ctx->runner;
	state->checkpoint = PVSCHED_CLOSE_FINAL_DISABLE;
	state->checkpoint_exit = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	ctx->session.closing = true;
	pvsched_session_release_runtimes(&ctx->session);
	KUNIT_EXPECT_EQ(test, state->checkpoint_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
}

static void pvsched_lifecycle_detach_vs_worker_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	unsigned long flags;
	unsigned long waited;

	if (!ctx)
		return;
	state->block_work = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock, flags);
	pvsched_runner_request_cleanup_locked(&ctx->runner.runtime->attachment);
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   flags);
	waited = wait_for_completion_timeout(&state->work_entered, 5 * HZ);
	if (!waited) {
		complete(&state->allow_work);
		flush_work(&ctx->runner.cleanup_work);
		KUNIT_FAIL(test, "cleanup worker did not reach entry hook");
		return;
	}
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	complete(&state->allow_work);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
}

static void pvsched_lifecycle_final_disable_joins_worker_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_lifecycle_release_ctx release = { };
	struct task_struct *task;
	unsigned long flags;
	unsigned long waited;

	if (!ctx)
		return;
	release.session = &ctx->session;
	init_completion(&release.done);
	state->block_work = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock, flags);
	pvsched_runner_request_cleanup_locked(&ctx->runner.runtime->attachment);
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   flags);
	waited = wait_for_completion_timeout(&state->work_entered, 5 * HZ);
	if (!waited) {
		complete(&state->allow_work);
		flush_work(&ctx->runner.cleanup_work);
		KUNIT_FAIL(test, "cleanup worker did not reach entry hook");
		return;
	}
	task = kthread_run(pvsched_lifecycle_release_thread, &release,
			   "pvsched-final-release");
	if (IS_ERR(task)) {
		complete(&state->allow_work);
		flush_work(&ctx->runner.cleanup_work);
		KUNIT_FAIL(test, "failed to create final-release thread: %ld",
			   PTR_ERR(task));
		return;
	}
	/* The thread exits on its own; keep it valid for kthread_stop(). */
	get_task_struct(task);
	waited = wait_for_completion_timeout(&state->final_disabled, 5 * HZ);
	if (!waited) {
		complete(&state->allow_work);
		kthread_stop(task);
		put_task_struct(task);
		KUNIT_FAIL(test, "final release did not disable runtime");
		return;
	}
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_mark_exited(current,
							     NULL, NULL));
	KUNIT_EXPECT_FALSE(test, completion_done(&release.done));
	complete(&state->allow_work);
	waited = wait_for_completion_timeout(&release.done, 5 * HZ);
	if (!waited)
		KUNIT_FAIL(test, "final release did not join cleanup worker");
	kthread_stop(task);
	put_task_struct(task);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
}

static void pvsched_lifecycle_worker_failure_exit_requeue_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	unsigned long flags;
	unsigned long waited;

	if (!ctx)
		return;
	ctx->runner.runtime->restore_failed = true;
	ctx->runner.runtime->last_fault = -EIO;
	state->checkpoint_runner = &ctx->runner;
	state->checkpoint = PVSCHED_CLOSE_AFTER_FINISH;
	state->block_checkpoint = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock, flags);
	pvsched_runner_request_cleanup_locked(&ctx->runner.runtime->attachment);
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   flags);
	waited = wait_for_completion_timeout(&state->checkpoint_entered, 5 * HZ);
	if (!waited) {
		complete(&state->allow_work);
		flush_work(&ctx->runner.cleanup_work);
		KUNIT_FAIL(test, "cleanup worker did not reach restore disposition");
		return;
	}
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_mark_exited(current,
							     NULL, NULL));
	complete(&state->allow_work);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, ctx->runner.exited);
	KUNIT_EXPECT_TRUE(test, ctx->runner.last_fault_valid);
	KUNIT_EXPECT_EQ(test, ctx->runner.last_fault, -EIO);
}

static void pvsched_lifecycle_internal_fault_query_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_setup(test);
	struct pvsched_default_guest_area guest = { };
	struct pvsched_runner_event_input input = { .guest = &guest };
	struct pvsched_runner_vmentry_input entry = { };
	struct pvsched_host_area host = { };
	unsigned long irqflags;
	u32 state, flags;
	bool teardown_requested;
	int fault;

	if (!ctx)
		return;
	KUNIT_ASSERT_EQ(test, pvsched_runner_reconcile(ctx->runner.runtime,
						      PVSCHED_RECONCILE_RUN_ENTER,
						      &input), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_vmentry(ctx->runner.runtime,
						    &entry, &host), 0);
	mutex_lock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, pvsched_runner_reconcile(ctx->runner.runtime,
						       PVSCHED_RECONCILE_RUN_ENTER,
						       &input), -EINVAL);
	raw_spin_lock_irqsave(&ctx->runner.runtime->attachment.state_lock,
				irqflags);
	teardown_requested = ctx->runner.runtime->attachment.teardown_requested;
	raw_spin_unlock_irqrestore(&ctx->runner.runtime->attachment.state_lock,
				   irqflags);
	KUNIT_EXPECT_TRUE(test, teardown_requested);
	KUNIT_EXPECT_FALSE(test, ctx->runner.runtime->attachment.active);
	KUNIT_EXPECT_FALSE(test, ctx->runner.runtime->restore_failed);
	pvsched_runner_query_locked(&ctx->runner, &state, &flags, &fault);
	mutex_unlock(&ctx->session.lock);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_EQ(test, state, (u32)PVSCHED_RUNNER_INACTIVE);
	KUNIT_EXPECT_EQ(test, flags, (u32)PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID);
	KUNIT_EXPECT_EQ(test, fault, -EINVAL);
}

static int pvsched_lifecycle_shm_mapping(struct mm_struct *mm,
					 unsigned long start, unsigned long end)
{
	return 0;
}

static long pvsched_lifecycle_shm_pin(struct mm_struct *mm,
				      unsigned long user_addr,
				      struct page **page)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_lifecycle_test_state *state = test->priv;

	mmap_assert_locked(mm);
	if (state->pin_ret == 1)
		*page = virt_to_page(state->shm_page);
	return state->pin_ret;
}

static void pvsched_lifecycle_shm_unpin(struct page *page, bool dirty)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_lifecycle_test_state *state = test->priv;

	state->unpin_calls++;
	state->unpin_dirty = dirty;
}

/* A runner and session with no runtime; the page is a guest request. */
static struct pvsched_lifecycle_test_ctx *
pvsched_lifecycle_attach_setup(struct kunit *test)
{
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_lifecycle_test_ctx *ctx;
	struct pvsched_header *header;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	/* ATTACH pins in the vCPU thread's own mm; a KUnit kthread needs one. */
	KUNIT_ASSERT_EQ(test, kunit_attach_mm(), 0);
	ctx->mm = current->mm;
	state->shm_page = (void *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NOT_NULL(test, state->shm_page);
	state->pin_ret = 1;
	header = &state->shm_page->header;
	header->abi_version = cpu_to_le32(PVSCHED_ABI_VERSION);
	header->policy_version = cpu_to_le32(PVSCHED_DEFAULT_POLICY_VERSION);
	strscpy(header->policy_name, PVSCHED_DEFAULT_POLICY_NAME,
		sizeof(header->policy_name));
	header->protocol_id = cpu_to_le32(PVSCHED_PROTOCOL_DEFAULT);
	header->requested_mode = cpu_to_le32(PVSCHED_MODE_FRAMEWORK);
	/* A sentinel shows whether the host wrote a response. */
	header->status = cpu_to_le32(0x77);
	mutex_init(&ctx->session.lock);
	INIT_LIST_HEAD(&ctx->session.runners);
	ctx->session.owner_mm = ctx->mm;
	ctx->session.owner_tgid = task_tgid(current);
	ctx->test_state = state;
	ctx->runner.pid = get_task_pid(current, PIDTYPE_PID);
	pvsched_runner_lifecycle_init(&ctx->runner, &ctx->session);
	list_add(&ctx->runner.session_node, &ctx->session.runners);
	kunit_activate_static_stub(test, pvsched_shm_mapping_admit,
				   pvsched_lifecycle_shm_mapping);
	kunit_activate_static_stub(test, pvsched_shm_pin_page,
				   pvsched_lifecycle_shm_pin);
	kunit_activate_static_stub(test, pvsched_shm_unpin_page,
				   pvsched_lifecycle_shm_unpin);
	return ctx;
}

static void pvsched_lifecycle_attach_teardown(struct pvsched_lifecycle_test_ctx *ctx)
{
	struct pvsched_lifecycle_test_state *state = ctx->test_state;

	cancel_work_sync(&ctx->runner.cleanup_work);
	put_pid(ctx->runner.pid);
	free_page((unsigned long)state->shm_page);
}

/* Callers hold the session mutex until publish, reject or discard. */
static int
pvsched_lifecycle_prepare_locked(struct pvsched_lifecycle_test_ctx *ctx,
				 struct pvsched_runner_runtime **runtime,
				 enum pvsched_status *status)
{
	struct pvsched_default_policy_config config = pvsched_lifecycle_config();
	struct pvsched_attach_params params = {
		.user_addr = 0x100000,
		.config = &config,
		.cs_budget_ns = NSEC_PER_SEC,
		.generic_budget_ns = NSEC_PER_SEC,
		.deboost_notify = true,
		.idle_hold_ns = 150 * NSEC_PER_USEC,
		.cap_ipc_lock = true,
	};

	return pvsched_runner_prepare_attach_locked(&ctx->runner, current,
						    &params, runtime, status);
}

static void pvsched_lifecycle_prepare_commit_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;
	struct pvsched_runner_runtime *runtime;
	enum pvsched_status status;

	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), 0);
	KUNIT_EXPECT_EQ(test, status, PVSCHED_STATUS_ENABLED);
	/* Preparation writes nothing and publishes nothing. */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status), 0x77U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, runtime->attachment.shm.service_charged);
	/* The page is pinned in the vCPU thread's address space. */
	KUNIT_EXPECT_PTR_EQ(test, runtime->attachment.shm.mm, ctx->mm);
	/* The global idle-hold bound reaches the runtime. */
	KUNIT_EXPECT_EQ(test, runtime->idle_hold_ns, 150ULL * NSEC_PER_USEC);
	KUNIT_EXPECT_EQ(test, pvsched_runner_publish_prepared_locked(&ctx->runner,
			runtime), 0);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, runtime);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_ENABLED);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version),
			(u32)PVSCHED_ABI_VERSION);

	/* DETACH tells the guest, then releases the page exactly once. */
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	KUNIT_EXPECT_TRUE(test, state->unpin_dirty);
	pvsched_lifecycle_attach_teardown(ctx);
}

static void pvsched_lifecycle_prepare_reject_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;
	struct pvsched_runner_runtime *runtime;
	enum pvsched_status status;

	page->header.requested_mode = cpu_to_le32(PVSCHED_MODE_POLICY);
	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), 0);
	KUNIT_EXPECT_EQ(test, status, PVSCHED_STATUS_MODE_MISMATCH);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status), 0x77U);
	pvsched_runner_reject_prepared_locked(runtime);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_MODE_MISMATCH);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version),
			(u32)PVSCHED_ABI_VERSION);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	pvsched_lifecycle_attach_teardown(ctx);
}

/* A failed copyout discards the preparation without any page response. */
static void pvsched_lifecycle_prepare_discard_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;
	struct pvsched_runner_runtime *runtime;
	enum pvsched_status status;

	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), 0);
	pvsched_runner_discard_prepared_locked(runtime);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status), 0x77U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version), 0U);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	pvsched_lifecycle_attach_teardown(ctx);
}

static void pvsched_lifecycle_prepare_failure_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	struct pvsched_runner_runtime *runtime = NULL, *existing;
	enum pvsched_status status;

	/* A failed pin unwinds every charge and leaves no runtime. */
	state->pin_ret = -EFAULT;
	mutex_lock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), -EFAULT);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_PTR_EQ(test, runtime, NULL);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(state->shm_page->header.status), 0x77U);

	/* An attached runner rejects a second ATTACH before any resource. */
	state->pin_ret = 1;
	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &existing,
							       &status), 0);
	KUNIT_ASSERT_EQ(test, pvsched_runner_publish_prepared_locked(&ctx->runner,
			existing), 0);
	KUNIT_EXPECT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), -EEXIST);
	KUNIT_EXPECT_TRUE(test, existing->attachment.shm.service_charged);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	pvsched_lifecycle_attach_teardown(ctx);
}

/* Prepare and publish one ENABLED attachment on the stubbed page. */
static struct pvsched_runner_runtime *
pvsched_lifecycle_prepare_published(struct kunit *test,
				    struct pvsched_lifecycle_test_ctx *ctx)
{
	struct pvsched_runner_runtime *runtime;
	enum pvsched_status status;

	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), 0);
	KUNIT_ASSERT_EQ(test, status, PVSCHED_STATUS_ENABLED);
	KUNIT_ASSERT_EQ(test, pvsched_runner_publish_prepared_locked(&ctx->runner,
			runtime), 0);
	mutex_unlock(&ctx->session.lock);
	return runtime;
}

static void pvsched_lifecycle_disabled_once_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;
	struct pvsched_runner_runtime *runtime;

	runtime = pvsched_lifecycle_prepare_published(test, ctx);
	pvsched_attachment_disable(&runtime->attachment);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	/* Later disables and the close sequence must not rewrite the status. */
	page->header.status = cpu_to_le32(0x55);
	pvsched_attachment_disable(&runtime->attachment);
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status), 0x55U);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	pvsched_lifecycle_attach_teardown(ctx);
}

static void pvsched_lifecycle_exit_disables_page_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;

	pvsched_lifecycle_prepare_published(test, ctx);
	/*
	 * The page stubs redirect only this KUnit thread, so hold the cleanup
	 * worker at entry and release the page from here instead.
	 */
	state->block_work = true;
	ctx->runner.lifecycle_test_hook = pvsched_lifecycle_test_checkpoint;
	ctx->runner.lifecycle_test_data = state;
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_mark_exited(current, NULL, NULL));
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&state->work_entered,
							   5 * HZ));
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	complete(&state->allow_work);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	pvsched_lifecycle_attach_teardown(ctx);
}

static void pvsched_lifecycle_owned_failure_keeps_page_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;
	struct pvsched_runner_runtime *runtime;
	unsigned long flags;

	runtime = pvsched_lifecycle_prepare_published(test, ctx);
	/* A recorded owned-restore failure makes automatic cleanup retain. */
	raw_spin_lock_irqsave(&runtime->attachment.state_lock, flags);
	runtime->restore_failed = true;
	pvsched_runner_request_cleanup_locked(&runtime->attachment);
	raw_spin_unlock_irqrestore(&runtime->attachment.state_lock, flags);
	flush_work(&ctx->runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, runtime);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 0U);
	KUNIT_EXPECT_TRUE(test, runtime->attachment.shm.service_charged);
	/* An explicit DETACH releases it once. */
	mutex_lock(&ctx->session.lock);
	pvsched_runner_detach_runtime_locked(&ctx->runner);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	pvsched_lifecycle_attach_teardown(ctx);
}

static void pvsched_lifecycle_publish_failure_discard_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;
	struct pvsched_runner_runtime *runtime = NULL;
	enum pvsched_status status;

	/* A changed baseline on reattach fails before any page is pinned. */
	ctx->runner.baseline_valid = true;
	ctx->runner.baseline.policy = SCHED_NORMAL;
	ctx->runner.baseline.nice = 19;
	mutex_lock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), -EBUSY);
	mutex_unlock(&ctx->session.lock);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 0U);
	ctx->runner.baseline_valid = false;

	/* An exit found after task-hash insertion fails publication. */
	mutex_lock(&ctx->session.lock);
	KUNIT_ASSERT_EQ(test, pvsched_lifecycle_prepare_locked(ctx, &runtime,
							       &status), 0);
	kunit_activate_static_stub(test, pvsched_attachment_owner_snapshot,
				   pvsched_lifecycle_test_owner_exit);
	KUNIT_EXPECT_EQ(test, pvsched_runner_publish_prepared_locked(&ctx->runner,
			runtime), -ESRCH);
	pvsched_runner_discard_prepared_locked(runtime);
	mutex_unlock(&ctx->session.lock);
	kunit_deactivate_static_stub(test, pvsched_attachment_owner_snapshot);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status), 0x77U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version), 0U);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	pvsched_lifecycle_attach_teardown(ctx);
}

static void pvsched_lifecycle_final_close_releases_page_test(struct kunit *test)
{
	struct pvsched_lifecycle_test_ctx *ctx = pvsched_lifecycle_attach_setup(test);
	struct pvsched_lifecycle_test_state *state = test->priv;
	union pvsched_vcpu_page *page = state->shm_page;

	pvsched_lifecycle_prepare_published(test, ctx);
	mutex_lock(&ctx->session.lock);
	ctx->session.closing = true;
	mutex_unlock(&ctx->session.lock);
	pvsched_session_release_runtimes(&ctx->session);
	KUNIT_EXPECT_PTR_EQ(test, ctx->runner.runtime, NULL);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	pvsched_lifecycle_attach_teardown(ctx);
}

static struct kunit_case pvsched_lifecycle_test_cases[] = {
	KUNIT_CASE(pvsched_lifecycle_prepare_commit_test),
	KUNIT_CASE(pvsched_lifecycle_prepare_reject_test),
	KUNIT_CASE(pvsched_lifecycle_prepare_discard_test),
	KUNIT_CASE(pvsched_lifecycle_prepare_failure_test),
	KUNIT_CASE(pvsched_lifecycle_disabled_once_test),
	KUNIT_CASE(pvsched_lifecycle_exit_disables_page_test),
	KUNIT_CASE(pvsched_lifecycle_owned_failure_keeps_page_test),
	KUNIT_CASE(pvsched_lifecycle_publish_failure_discard_test),
	KUNIT_CASE(pvsched_lifecycle_final_close_releases_page_test),
	KUNIT_CASE(pvsched_lifecycle_history_reattach_test),
	KUNIT_CASE(pvsched_lifecycle_binding_work_test),
	KUNIT_CASE(pvsched_lifecycle_exit_work_test),
	KUNIT_CASE(pvsched_lifecycle_stale_work_test),
	KUNIT_CASE(pvsched_lifecycle_mode_work_test),
	KUNIT_CASE(pvsched_lifecycle_external_owner_work_test),
	KUNIT_CASE(pvsched_lifecycle_final_close_one_drain_test),
	KUNIT_CASE(pvsched_lifecycle_publish_exit_stale_work_test),
	KUNIT_CASE(pvsched_lifecycle_publish_insert_visit_test),
	KUNIT_CASE(pvsched_lifecycle_publish_without_page_test),
	KUNIT_CASE(pvsched_lifecycle_duplicate_zero_drain_test),
	KUNIT_CASE(pvsched_lifecycle_query_converges_exit_test),
	KUNIT_CASE(pvsched_lifecycle_target_state_test),
	KUNIT_CASE(pvsched_lifecycle_failed_restore_then_exit_test),
	KUNIT_CASE(pvsched_lifecycle_exit_during_detach_test),
	KUNIT_CASE(pvsched_lifecycle_exit_during_failed_restore_test),
	KUNIT_CASE(pvsched_lifecycle_exit_across_final_disable_test),
	KUNIT_CASE(pvsched_lifecycle_detach_vs_worker_test),
	KUNIT_CASE(pvsched_lifecycle_final_disable_joins_worker_test),
	KUNIT_CASE(pvsched_lifecycle_worker_failure_exit_requeue_test),
	KUNIT_CASE(pvsched_lifecycle_internal_fault_query_test),
	{}
};

static struct kunit_suite pvsched_lifecycle_test_suite = {
	.name = "pvsched-lifecycle",
	.init = pvsched_lifecycle_test_init,
	.test_cases = pvsched_lifecycle_test_cases,
};

kunit_test_suite(pvsched_lifecycle_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched session lifecycle");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
