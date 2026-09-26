/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_LIFECYCLE_H
#define _VIRT_PVSCHED_LIFECYCLE_H

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

#endif /* _VIRT_PVSCHED_LIFECYCLE_H */
