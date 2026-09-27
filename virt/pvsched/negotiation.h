/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_NEGOTIATION_H
#define _VIRT_PVSCHED_NEGOTIATION_H

#include <uapi/linux/pvsched.h>

#include "shm_bridge.h"

/*
 * Decide the negotiation for the session's policy @ops from one private
 * snapshot: the guest must request exactly that policy's name and version,
 * its protocol and FRAMEWORK mode.  Returns PVSCHED_STATUS_ENABLED when the
 * request is acceptable, otherwise the non-enabled status the host
 * publishes with its rejection.
 */
enum pvsched_status
pvsched_negotiate(const struct pvsched_negotiation_request *request,
		  const struct pvsched_default_guest_area *guest,
		  const struct pvsched_policy_ops *ops);

#endif /* _VIRT_PVSCHED_NEGOTIATION_H */
