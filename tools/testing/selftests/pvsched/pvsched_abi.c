// SPDX-License-Identifier: GPL-2.0-only

#include <linux/pvsched.h>
#include <linux/sched.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kselftest.h"

/* linux/sched.h does not expose SCHED_OTHER to all libc feature profiles. */
#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif

_Static_assert(sizeof(struct pvsched_header) == 64, "header size");
_Static_assert(offsetof(struct pvsched_header, abi_version) == 0,
		       "abi version offset");
_Static_assert(offsetof(struct pvsched_header, policy_version) == 4,
		       "policy version offset");
_Static_assert(offsetof(struct pvsched_header, policy_name) == 8,
		       "policy name offset");

_Static_assert(offsetof(struct pvsched_header, protocol_id) == 40,
		       "protocol offset");
_Static_assert(offsetof(struct pvsched_header, requested_mode) == 44,
		       "requested mode offset");
_Static_assert(offsetof(struct pvsched_header, status) == 48,
		       "status offset");

_Static_assert(offsetof(struct pvsched_header, host_abi_version) == 52,
		       "host abi offset");
_Static_assert(offsetof(struct pvsched_header, accepted_mode) == 56,
		       "accepted mode offset");
_Static_assert(offsetof(struct pvsched_header, reserved) == 60,
		       "header reserved offset");
_Static_assert(sizeof(struct pvsched_prio_desc) == 3,
		       "task descriptor size");
_Static_assert(offsetof(struct pvsched_prio_desc, sched_policy) == 0,
		       "task policy offset");
_Static_assert(offsetof(struct pvsched_prio_desc, nice) == 1,
		       "task nice offset");
_Static_assert(offsetof(struct pvsched_prio_desc, rt_prio) == 2,
		       "task rt priority offset");
_Static_assert(sizeof(union pvsched_task_intent) == 8,
		       "task intent size");
_Static_assert(_Alignof(union pvsched_task_intent) >= 8,
		       "task intent alignment");
_Static_assert(offsetof(union pvsched_task_intent, current_task) == 0,
		       "current task offset");
_Static_assert(offsetof(union pvsched_task_intent, pending_task) == 3,
		       "pending task offset");
_Static_assert(offsetof(union pvsched_task_intent, flags) == 6,
		       "intent flags offset");
_Static_assert(offsetof(union pvsched_task_intent, reserved) == 7,
		       "intent reserved offset");
_Static_assert(offsetof(union pvsched_task_intent, raw) == 0,
		       "intent raw offset");
_Static_assert(sizeof(union pvsched_applied_state) == 8,
		       "applied state size");
_Static_assert(_Alignof(union pvsched_applied_state) >= 8,
		       "applied state alignment");
_Static_assert(offsetof(union pvsched_applied_state, reserved0) == 0,
		       "applied first reserved offset");
_Static_assert(offsetof(union pvsched_applied_state, hints) == 3,
		       "applied hints offset");
_Static_assert(offsetof(union pvsched_applied_state, flags) == 4,
		       "applied flags offset");
_Static_assert(offsetof(union pvsched_applied_state, boost) == 5,
		       "applied boost offset");
_Static_assert(offsetof(union pvsched_applied_state, reserved1) == 6,
		       "applied second reserved offset");
_Static_assert(offsetof(union pvsched_applied_state, raw) == 0,
		       "applied raw offset");
_Static_assert(sizeof(struct pvsched_default_guest_area) == 64,
		       "guest area size");
_Static_assert(offsetof(struct pvsched_default_guest_area, task_intent) == 0,
		       "intent offset");
_Static_assert(offsetof(struct pvsched_default_guest_area, cs_state) == 8,
		       "cs offset");
_Static_assert(offsetof(struct pvsched_default_guest_area, interrupt_ack) == 16,
		       "interrupt ack offset");
_Static_assert(offsetof(struct pvsched_default_guest_area, reserved) == 24,
		       "guest reserved offset");
