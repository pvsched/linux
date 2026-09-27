/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_LIFECYCLE_H
#define _VIRT_PVSCHED_LIFECYCLE_H

#include <linux/types.h>
#include <uapi/linux/pvsched.h>

#include "default_policy.h"

struct mm_struct;
struct task_struct;
struct pvsched_attachment;
struct pvsched_session;
struct pvsched_vcpu_runner;
struct pvsched_runner_runtime;

#if IS_ENABLED(CONFIG_KUNIT)
enum pvsched_lifecycle_test_point {
	PVSCHED_CLOSE_AFTER_DISABLE,
	PVSCHED_CLOSE_AFTER_FINISH,
	PVSCHED_CLOSE_FINAL_DISABLE,
	PVSCHED_CLEANUP_WORK_ENTER,
};
#endif

void pvsched_runner_lifecycle_init(struct pvsched_vcpu_runner *runner,
				   struct pvsched_session *session);
int pvsched_runner_publish_runtime_locked(struct pvsched_vcpu_runner *runner,
					  struct pvsched_runner_runtime *runtime,
					  pvsched_attachment_commit_fn commit,
					  void *data);
void pvsched_runner_request_cleanup_locked(struct pvsched_attachment *attachment);
bool pvsched_runner_cleanup_once_locked(struct pvsched_vcpu_runner *runner);
void pvsched_session_release_runtimes(struct pvsched_session *session);
void pvsched_runner_query_locked(struct pvsched_vcpu_runner *runner,
				 u32 *state, u32 *flags, int *last_fault);
void pvsched_runner_detach_runtime_locked(struct pvsched_vcpu_runner *runner);

/* Fixed inputs of one ATTACH, already validated by the ioctl layer. */
struct pvsched_attach_params {
	unsigned long user_addr;
	const struct pvsched_default_policy_config *config;
	u64 cs_budget_ns;
	u64 generic_budget_ns;
	bool deboost_notify;
	u64 idle_hold_ns;
	bool cap_ipc_lock;
	unsigned long memlock_limit_pages;
};

int pvsched_runner_prepare_attach_locked(struct pvsched_vcpu_runner *runner,
					 struct task_struct *task,
					 const struct pvsched_attach_params *params,
					 struct pvsched_runner_runtime **prepared,
					 enum pvsched_status *status);
int pvsched_runner_publish_prepared_locked(struct pvsched_vcpu_runner *runner,
					   struct pvsched_runner_runtime *runtime);
void pvsched_runner_reject_prepared_locked(struct pvsched_runner_runtime *runtime);
void pvsched_runner_discard_prepared_locked(struct pvsched_runner_runtime *runtime);

#endif /* _VIRT_PVSCHED_LIFECYCLE_H */
