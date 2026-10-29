/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_EXTERNAL_H
#define _VIRT_PVSCHED_EXTERNAL_H

#include <linux/types.h>

#include "attachment.h"

struct pvsched_policy_entry;
struct pvsched_runner_runtime;
struct pvsched_vcpu_runner;

/*
 * The external policy side of a runner runtime.  The runtime is only its
 * container: an external attachment runs none of the managed engine, and
 * these calls are all pvsched does for it besides the attachment's own
 * lifetime and hook checks.
 */

/* Runtime prepare for an external @entry: take a reference on it. */
void pvsched_external_prepare(struct pvsched_runner_runtime *runtime,
			      struct pvsched_policy_entry *entry);
/*
 * Fill in the vCPU context and call the attach op, once negotiation is
 * enabled and before publication.  Session mutex held; may sleep.
 */
int pvsched_external_attach(struct pvsched_runner_runtime *runtime,
			    struct pvsched_vcpu_runner *runner);
/*
 * Call the detach op if attach succeeded and detach is still owed: at
 * teardown, once the attachment is disabled and drained, or when
 * publication fails.  Process context; takes the state lock.
 */
void pvsched_external_detach(struct pvsched_runner_runtime *runtime);
/* A target-local event under state_lock. */
void pvsched_external_local_event_locked(struct pvsched_runner_runtime *runtime,
					 enum pvsched_attachment_visit_kind kind,
					 u32 mode_flags);

#endif /* _VIRT_PVSCHED_EXTERNAL_H */