_Static_assert(sizeof(struct pvsched_host_area) == 64, "host area size");
_Static_assert(sizeof(struct pvsched_policy_info) == 56, "policy info size");
_Static_assert(offsetof(struct pvsched_policy_info, name) == 8,
		       "policy info name offset");
_Static_assert(offsetof(struct pvsched_policy_info, version) == 40,
		       "policy info version offset");
_Static_assert(offsetof(struct pvsched_policy_info, reserved) == 52,
		       "policy info reserved offset");
_Static_assert(sizeof(struct pvsched_set_policy) == 40, "set policy size");
_Static_assert(offsetof(struct pvsched_set_policy, version) == 32,
		       "set policy version offset");

_Static_assert(offsetof(struct pvsched_host_area, applied_state) == 0,
		       "applied offset");
_Static_assert(offsetof(struct pvsched_host_area, custom) == 8,
		       "host custom offset");
_Static_assert(offsetof(struct pvsched_host_area, default_area.interrupt_ticket) == 8,
		       "interrupt ticket offset");
_Static_assert(offsetof(struct pvsched_host_area, default_area.reserved) == 16,
		       "host reserved offset");
_Static_assert(sizeof(union pvsched_vcpu_page) == PVSCHED_VCPU_STRIDE,
		       "page size");
_Static_assert(offsetof(union pvsched_vcpu_page, header) == 0,
		       "common header page offset");
_Static_assert(offsetof(union pvsched_vcpu_page, guest_area) == 64,
		       "guest area page offset");
_Static_assert(offsetof(union pvsched_vcpu_page, host_area) == 128,
		       "host area page offset");
_Static_assert(offsetof(union pvsched_vcpu_page, extension) == 192,
		       "extension page offset");
_Static_assert(_Alignof(union pvsched_vcpu_page) >= 8,
		       "page alignment");
_Static_assert(sizeof(struct pvsched_info) == 32, "info size");
_Static_assert(offsetof(struct pvsched_info, max_shm_pages_per_session) == 20,
		       "per-session SHM limit offset");
_Static_assert(offsetof(struct pvsched_info, max_shm_pages_global) == 24,
		       "global SHM limit offset");
_Static_assert(offsetof(struct pvsched_info, reserved) == 28, "info reserved offset");
_Static_assert(sizeof(struct pvsched_query_runner) == 32, "query size");
_Static_assert(offsetof(struct pvsched_query_runner, runner_id) == 0,
		       "query runner offset");
_Static_assert(offsetof(struct pvsched_query_runner, flags) == 8,
		       "query flags offset");
_Static_assert(offsetof(struct pvsched_query_runner, state) == 12,
		       "query state offset");
_Static_assert(offsetof(struct pvsched_query_runner, last_fault_errno) == 16,
		       "query fault offset");
_Static_assert(offsetof(struct pvsched_query_runner, reserved0) == 20,
		       "query reserved0 offset");
_Static_assert(offsetof(struct pvsched_query_runner, reserved1) == 24,
		       "query reserved1 offset");
_Static_assert(sizeof(struct pvsched_attach_shm) == 32, "attach size");
_Static_assert(offsetof(struct pvsched_attach_shm, runner_id) == 0,
		       "attach runner offset");
_Static_assert(offsetof(struct pvsched_attach_shm, user_addr) == 8,
		       "attach address offset");
_Static_assert(offsetof(struct pvsched_attach_shm, size) == 16,
		       "attach size offset");
_Static_assert(offsetof(struct pvsched_attach_shm, flags) == 24,
		       "attach flags offset");
_Static_assert(offsetof(struct pvsched_attach_shm, negotiation_status) == 28,
		       "attach status offset");
_Static_assert(sizeof(struct pvsched_detach_shm) == 32, "detach size");
_Static_assert(offsetof(struct pvsched_detach_shm, runner_id) == 0,
		       "detach runner offset");
_Static_assert(offsetof(struct pvsched_detach_shm, reserved) == 8,
		       "detach reserved offset");
