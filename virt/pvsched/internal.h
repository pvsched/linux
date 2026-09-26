/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_INTERNAL_H
#define _VIRT_PVSCHED_INTERNAL_H

#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/workqueue.h>

#include "runner_runtime.h"
#include "lifecycle.h"

/*
 * Resource limits, reported to userspace by GET_INFO.  A session holds at
 * most one shared page per runner; the global page count is kept for
 * attachments that may one day span more than one page.
 */
#define PVSCHED_MAX_RUNNERS_PER_SESSION		1024
#define PVSCHED_MAX_RUNNERS_GLOBAL		16384
#define PVSCHED_MAX_SESSIONS_GLOBAL		256
#define PVSCHED_MAX_SHM_PAGES_PER_SESSION	1024
#define PVSCHED_MAX_SHM_PAGES_GLOBAL		16384

struct pvsched_session;

struct pvsched_vcpu_runner {
	struct list_head session_node;
	struct hlist_node global_node;
	struct pid *pid;
	u64 runner_id;
	struct pvsched_session *session;
	struct pvsched_runner_runtime *runtime;
	struct sched_task_state baseline;
	struct pvsched_ticket_owner ticket_owner;
	struct work_struct cleanup_work;
	int last_fault;
	bool baseline_valid;
	bool last_fault_valid;
	bool exited;
#if IS_ENABLED(CONFIG_KUNIT)
	/*
	 * Static stubs redirect only the test task, so use this hook to
	 * synchronize actual cleanup-worker and final-close contexts.
	 */
	void (*lifecycle_test_hook)(struct pvsched_vcpu_runner *runner,
				    enum pvsched_lifecycle_test_point point,
				    void *data);
	void *lifecycle_test_data;
#endif
};

struct pvsched_session {
	/* Serializes control operations, runner ownership and cleanup work. */
	struct mutex lock;
	struct list_head runners;
	struct mm_struct *owner_mm;
	struct pid *owner_tgid;
	u64 next_runner_id;
	u32 nr_runners;
	bool closing;
};

#endif /* _VIRT_PVSCHED_INTERNAL_H */
