// SPDX-License-Identifier: GPL-2.0-only

#include <linux/export.h>
#include <linux/string.h>
#include <kunit/visibility.h>

#include "negotiation.h"
#include "policy.h"

static bool pvsched_zero_bytes(const void *bytes, size_t size)
{
	return !memchr_inv(bytes, 0, size);
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
pvsched_negotiate(const struct pvsched_negotiation_request *request,
		  const struct pvsched_default_guest_area *guest,
		  const struct pvsched_policy_ops *ops)
{
	/* The rejection order is part of the ABI; keep it deterministic. */
	if (request->abi_version != PVSCHED_ABI_VERSION)
		return PVSCHED_STATUS_ABI_MISMATCH;
	if (!pvsched_policy_name_valid(request->policy_name))
		return PVSCHED_STATUS_DISABLED;
	/* A guest cannot switch its session to another policy. */
	if (strcmp(request->policy_name, ops->name))
		return PVSCHED_STATUS_UNKNOWN_POLICY;
	if (request->policy_version != ops->version)
		return PVSCHED_STATUS_POLICY_VERSION_MISMATCH;
	if (request->protocol_id != ops->protocol)
		return PVSCHED_STATUS_PROTOCOL_MISMATCH;
	if (request->requested_mode != PVSCHED_MODE_FRAMEWORK)
		return PVSCHED_STATUS_MODE_MISMATCH;
	if (!pvsched_default_guest_area_valid(guest))
		return PVSCHED_STATUS_DISABLED;
	/* The policy's own checks come last. */
	if (ops->negotiate && ops->negotiate(request))
		return PVSCHED_STATUS_DISABLED;
	return PVSCHED_STATUS_ENABLED;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_negotiate);