_Static_assert(PVSCHED_VCPU_STRIDE == 4096 && PVSCHED_NAME_MAX == 32 &&
	       PVSCHED_COMMON_HEADER_SIZE == 64 &&
	       PVSCHED_GUEST_AREA_SIZE == 64 && PVSCHED_HOST_AREA_SIZE == 64 &&
	       PVSCHED_FIXED_SIZE == 192 && PVSCHED_EXTENSION_SIZE == 3904,
	       "ABI size constants");

static unsigned int failures;

static void check(int condition, const char *name)
{
	if (condition)
		ksft_test_result_pass("%s\n", name);
	else {
		ksft_test_result_fail("%s\n", name);
		failures++;
	}
}

/* Test-local construction: the UAPI intentionally has no accessors. */
static void le32(unsigned char out[4], uint32_t value)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		out[i] = (unsigned char)(value >> (i * 8));
}

static void le64(unsigned char out[8], uint64_t value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		out[i] = (unsigned char)(value >> (i * 8));
}

static int canonical(unsigned int policy, int nice, unsigned int rt)
{
	if (policy == SCHED_OTHER || policy == SCHED_BATCH)
		return nice >= -20 && nice <= 19 && rt == 0;
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		return nice == 0 && rt >= 1 && rt <= 99;
	if (policy == SCHED_IDLE || policy == SCHED_DEADLINE)
		return nice == 0 && rt == 0;
	return 0;
}

