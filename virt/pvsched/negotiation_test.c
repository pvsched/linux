// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "default_policy.h"
#include "negotiation.h"

static void pvsched_negotiation_valid(struct pvsched_negotiation_request *request,
				      struct pvsched_default_guest_area *guest)
{
	*request = (struct pvsched_negotiation_request) {
		.abi_version = PVSCHED_ABI_VERSION,
		.policy_version = PVSCHED_DEFAULT_POLICY_VERSION,
		.protocol_id = PVSCHED_PROTOCOL_DEFAULT,
		.requested_mode = PVSCHED_MODE_FRAMEWORK,
	};
	strscpy(request->policy_name, PVSCHED_DEFAULT_POLICY_NAME,
		sizeof(request->policy_name));
	memset(guest, 0, sizeof(*guest));
}

static void pvsched_negotiation_accept_test(struct kunit *test)
{
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;

	pvsched_negotiation_valid(&request, &guest);
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_ENABLED);
	/* Dynamic guest intent is judged at selection time, not here. */
	guest.cs_state = cpu_to_le64(PVSCHED_CS_NMI | PVSCHED_CS_HARDIRQ);
	guest.task_intent.current_task.sched_policy = 200;
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_ENABLED);
	/* A reattach may find an idle guest's published intent. */
	guest.task_intent.flags = PVSCHED_INTENT_FLAGS_VALID;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_ENABLED);
}

/* Each failure is rejected with its status even when later fields are bad too. */
static void pvsched_negotiation_order_test(struct kunit *test)
{
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;

	pvsched_negotiation_valid(&request, &guest);
	guest.interrupt_ack = cpu_to_le64(1);
	request.requested_mode = PVSCHED_MODE_POLICY;
	request.protocol_id = PVSCHED_PROTOCOL_CUSTOM;
	request.policy_version = 2;
	strscpy(request.policy_name, "custom", sizeof(request.policy_name));
	request.abi_version = 2;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_ABI_MISMATCH);
	request.abi_version = PVSCHED_ABI_VERSION;
	request.policy_name[sizeof(request.policy_name) - 1] = 'x';
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	request.policy_name[sizeof(request.policy_name) - 1] = 0;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_UNKNOWN_POLICY);
	strscpy(request.policy_name, PVSCHED_DEFAULT_POLICY_NAME,
		sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_POLICY_VERSION_MISMATCH);
	request.policy_version = PVSCHED_DEFAULT_POLICY_VERSION;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_PROTOCOL_MISMATCH);
	request.protocol_id = PVSCHED_PROTOCOL_DEFAULT;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_MODE_MISMATCH);
	request.requested_mode = PVSCHED_MODE_FRAMEWORK;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	guest.interrupt_ack = 0;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_ENABLED);
}

static void pvsched_negotiation_name_encoding_test(struct kunit *test)
{
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;

	pvsched_negotiation_valid(&request, &guest);
	memset(request.policy_name, 0, sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	/* Bytes after the terminator must be zero padding. */
	strscpy(request.policy_name, PVSCHED_DEFAULT_POLICY_NAME,
		sizeof(request.policy_name));
	request.policy_name[sizeof(PVSCHED_DEFAULT_POLICY_NAME) + 2] = 'z';
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	/* A full-width name has no terminator. */
	memset(request.policy_name, 'a', sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	/* A name that only starts with "default" is a different policy. */
	memset(request.policy_name, 0, sizeof(request.policy_name));
	strscpy(request.policy_name, "defaults", sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_UNKNOWN_POLICY);
}

static void pvsched_negotiation_guest_fields_test(struct kunit *test)
{
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;

	pvsched_negotiation_valid(&request, &guest);
	guest.cs_state = cpu_to_le64(PVSCHED_CS_RESERVED_MASK & (1ULL << 40));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	pvsched_negotiation_valid(&request, &guest);
	guest.task_intent.reserved = 1;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	pvsched_negotiation_valid(&request, &guest);
	guest.task_intent.flags = 0x80;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	pvsched_negotiation_valid(&request, &guest);
	guest.task_intent.flags = PVSCHED_INTENT_FLAG_IDLE << 1;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
	pvsched_negotiation_valid(&request, &guest);
	guest.reserved[sizeof(guest.reserved) - 1] = 1;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &pvsched_default_policy_ops),
			PVSCHED_STATUS_DISABLED);
}

static int pvsched_negotiation_test_calls;

static int pvsched_negotiation_test_refuse(const struct pvsched_negotiation_request *request)
{
	pvsched_negotiation_test_calls++;
	return request->policy_version == 7 ? -EPERM : 0;
}

/*
 * The session's policy decides: its name and version are the only ones
 * accepted, and its own check runs after every framework check.
 */
static void pvsched_negotiation_session_policy_test(struct kunit *test)
{
	struct pvsched_policy_ops ops = {
		.name = "kunit-negotiation",
		.version = 3,
		.protocol = PVSCHED_PROTOCOL_DEFAULT,
	};
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;

	/* The default policy is only one choice, not a fallback. */
	pvsched_negotiation_valid(&request, &guest);
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &ops),
			PVSCHED_STATUS_UNKNOWN_POLICY);
	strscpy(request.policy_name, "kunit-negotiation",
		sizeof(request.policy_name));
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest,
						&pvsched_default_policy_ops),
			PVSCHED_STATUS_UNKNOWN_POLICY);
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &ops),
			PVSCHED_STATUS_POLICY_VERSION_MISMATCH);
	request.policy_version = 3;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &ops),
			PVSCHED_STATUS_ENABLED);

	/* The policy's own check runs last, and only then. */
	ops.negotiate = pvsched_negotiation_test_refuse;
	pvsched_negotiation_test_calls = 0;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &ops),
			PVSCHED_STATUS_ENABLED);
	KUNIT_EXPECT_EQ(test, pvsched_negotiation_test_calls, 1);
	guest.interrupt_ack = cpu_to_le64(1);
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &ops),
			PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, pvsched_negotiation_test_calls, 1);
	guest.interrupt_ack = 0;
	ops.version = 7;
	request.policy_version = 7;
	KUNIT_EXPECT_EQ(test, pvsched_negotiate(&request, &guest, &ops),
			PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, pvsched_negotiation_test_calls, 2);
}

static struct kunit_case pvsched_negotiation_cases[] = {
	KUNIT_CASE(pvsched_negotiation_accept_test),
	KUNIT_CASE(pvsched_negotiation_order_test),
	KUNIT_CASE(pvsched_negotiation_name_encoding_test),
	KUNIT_CASE(pvsched_negotiation_guest_fields_test),
	KUNIT_CASE(pvsched_negotiation_session_policy_test),
	{}
};

static struct kunit_suite pvsched_negotiation_suite = {
	.name = "pvsched-negotiation",
	.test_cases = pvsched_negotiation_cases,
};

kunit_test_suite(pvsched_negotiation_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
