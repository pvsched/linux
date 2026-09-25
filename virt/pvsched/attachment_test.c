// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/sched.h>

#include "attachment.h"

struct pvsched_attachment_test_ctx {
	struct pvsched_attachment attachment;
	unsigned int visits;
	bool live;
};

struct pvsched_attachment_visit_record {
	unsigned int visits;
	enum pvsched_attachment_visit_kind kind;
};

struct pvsched_attachment_commit_record {
	struct kunit *test;
	bool called;
};

static void pvsched_attachment_test_visit(struct pvsched_attachment *attachment,
					  enum pvsched_attachment_visit_kind kind,
					  void *data)
{
	unsigned int *visits = data;

	(*visits)++;
}

static void
pvsched_attachment_test_record_visit(struct pvsched_attachment *attachment,
				     enum pvsched_attachment_visit_kind kind,
				     void *data)
{
	struct pvsched_attachment_visit_record *record = data;

	record->visits++;
	record->kind = kind;
}

static void pvsched_attachment_test_commit(struct pvsched_attachment *attachment,
					   void *data)
{
	struct pvsched_attachment_commit_record *record = data;

	KUNIT_EXPECT_TRUE(record->test, attachment->active);
	KUNIT_EXPECT_TRUE(record->test, attachment->task_hashed);
	record->called = true;
}

static bool pvsched_attachment_test_local(const void *key,
					  enum pvsched_runner_position position,
					  unsigned int *visits)
{
	return pvsched_attachment_local_visit(key, position, 0,
		visits ? pvsched_attachment_test_visit : NULL, visits);
}

static unsigned int pvsched_attachment_test_remote(const void *key,
						   bool supported, bool nested,
						   unsigned int *visits)
{
	return pvsched_attachment_remote_visit(key,
		(supported ? 0 : PVSCHED_RUNNER_MODE_UNSUPPORTED) |
		(nested ? PVSCHED_RUNNER_MODE_NESTED : 0),
		visits ? pvsched_attachment_test_visit : NULL, visits);
}

/* Refuse service, remove reachability, then wait before dropping pins. */
static void pvsched_attachment_destroy(struct pvsched_attachment *attachment)
{
	pvsched_attachment_disable(attachment);
	pvsched_attachment_unhash(attachment);
	pvsched_attachment_drain();
	pvsched_attachment_release(attachment);
}

static void pvsched_attachment_test_cleanup(void *data)
{
	struct pvsched_attachment_test_ctx *ctx = data;

	if (ctx->live)
		pvsched_attachment_destroy(&ctx->attachment);
}

static struct pvsched_attachment_test_ctx *
pvsched_attachment_test_setup(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		KUNIT_FAIL(test, "failed to allocate attachment context");
		return NULL;
	}
	ret = pvsched_attachment_init(&ctx->attachment, current);
	if (ret) {
		KUNIT_FAIL(test, "attachment init failed: %d", ret);
		return NULL;
	}
	ctx->live = true;
	ret = pvsched_attachment_publish(&ctx->attachment, NULL, NULL);
	if (ret) {
		KUNIT_FAIL(test, "attachment publish failed: %d", ret);
		pvsched_attachment_test_cleanup(ctx);
		ctx->live = false;
		return NULL;
	}
	ret = kunit_add_action_or_reset(test, pvsched_attachment_test_cleanup,
					ctx);
	if (ret) {
		KUNIT_FAIL(test, "failed to register attachment cleanup: %d", ret);
		return NULL;
	}
	return ctx;
}

static void pvsched_attachment_bind_and_position_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	unsigned int visited = 0;
	bool found;
	int key;

	if (!ctx)
		return;
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_HOST,
					      &ctx->visits);
	KUNIT_ASSERT_TRUE(test, found);
	KUNIT_EXPECT_EQ(test, ctx->attachment.position, PVSCHED_RUNNER_HOST);
	KUNIT_EXPECT_EQ(test, ctx->visits, 1U);
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 1U);
	KUNIT_EXPECT_EQ(test, ctx->visits, 2U);
	visited = pvsched_attachment_test_remote(&key, false, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	visited = pvsched_attachment_test_remote(&key, true, true,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
}

static void pvsched_attachment_run_boundary_disable_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	struct pvsched_attachment_visit_record record = { };
	unsigned int visited = 0;
	bool found;
	int key;

	if (!ctx)
		return;
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_HOST, NULL);
	KUNIT_ASSERT_TRUE(test, found);
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_QEMU, NULL);
	KUNIT_ASSERT_TRUE(test, found);
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_HOST, NULL);
	KUNIT_ASSERT_TRUE(test, found);
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 1U);

	pvsched_attachment_disable(&ctx->attachment);
	KUNIT_EXPECT_EQ(test, ctx->attachment.position, PVSCHED_RUNNER_HOST);
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	found = pvsched_attachment_local_visit(&key, PVSCHED_RUNNER_HOST, 0,
					     pvsched_attachment_test_record_visit,
					     &record);
	KUNIT_EXPECT_TRUE(test, found);
	KUNIT_EXPECT_EQ(test, record.visits, 1U);
	KUNIT_EXPECT_EQ(test, record.kind, PVSCHED_ATTACHMENT_VISIT_CLEANUP);
	found = pvsched_attachment_cleanup_visit(current,
						 pvsched_attachment_test_visit,
						 &ctx->visits);
	KUNIT_EXPECT_TRUE(test, found);
}

