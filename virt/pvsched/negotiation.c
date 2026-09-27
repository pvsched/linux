// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/string.h>
#include <kunit/visibility.h>

#include "negotiation.h"

static bool pvsched_zero_bytes(const void *bytes, size_t size)
{
	return !memchr_inv(bytes, 0, size);
}

/* Nonempty, NUL-terminated, and zero-padded after the terminator. */
static bool pvsched_policy_name_valid(const char *name)
{
	size_t len = strnlen(name, PVSCHED_NAME_MAX);

	if (!len || len == PVSCHED_NAME_MAX)
		return false;
	return pvsched_zero_bytes(name + len, PVSCHED_NAME_MAX - len);
}

/*
 * Guest-owned bits that must be zero at attach.  Task tuples themselves are
 * dynamic and revalidated at every selection, so they are not judged here.
 */
static bool
pvsched_default_guest_area_valid(const struct pvsched_default_guest_area *guest)
{
	union pvsched_task_intent intent = guest->task_intent;

	if (guest->interrupt_ack)
		return false;
	if (le64_to_cpu(guest->cs_state) & PVSCHED_CS_RESERVED_MASK)
		return false;
	if (intent.reserved ||
	    (intent.flags & ~PVSCHED_INTENT_FLAGS_VALID))
		return false;
	return pvsched_zero_bytes(guest->reserved, sizeof(guest->reserved));
}

enum pvsched_status
pvsched_negotiate_default(const struct pvsched_negotiation_request *request,
			  const struct pvsched_default_guest_area *guest)
{
	/* The rejection order is part of the ABI; keep it deterministic. */
	if (request->abi_version != PVSCHED_ABI_VERSION)
		return PVSCHED_STATUS_ABI_MISMATCH;
	if (!pvsched_policy_name_valid(request->policy_name))
		return PVSCHED_STATUS_DISABLED;
	if (strcmp(request->policy_name, PVSCHED_DEFAULT_POLICY_NAME))
		return PVSCHED_STATUS_UNKNOWN_POLICY;
	if (request->policy_version != PVSCHED_DEFAULT_POLICY_VERSION)
		return PVSCHED_STATUS_POLICY_VERSION_MISMATCH;
	if (request->protocol_id != PVSCHED_PROTOCOL_DEFAULT)
		return PVSCHED_STATUS_PROTOCOL_MISMATCH;
	if (request->requested_mode != PVSCHED_MODE_FRAMEWORK)
		return PVSCHED_STATUS_MODE_MISMATCH;
	if (!pvsched_default_guest_area_valid(guest))
		return PVSCHED_STATUS_DISABLED;
	return PVSCHED_STATUS_ENABLED;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_negotiate_default);