int main(void)
{
	unsigned char bytes[8];
	unsigned char expected[8] = { 1, 0, 0x12, 0, 0xec, 0, 1, 0 };
	unsigned char applied_expected[8] = { 0x00, 0x00, 0x00, 0x01,
		0x03, 0x02, 0x00, 0x00 };
	unsigned char cs_expected[8] = { 0x05, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00 };
	unsigned char zero_expected[8] = { 0 };
	unsigned char ack_expected[8] = { 0xef, 0xcd, 0xab, 0x89,
		0x67, 0x45, 0x23, 0x01 };
	unsigned char ticket_expected[8] = { 0x11, 0x22, 0x33, 0x44,
		0x55, 0x66, 0x77, 0x88 };
	unsigned char ticket_bytes[8];
	unsigned char header_expected[64] = { 0 };
	struct pvsched_prio_desc current = { SCHED_FIFO, 0, 0x12 };
	struct pvsched_prio_desc pending = { SCHED_OTHER, -20, 0 };
	struct pvsched_prio_desc positive = { SCHED_OTHER, 19, 0 };
	union pvsched_task_intent intent = { 0 };
	union pvsched_applied_state applied = { 0 };
	struct pvsched_header header = { 0 };
	struct pvsched_default_guest_area guest = { 0 };
	struct pvsched_host_area host = { 0 };
	union pvsched_vcpu_page page = { 0 };

	memcpy(&intent.current_task, &current, sizeof(current));
	memcpy(&intent.pending_task, &pending, sizeof(pending));
	intent.flags = PVSCHED_INTENT_FLAG_PENDING_VALID;
	applied.hints = PVSCHED_HINT_KICK_DEBOOST;
	applied.flags = PVSCHED_APPLIED_CS_THROTTLED |
			PVSCHED_APPLIED_TOTAL_THROTTLED;
	applied.boost = PVSCHED_BOOST_CS;

	ksft_print_header();
	ksft_set_plan(34);

	check(sizeof(__le64) == sizeof(uint64_t) &&
	      _Alignof(struct pvsched_default_guest_area) >= 8,
	      "aligned le64 width and alignment");
	check(offsetof(union pvsched_vcpu_page, guest_area.default_area.interrupt_ack) == 80 &&
	      offsetof(union pvsched_vcpu_page, host_area.default_area.interrupt_ticket) == 136 &&
	      sizeof(host.default_area) == sizeof(host.custom),
	      "default ack and ticket overlay layout");
	check(_IOC_DIR(PVSCHED_GET_INFO) == _IOC_READ &&
	      _IOC_SIZE(PVSCHED_GET_INFO) == sizeof(struct pvsched_info) &&
	      _IOC_DIR(PVSCHED_QUERY_RUNNER) == (_IOC_READ | _IOC_WRITE) &&
	      _IOC_SIZE(PVSCHED_QUERY_RUNNER) == sizeof(struct pvsched_query_runner) &&
	      _IOC_DIR(PVSCHED_ATTACH_SHM) == (_IOC_READ | _IOC_WRITE) &&
	      _IOC_SIZE(PVSCHED_ATTACH_SHM) == sizeof(struct pvsched_attach_shm) &&
	      _IOC_DIR(PVSCHED_DETACH_SHM) == _IOC_WRITE &&
	      _IOC_SIZE(PVSCHED_DETACH_SHM) == sizeof(struct pvsched_detach_shm) &&
	      _IOC_DIR(PVSCHED_QUERY_POLICY) == (_IOC_READ | _IOC_WRITE) &&
	      _IOC_NR(PVSCHED_QUERY_POLICY) == 5 &&
	      _IOC_SIZE(PVSCHED_QUERY_POLICY) == sizeof(struct pvsched_policy_info) &&
	      _IOC_DIR(PVSCHED_SET_POLICY) == _IOC_WRITE &&
	      _IOC_NR(PVSCHED_SET_POLICY) == 6 &&
	      _IOC_SIZE(PVSCHED_SET_POLICY) == sizeof(struct pvsched_set_policy),
	      "control ioctl directions and sizes");
	check(sizeof(intent) == 8 && intent.reserved == 0,
	      "intent word and reserved bits");
	check(PVSCHED_INTENT_FLAG_PENDING_VALID == 1,
	      "intent pending flag value");
	{
		union pvsched_task_intent idle = {
			.flags = PVSCHED_INTENT_FLAG_IDLE,
		};
		const unsigned char *wire = (const unsigned char *)&idle.raw;

		check(PVSCHED_INTENT_FLAG_IDLE == 2 &&
		      !(PVSCHED_INTENT_FLAG_IDLE &
			PVSCHED_INTENT_FLAG_PENDING_VALID) &&
		      wire[6] == 2 && wire[7] == 0,
		      "intent idle flag value and wire byte");
	}
	check(PVSCHED_CS_NMI == 1 && PVSCHED_CS_HARDIRQ == 2 &&
	      PVSCHED_CS_SOFTIRQ == 4 && PVSCHED_CS_PREEMPT_DISABLED == 8 &&
	      PVSCHED_CS_VALID_MASK == 0xfULL &&
	      (PVSCHED_CS_RESERVED_MASK & PVSCHED_CS_VALID_MASK) == 0,
	      "critical section masks");

	check(!memcmp(&intent.raw, expected, sizeof(expected)),
	      "intent struct wire bytes");
	check(!memcmp(&applied.raw, applied_expected, sizeof(applied_expected)),
	      "applied-state golden bytes");
	le64(bytes, PVSCHED_CS_NMI | PVSCHED_CS_SOFTIRQ);
	check(!memcmp(bytes, cs_expected, sizeof(cs_expected)),
	      "CS golden bytes");
	check(PVSCHED_HINT_KICK_DEBOOST == 1 &&
	      PVSCHED_APPLIED_CS_THROTTLED == 1 &&
	      PVSCHED_APPLIED_TOTAL_THROTTLED == 2 &&
	      PVSCHED_BOOST_BASELINE == 0 && PVSCHED_BOOST_TASK == 1 &&
	      PVSCHED_BOOST_CS == 2,
	      "applied hint, flag and boost values");
	check(((uint8_t)(int8_t)-20) == 0xec && ((uint8_t)(int8_t)19) == 0x13,
	      "signed nice byte vectors");
	check((unsigned char)pending.nice == 0xec &&
	      (unsigned char)positive.nice == 0x13,
	      "signed nice stays in descriptor field");

	check(canonical(SCHED_OTHER, -20, 0) && canonical(SCHED_BATCH, 19, 0),
	      "normal canonical tuples");
	check(canonical(SCHED_FIFO, 0, 1) && canonical(SCHED_RR, 0, 99),
	      "rt canonical tuples");
	check(canonical(SCHED_IDLE, 0, 0) && canonical(SCHED_DEADLINE, 0, 0),
	      "idle deadline canonical tuples");
	check(!canonical(SCHED_FIFO, 1, 1) && !canonical(SCHED_OTHER, 0, 1),
	      "noncanonical tuple vectors identified");

	check((intent.flags & PVSCHED_INTENT_FLAG_PENDING_VALID) &&
	      intent.pending_task.sched_policy == SCHED_OTHER,
	      "pending-valid fixture");
	check(intent.reserved == 0 && intent.flags == 1,
	      "pending flags and reserved bytes");
	check(PVSCHED_ABI_VERSION == 1 &&
	      !strcmp(PVSCHED_DEFAULT_POLICY_NAME, "default") &&
	      PVSCHED_DEFAULT_POLICY_VERSION == 1,
	      "ABI and default policy versions");
	check(PVSCHED_PROTOCOL_DEFAULT == 0 && PVSCHED_PROTOCOL_CUSTOM == 1 &&
	      PVSCHED_MODE_FRAMEWORK == 0 && PVSCHED_MODE_POLICY == 1 &&
	      PVSCHED_STATUS_DISABLED == 0 && PVSCHED_STATUS_ENABLED == 1 &&
	      PVSCHED_STATUS_UNKNOWN_POLICY == 2 &&
	      PVSCHED_STATUS_ABI_MISMATCH == 3 &&
	      PVSCHED_STATUS_POLICY_VERSION_MISMATCH == 4 &&
	      PVSCHED_STATUS_PROTOCOL_MISMATCH == 5 &&
	      PVSCHED_STATUS_MODE_MISMATCH == 6,
	      "protocol mode and status constants");

	strcpy(header.policy_name, PVSCHED_DEFAULT_POLICY_NAME);
	le32(header_expected + 0, PVSCHED_ABI_VERSION);
	le32(header_expected + 4, PVSCHED_DEFAULT_POLICY_VERSION);
	memcpy(header_expected + 8, PVSCHED_DEFAULT_POLICY_NAME,
	       sizeof(PVSCHED_DEFAULT_POLICY_NAME));
	le32(header_expected + 40, PVSCHED_PROTOCOL_CUSTOM);
	le32(header_expected + 44, PVSCHED_MODE_POLICY);
	le32(header_expected + 48, PVSCHED_STATUS_ABI_MISMATCH);
	le32(header_expected + 52, 0x01020304);
	le32(header_expected + 56, PVSCHED_MODE_POLICY);
	memcpy(&header.abi_version, header_expected + 0, 4);
	memcpy(&header.policy_version, header_expected + 4, 4);
	memcpy(&header.protocol_id, header_expected + 40, 4);
	memcpy(&header.requested_mode, header_expected + 44, 4);
	memcpy(&header.status, header_expected + 48, 4);
	memcpy(&header.host_abi_version, header_expected + 52, 4);
	memcpy(&header.accepted_mode, header_expected + 56, 4);
	check(!memcmp(&header, header_expected, sizeof(header)),
	      "common header wire bytes");
	memcpy(&guest.task_intent, &intent, sizeof(intent));
	memcpy(&host.applied_state, &applied, sizeof(applied));
	check(!memcmp(&guest.interrupt_ack, zero_expected, sizeof(zero_expected)) &&
	      !memcmp(&host.default_area.interrupt_ticket, zero_expected,
		      sizeof(zero_expected)),
	      "zero interrupt ack and ticket vectors");
	le64(bytes, UINT64_C(0x0123456789abcdef));
	le64(ticket_bytes, UINT64_C(0x8877665544332211));
	memcpy(page.raw + offsetof(union pvsched_vcpu_page,
				  guest_area.default_area.interrupt_ack), bytes,
	       sizeof(bytes));
	memcpy(page.raw + offsetof(union pvsched_vcpu_page,
				  host_area.default_area.interrupt_ticket), ticket_bytes,
	       sizeof(ticket_bytes));
	check(!memcmp(bytes, ack_expected, sizeof(ack_expected)) &&
	      !memcmp(ticket_bytes, ticket_expected, sizeof(ticket_expected)) &&
	      !memcmp(&page.guest_area.default_area.interrupt_ack,
		      ack_expected, sizeof(ack_expected)) &&
	      !memcmp(&page.host_area.default_area.interrupt_ticket,
		      ticket_expected, sizeof(ticket_expected)) &&
	      !memcmp(page.raw + 80, ack_expected, sizeof(ack_expected)) &&
	      !memcmp(page.raw + 136, ticket_expected, sizeof(ticket_expected)),
	      "little-endian interrupt ack and ticket vectors");
	check(header.policy_name[7] == '\0' &&
	      header.policy_name[PVSCHED_NAME_MAX - 1] == '\0',
	      "default policy name zero padding");
	check(guest.reserved[0] == 0 && host.default_area.reserved[0] == 0,
	      "default reserved areas stay zero initialized");
	check(!memcmp(&guest.task_intent.raw, expected, sizeof(expected)) &&
	      !memcmp(&host.applied_state.raw, applied_expected,
		       sizeof(applied_expected)),
	      "struct fields match wire vectors");
	check(!memcmp(&intent.raw, expected, sizeof(expected)) &&
	      !memcmp(&guest.task_intent, expected, sizeof(expected)),
	      "intent raw and guest byte views match");
	{
		union pvsched_task_intent raw_view = { 0 };

		raw_view.raw = intent.raw;
		check(raw_view.current_task.sched_policy == SCHED_FIFO &&
		      raw_view.current_task.nice == 0 &&
		      raw_view.current_task.rt_prio == 0x12 &&
		      raw_view.pending_task.nice == -20 &&
		      raw_view.flags == PVSCHED_INTENT_FLAG_PENDING_VALID,
		      "raw to local intent byte view");
	}
	{
		union pvsched_applied_state raw_view = { 0 };

		raw_view.raw = applied.raw;
		check(raw_view.boost == PVSCHED_BOOST_CS &&
		      raw_view.hints == 1 && raw_view.flags == 3,
		      "raw to local applied byte view");
	}
	{
		union pvsched_applied_state malformed = { 0 };
		const unsigned char malformed_bytes[8] = {
			1, 2, 3, 0x80, 0x80, 7, 4, 5
		};

		memcpy(&malformed.raw, malformed_bytes, sizeof(malformed_bytes));
		check((malformed.hints & ~PVSCHED_HINT_KICK_DEBOOST) == 0x80 &&
		      (malformed.flags & ~(PVSCHED_APPLIED_CS_THROTTLED |
					    PVSCHED_APPLIED_TOTAL_THROTTLED)) == 0x80 &&
		      malformed.boost == 7 &&
		      malformed.reserved0[0] == 1 && malformed.reserved0[1] == 2 &&
		      malformed.reserved0[2] == 3 && malformed.reserved1[0] == 4 &&
		      malformed.reserved1[1] == 5,
		      "unknown applied flags, boost and reserved fixture");
	}
	{
		unsigned char applied_before[8];

		memcpy(applied_before, &host.applied_state, sizeof(applied_before));
		memset(host.custom, 0xa5, sizeof(host.custom));
		check(host.custom[0] == 0xa5 && host.custom[55] == 0xa5,
		      "custom protocol host view covers ticket overlay");
		check(!memcmp(applied_before, &host.applied_state,
			      sizeof(applied_before)),
		      "host custom independent of applied state");
	}
	check(sizeof(((union pvsched_vcpu_page *)0)->raw) == 4096 &&
	      PVSCHED_FIXED_SIZE + PVSCHED_EXTENSION_SIZE == PVSCHED_VCPU_STRIDE &&
	      offsetof(union pvsched_vcpu_page, extension) == PVSCHED_FIXED_SIZE,
	      "reserved extent reaches page end");

	ksft_print_cnts();
	return failures ? KSFT_FAIL : KSFT_PASS;
}