static void pvsched_attachment_changed_key_fails_closed_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	unsigned int visited;
	bool found;
	int first_key, replacement_key;

	if (!ctx)
		return;
	found = pvsched_attachment_test_local(&first_key, PVSCHED_RUNNER_HOST,
					      NULL);
	KUNIT_ASSERT_TRUE(test, found);
	found = pvsched_attachment_test_local(&replacement_key,
					      PVSCHED_RUNNER_HOST, NULL);
	KUNIT_EXPECT_TRUE(test, found);
	KUNIT_EXPECT_FALSE(test, ctx->attachment.active);
	KUNIT_EXPECT_TRUE(test, ctx->attachment.binding_failed);
	KUNIT_EXPECT_PTR_EQ(test, ctx->attachment.vcpu.key, &first_key);
	visited = pvsched_attachment_test_remote(&first_key, true, false, NULL);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	found = pvsched_attachment_cleanup_visit(current,
						 pvsched_attachment_test_visit,
						 &ctx->visits);
	KUNIT_EXPECT_TRUE(test, found);
	KUNIT_EXPECT_EQ(test, ctx->visits, 1U);
}

static void pvsched_attachment_disabled_unbound_cannot_bind_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	unsigned int visited;
	int key;

	if (!ctx)
		return;
	pvsched_attachment_disable(&ctx->attachment);
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_test_local(&key,
			PVSCHED_RUNNER_HOST, &ctx->visits));
	KUNIT_EXPECT_PTR_EQ(test, ctx->attachment.vcpu.key, NULL);
	KUNIT_EXPECT_FALSE(test, ctx->attachment.vcpu_hashed);
	visited = pvsched_attachment_test_remote(&key, true, false, NULL);
	KUNIT_EXPECT_EQ(test, visited, 0U);
}

static void pvsched_attachment_null_key_fails_closed_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	struct pvsched_attachment_visit_record record = { };

	if (!ctx)
		return;
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_local_visit(NULL,
		PVSCHED_RUNNER_HOST, 0, pvsched_attachment_test_record_visit,
		&record));
	KUNIT_EXPECT_FALSE(test, ctx->attachment.active);
	KUNIT_EXPECT_TRUE(test, ctx->attachment.binding_failed);
	KUNIT_EXPECT_EQ(test, record.kind,
			PVSCHED_ATTACHMENT_VISIT_BINDING_FAILED);
	KUNIT_EXPECT_PTR_EQ(test, ctx->attachment.vcpu.key, NULL);
	KUNIT_EXPECT_FALSE(test, ctx->attachment.vcpu_hashed);
	KUNIT_EXPECT_EQ(test, record.visits, 1U);
}

static void pvsched_attachment_target_mode_gate_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	unsigned int visited;
	int key;

	if (!ctx)
		return;
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_local_visit(&key,
		PVSCHED_RUNNER_HOST, PVSCHED_RUNNER_MODE_NESTED, NULL, NULL));
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	KUNIT_ASSERT_TRUE(test, pvsched_attachment_local_visit(&key,
		PVSCHED_RUNNER_HOST, PVSCHED_RUNNER_MODE_NO_TICKET, NULL, NULL));
	visited = pvsched_attachment_test_remote(&key, false, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 0U);
	visited = pvsched_attachment_test_remote(&key, true, false,
						 &ctx->visits);
	KUNIT_EXPECT_EQ(test, visited, 1U);
}

static void pvsched_attachment_exit_latch_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *ctx =
		pvsched_attachment_test_setup(test);
	bool found;
	int key;

	if (!ctx)
		return;
	KUNIT_EXPECT_TRUE(test, pvsched_attachment_mark_exited(current));
	KUNIT_EXPECT_TRUE(test, ctx->attachment.exited);
	KUNIT_EXPECT_FALSE(test, ctx->attachment.active);
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_HOST, NULL);
	KUNIT_EXPECT_FALSE(test, found);
	found = pvsched_attachment_cleanup_visit(current,
						 NULL, NULL);
	KUNIT_EXPECT_TRUE(test, found);
}

