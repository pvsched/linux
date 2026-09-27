// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "shm_bridge.h"

/* A page-sized shared area is too large for the stack; allocate it. */
static union pvsched_vcpu_page *
pvsched_shm_bridge_test_page(struct kunit *test, struct pvsched_shm *shm)
{
	union pvsched_vcpu_page *page;

	page = kunit_kzalloc(test, sizeof(*page), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	*shm = (struct pvsched_shm) { .addr = page };
	return page;
}

static void pvsched_shm_bridge_snapshot_test(struct kunit *test)
{
	struct pvsched_shm shm;
	union pvsched_vcpu_page *page = pvsched_shm_bridge_test_page(test, &shm);
	struct pvsched_default_guest_area guest;

	page->guest_area.default_area.interrupt_ack =
		cpu_to_le64(0xfedcba9876543210ULL);
	page->guest_area.default_area.cs_state =
		cpu_to_le64(0x0123456789abcdefULL);
	page->guest_area.default_area.task_intent.raw =
		cpu_to_le64(0xa5a55a5ac3c33c3cULL);
	memset(page->guest_area.default_area.reserved, 0xa5,
	       sizeof(page->guest_area.default_area.reserved));
	pvsched_shm_bridge_snapshot(&shm, &guest);
	KUNIT_EXPECT_EQ(test, guest.interrupt_ack,
			cpu_to_le64(0xfedcba9876543210ULL));
	KUNIT_EXPECT_EQ(test, guest.cs_state,
			cpu_to_le64(0x0123456789abcdefULL));
	KUNIT_EXPECT_EQ(test, guest.task_intent.raw,
			cpu_to_le64(0xa5a55a5ac3c33c3cULL));
	KUNIT_EXPECT_MEMEQ(test, guest.reserved,
			   page->guest_area.default_area.reserved,
			   sizeof(guest.reserved));
}

static void pvsched_shm_bridge_publish_test(struct kunit *test)
{
	struct pvsched_shm shm;
	union pvsched_vcpu_page *page = pvsched_shm_bridge_test_page(test, &shm);
	struct pvsched_host_area host = { };

	host.applied_state.raw = cpu_to_le64(0x0123456789abcdefULL);
	host.default_area.interrupt_ticket =
		cpu_to_le64(0xf0e1d2c3b4a59687ULL);
	pvsched_shm_bridge_publish_vmentry(&shm, &host);
	KUNIT_EXPECT_EQ(test, page->host_area.applied_state.raw,
			cpu_to_le64(0x0123456789abcdefULL));
	KUNIT_EXPECT_EQ(test, page->host_area.default_area.interrupt_ticket,
			cpu_to_le64(0xf0e1d2c3b4a59687ULL));
}

static void pvsched_shm_bridge_negotiation_snapshot_test(struct kunit *test)
{
	struct pvsched_shm shm;
	union pvsched_vcpu_page *page = pvsched_shm_bridge_test_page(test, &shm);
	struct pvsched_negotiation_request request;
	struct pvsched_default_guest_area guest;

	page->header.abi_version = cpu_to_le32(1);
	page->header.policy_version = cpu_to_le32(7);
	strscpy(page->header.policy_name, "default",
		sizeof(page->header.policy_name));
	page->header.protocol_id = cpu_to_le32(PVSCHED_PROTOCOL_CUSTOM);
	page->header.requested_mode = cpu_to_le32(PVSCHED_MODE_POLICY);
	page->guest_area.default_area.cs_state = cpu_to_le64(PVSCHED_CS_NMI);
	pvsched_shm_bridge_read_negotiation(&shm, &request, &guest);
	KUNIT_EXPECT_EQ(test, request.abi_version, 1U);
	KUNIT_EXPECT_EQ(test, request.policy_version, 7U);
	KUNIT_EXPECT_STREQ(test, request.policy_name, "default");
	KUNIT_EXPECT_EQ(test, request.protocol_id, (u32)PVSCHED_PROTOCOL_CUSTOM);
	KUNIT_EXPECT_EQ(test, request.requested_mode, (u32)PVSCHED_MODE_POLICY);
	KUNIT_EXPECT_EQ(test, guest.cs_state, cpu_to_le64(PVSCHED_CS_NMI));
	/* The snapshot is private: later page changes do not reach it. */
	page->header.abi_version = cpu_to_le32(2);
	KUNIT_EXPECT_EQ(test, request.abi_version, 1U);
}

static void pvsched_shm_bridge_response_test(struct kunit *test)
{
	struct pvsched_shm shm;
	union pvsched_vcpu_page *page = pvsched_shm_bridge_test_page(test, &shm);

	/* Stale host-owned bytes are overwritten; guest-owned bytes are kept. */
	memset(page->raw, 0x5a, sizeof(page->raw));
	pvsched_shm_bridge_publish_response(&shm, PVSCHED_STATUS_MODE_MISMATCH);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_MODE_MISMATCH);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version),
			(u32)PVSCHED_ABI_VERSION);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.accepted_mode),
			(u32)PVSCHED_MODE_FRAMEWORK);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.reserved), 0U);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(&page->host_area, 0,
					    sizeof(page->host_area)));
	KUNIT_EXPECT_TRUE(test, !memchr_inv(page->extension, 0,
					    sizeof(page->extension)));
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.abi_version), 0x5a5a5a5aU);
	KUNIT_EXPECT_TRUE(test, !memchr_inv(page->guest_area.custom, 0x5a,
					    sizeof(page->guest_area.custom)));
}

static void pvsched_shm_bridge_status_test(struct kunit *test)
{
	struct pvsched_shm shm;
	union pvsched_vcpu_page *page = pvsched_shm_bridge_test_page(test, &shm);

	page->header.host_abi_version = cpu_to_le32(PVSCHED_ABI_VERSION);
	pvsched_shm_bridge_publish_status(&shm, PVSCHED_STATUS_ENABLED);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_ENABLED);
	pvsched_shm_bridge_publish_status(&shm, PVSCHED_STATUS_DISABLED);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.status),
			(u32)PVSCHED_STATUS_DISABLED);
	/* A status change touches only the status word. */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(page->header.host_abi_version),
			(u32)PVSCHED_ABI_VERSION);
}

static struct kunit_case pvsched_shm_bridge_cases[] = {
	KUNIT_CASE(pvsched_shm_bridge_snapshot_test),
	KUNIT_CASE(pvsched_shm_bridge_publish_test),
	KUNIT_CASE(pvsched_shm_bridge_negotiation_snapshot_test),
	KUNIT_CASE(pvsched_shm_bridge_response_test),
	KUNIT_CASE(pvsched_shm_bridge_status_test),
	{}
};

static struct kunit_suite pvsched_shm_bridge_suite = {
	.name = "pvsched-shm-bridge",
	.test_cases = pvsched_shm_bridge_cases,
};

kunit_test_suite(pvsched_shm_bridge_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
