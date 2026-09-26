// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/time64.h>
#include <trace/events/sched.h>

#include <asm/kvm_pvsched.h>

#include "event_service.h"
#include "internal.h"

struct pvsched_holder_thread {
	struct completion *start;
	struct completion *held;
	struct completion *release;
	int ret;
};

/* Race the first get and the last put against another holder. */
static int pvsched_holder_thread_fn(void *data)
{
	struct pvsched_holder_thread *ctx = data;

	wait_for_completion(ctx->start);
	ctx->ret = pvsched_event_service_get();
	complete(ctx->held);
	wait_for_completion(ctx->release);
	if (!ctx->ret)
		pvsched_event_service_put();
	return 0;
}

static void pvsched_event_service_first_last_race_test(struct kunit *test)
{
	struct completion start, held, release;
	struct task_struct *tasks[2];
	struct pvsched_holder_thread ctx[2] = { };
	int i;

	init_completion(&start);
	init_completion(&held);
	init_completion(&release);
	for (i = 0; i < ARRAY_SIZE(ctx); i++) {
		ctx[i].start = &start;
		ctx[i].held = &held;
		ctx[i].release = &release;
		tasks[i] = kthread_run(pvsched_holder_thread_fn, &ctx[i],
				       "pvsched-holder-%d", i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(tasks[i]));
		/* The thread may exit on its own before kthread_stop(). */
		get_task_struct(tasks[i]);
	}
	complete_all(&start);
	wait_for_completion(&held);
	wait_for_completion(&held);
	KUNIT_EXPECT_EQ(test, ctx[0].ret, 0);
	KUNIT_EXPECT_EQ(test, ctx[1].ret, 0);
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), 2U);
	complete_all(&release);
	for (i = 0; i < ARRAY_SIZE(ctx); i++) {
		KUNIT_EXPECT_EQ(test, kthread_stop(tasks[i]), 0);
		put_task_struct(tasks[i]);
	}
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), 0U);
}

/* Published runtimes need a mapped page; these tests never inspect it. */
static union pvsched_vcpu_page pvsched_event_service_page;

struct pvsched_exit_thread_ctx {
	struct completion ready;
	struct completion exit;
};

static int pvsched_exit_thread(void *data)
{
	struct pvsched_exit_thread_ctx *ctx = data;

	complete(&ctx->ready);
	wait_for_completion(&ctx->exit);
	return 0;
}

static void pvsched_event_service_real_exit_test(struct kunit *test)
{
	struct pvsched_policy_entry *entry = pvsched_policy_default_entry();
	struct pvsched_runner_runtime *runtime;
	struct pvsched_session session = { };
	struct pvsched_vcpu_runner runner = { };
	struct pvsched_exit_thread_ctx exit_ctx;
	struct task_struct *task;
	unsigned int holders;
	int ret;

	init_completion(&exit_ctx.ready);
	init_completion(&exit_ctx.exit);
	task = kthread_run(pvsched_exit_thread, &exit_ctx,
			   "pvsched-exit-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(task));
	/* The thread exits on its own; keep it valid for kthread_stop(). */
	get_task_struct(task);
	wait_for_completion(&exit_ctx.ready);
	mutex_init(&session.lock);
	INIT_LIST_HEAD(&session.runners);
	KUNIT_ASSERT_EQ(test, pvsched_runner_lifecycle_init(&runner, &session,
			entry->ops->params_size), 0);
	runner.pid = get_task_pid(task, PIDTYPE_PID);
	list_add_tail(&runner.session_node, &session.runners);
	runtime = kzalloc_obj(*runtime);
	KUNIT_ASSERT_NOT_NULL(test, runtime);
	ret = pvsched_runner_runtime_prepare(runtime, task,
			entry, NSEC_PER_SEC,
			NSEC_PER_SEC, &runner.ticket_owner, true);
	KUNIT_ASSERT_EQ(test, ret, 0);
	runtime->attachment.shm.addr = &pvsched_event_service_page;
	holders = pvsched_event_service_holders();
	mutex_lock(&session.lock);
	ret = pvsched_runner_publish_runtime_locked(&runner, runtime, NULL, NULL);
	mutex_unlock(&session.lock);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders + 1);
	complete(&exit_ctx.exit);
	KUNIT_EXPECT_EQ(test, kthread_stop(task), 0);
	put_task_struct(task);
	flush_work(&runner.cleanup_work);
	KUNIT_EXPECT_PTR_EQ(test, runner.runtime, NULL);
	KUNIT_EXPECT_TRUE(test, runner.exited);
	/* Exit teardown gave back the reference that publication took. */
	KUNIT_EXPECT_EQ(test, pvsched_event_service_holders(), holders);

	cancel_work_sync(&runner.cleanup_work);
	put_pid(runner.pid);
	pvsched_runner_lifecycle_destroy(&runner);
}

/* SVM AVIC and vNMI only withhold the ticket; other unknown modes revoke. */
static void pvsched_event_service_mode_flags_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(0), 0);
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(KVM_PVSCHED_MODE_NESTED),
			PVSCHED_RUNNER_MODE_NESTED);
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(KVM_PVSCHED_MODE_SVM_AVIC),
			PVSCHED_RUNNER_MODE_NO_TICKET);
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(KVM_PVSCHED_MODE_SVM_VNMI),
			PVSCHED_RUNNER_MODE_NO_TICKET);
	KUNIT_EXPECT_EQ(test,
			pvsched_mode_flags(KVM_PVSCHED_MODE_SVM_AVIC |
					   KVM_PVSCHED_MODE_SVM_VNMI |
					   KVM_PVSCHED_MODE_NESTED),
			PVSCHED_RUNNER_MODE_NO_TICKET |
			PVSCHED_RUNNER_MODE_NESTED);
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(KVM_PVSCHED_MODE_PROTECTED),
			PVSCHED_RUNNER_MODE_UNSUPPORTED);
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(KVM_PVSCHED_MODE_VMX_SW_RMODE),
			PVSCHED_RUNNER_MODE_UNSUPPORTED);
	KUNIT_EXPECT_EQ(test,
			pvsched_mode_flags(KVM_PVSCHED_MODE_SVM_AVIC |
					   KVM_PVSCHED_MODE_PROTECTED),
			PVSCHED_RUNNER_MODE_NO_TICKET |
			PVSCHED_RUNNER_MODE_UNSUPPORTED);
	/* A bit this translation does not know is never treated as benign. */
	KUNIT_EXPECT_EQ(test, pvsched_mode_flags(BIT(31)),
			PVSCHED_RUNNER_MODE_UNSUPPORTED);
}

static struct kunit_case pvsched_event_service_test_cases[] = {
	KUNIT_CASE(pvsched_event_service_mode_flags_test),
	KUNIT_CASE(pvsched_event_service_first_last_race_test),
	KUNIT_CASE(pvsched_event_service_real_exit_test),
	{}
};

static struct kunit_suite pvsched_event_service_test_suite = {
	.name = "pvsched-event-service",
	.test_cases = pvsched_event_service_test_cases,
};

kunit_test_suite(pvsched_event_service_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