static void pvsched_attachment_publish_exit_rollback_test(struct kunit *test)
{
	struct pvsched_attachment attachment;

	KUNIT_ASSERT_EQ(test, pvsched_attachment_init(&attachment, current), 0);
	attachment.exited = true;
	KUNIT_EXPECT_EQ(test, pvsched_attachment_publish(&attachment, NULL, NULL),
			-ESRCH);
	KUNIT_EXPECT_FALSE(test, attachment.task_hashed);
	/* publish() inserted before its final exit check, then unhashed and drained. */
	pvsched_attachment_release(&attachment);
}

static void pvsched_attachment_publish_commit_test(struct kunit *test)
{
	struct pvsched_attachment attachment;
	struct pvsched_attachment_commit_record record = {
		.test = test,
	};

	KUNIT_ASSERT_EQ(test, pvsched_attachment_init(&attachment, current), 0);
	KUNIT_ASSERT_EQ(test, pvsched_attachment_publish(&attachment,
			pvsched_attachment_test_commit, &record), 0);
	KUNIT_EXPECT_TRUE(test, record.called);
	KUNIT_EXPECT_TRUE(test, attachment.active);
	pvsched_attachment_destroy(&attachment);
}

static void pvsched_attachment_split_release_test(struct kunit *test)
{
	struct pvsched_attachment attachment;

	KUNIT_ASSERT_EQ(test, pvsched_attachment_init(&attachment, current), 0);
	KUNIT_ASSERT_EQ(test, pvsched_attachment_publish(&attachment, NULL, NULL), 0);
	pvsched_attachment_disable(&attachment);
	pvsched_attachment_unhash(&attachment);
	pvsched_attachment_drain();
	pvsched_attachment_release(&attachment);
	KUNIT_EXPECT_FALSE(test, attachment.task_hashed);
	KUNIT_EXPECT_PTR_EQ(test, attachment.task, NULL);
	KUNIT_EXPECT_PTR_EQ(test, attachment.owner_mm, NULL);
	KUNIT_EXPECT_PTR_EQ(test, attachment.owner_tgid, NULL);
}

static void pvsched_attachment_stale_local_candidate_test(struct kunit *test)
{
	struct pvsched_attachment_test_ctx *old =
		pvsched_attachment_test_setup(test);
	struct pvsched_attachment new;
	unsigned int visited = 0;
	bool found;
	int key;

	if (!old)
		return;
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_HOST, NULL);
	KUNIT_ASSERT_TRUE(test, found);
	pvsched_attachment_disable(&old->attachment);

	/* A disabled holder keeps the exact-task reservation until unhash. */
	KUNIT_ASSERT_EQ(test, pvsched_attachment_init(&new, current), 0);
	KUNIT_EXPECT_EQ(test, pvsched_attachment_publish(&new, NULL, NULL), -EEXIST);
	pvsched_attachment_unhash(&old->attachment);
	pvsched_attachment_drain();
	KUNIT_ASSERT_EQ(test, pvsched_attachment_publish(&new, NULL, NULL), 0);
	found = pvsched_attachment_test_local(&key, PVSCHED_RUNNER_HOST,
					      &visited);
	KUNIT_ASSERT_TRUE(test, found);
	KUNIT_EXPECT_EQ(test, visited, 1U);
	KUNIT_EXPECT_PTR_EQ(test, old->attachment.vcpu.key, &key);
	KUNIT_EXPECT_PTR_EQ(test, new.vcpu.key, &key);
	KUNIT_EXPECT_FALSE(test, old->attachment.active);
	KUNIT_EXPECT_TRUE(test, new.active);
	visited = pvsched_attachment_test_remote(&key, true, false,
						 NULL);
	KUNIT_EXPECT_EQ(test, visited, 1U);
	pvsched_attachment_destroy(&new);
}

static struct kunit_case pvsched_attachment_test_cases[] = {
	KUNIT_CASE(pvsched_attachment_bind_and_position_test),
	KUNIT_CASE(pvsched_attachment_run_boundary_disable_test),
	KUNIT_CASE(pvsched_attachment_changed_key_fails_closed_test),
	KUNIT_CASE(pvsched_attachment_disabled_unbound_cannot_bind_test),
	KUNIT_CASE(pvsched_attachment_null_key_fails_closed_test),
	KUNIT_CASE(pvsched_attachment_target_mode_gate_test),
	KUNIT_CASE(pvsched_attachment_exit_latch_test),
	KUNIT_CASE(pvsched_attachment_publish_exit_rollback_test),
	KUNIT_CASE(pvsched_attachment_publish_commit_test),
	KUNIT_CASE(pvsched_attachment_split_release_test),
	KUNIT_CASE(pvsched_attachment_stale_local_candidate_test),
	{}
};

static struct kunit_suite pvsched_attachment_test_suite = {
	.name = "pvsched-attachment",
	.test_cases = pvsched_attachment_test_cases,
};

kunit_test_suite(pvsched_attachment_test_suite);

MODULE_DESCRIPTION("KUnit tests for pvsched attachment hashes");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
