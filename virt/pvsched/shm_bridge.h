/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_SHM_BRIDGE_H
#define _VIRT_PVSCHED_SHM_BRIDGE_H

#include <linux/pvsched_policy.h>
#include <uapi/linux/pvsched.h>

#include "shm.h"

/*
 * The bridge is the only code that reads or writes the pinned shared page.
 * Every shared word is accessed whole and aligned; guest-owned words are
 * copied into caller storage and never interpreted in place.  None of these
 * helpers validates guest input.
 *
 * Callers access the page either from the owning ATTACH transaction before
 * publication, while the session mutex excludes every other host user, or
 * from an attachment state_lock visit after publication.  No access happens
 * after the attachment is unhashed and its readers drained.
 */

void pvsched_shm_bridge_snapshot(const struct pvsched_shm *shm,
				 struct pvsched_default_guest_area *guest);
void pvsched_shm_bridge_publish_vmentry(const struct pvsched_shm *shm,
					 const struct pvsched_host_area *host);
void pvsched_shm_bridge_read_negotiation(const struct pvsched_shm *shm,
					  struct pvsched_negotiation_request *request,
					  struct pvsched_default_guest_area *guest);
void pvsched_shm_bridge_publish_response(const struct pvsched_shm *shm,
					  enum pvsched_status status);
void pvsched_shm_bridge_publish_status(const struct pvsched_shm *shm,
				       enum pvsched_status status);

#endif /* _VIRT_PVSCHED_SHM_BRIDGE_H */
