/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_ATTACHMENT_H
#define _VIRT_PVSCHED_ATTACHMENT_H

#include <linux/hashtable.h>
#include <linux/bits.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

/*
 * The attachment is common framework state for every policy ownership mode:
 * pinned identity, lookup hashes and binding, admission, position/mode facts,
 * terminal latches, and teardown coordination. Framework-managed enforcement
 * state (baseline/applied tuples, accounting, timer, reasons, faults, and an
 * owed restore) belongs in struct pvsched_runner_runtime. A future fully
 * policy-managed binding may embed this attachment beside its own enforcement
 * state without using that runtime engine.
 */

enum pvsched_runner_position {
	PVSCHED_RUNNER_HOST,
	PVSCHED_RUNNER_BLOCKED,
	PVSCHED_RUNNER_GUEST,
	PVSCHED_RUNNER_QEMU,
};

/* Architecture adapters translate their factual target-mode bits here. */
enum pvsched_runner_mode_flag {
	PVSCHED_RUNNER_MODE_NESTED	= BIT(0),
	PVSCHED_RUNNER_MODE_UNSUPPORTED	= BIT(1),
	PVSCHED_RUNNER_MODE_NO_TICKET	= BIT(2),
	/* A VMEXIT fact only: the exit is a non-nested HLT. */
	PVSCHED_RUNNER_MODE_HLT_EXIT	= BIT(3),
};

struct pvsched_attachment;
struct pvsched_vcpu_runner;

/* Immutable remote-lookup identity, installed on the first local hook. */
struct pvsched_vcpu_key {
	/* RCU hash link; storage remains valid through the reader drain. */
	struct hlist_node node;
	/* Enclosing attachment reached by a remote visitor. */
	struct pvsched_attachment *attachment;
	/* Opaque KVM vCPU pointer used only as a hash key. */
	const void *key;
};

struct pvsched_attachment {
	/*
	 * Serializes mutable admission/position and embedding runner state.
	 * Lock order is state_lock -> vCPU-hash lock and, through a visitor's
	 * scheduler setter, state_lock -> pi_lock -> rq.  Never take the
	 * task-hash lock under state_lock, or task_lock under either hash lock.
	 */
	raw_spinlock_t state_lock;
	/* Exact target task pinned from private initialization to destroy. */
	struct task_struct *task;
	/* Address space captured at initialization for local-hook admission. */
	struct mm_struct *owner_mm;
	/* Thread group captured at initialization for local-hook admission. */
	struct pid *owner_tgid;
	/* Session-lifetime owner, installed before hash publication. */
	struct pvsched_vcpu_runner *runner;
	/* Target-local lookup link, keyed by the stable task pointer. */
	struct hlist_node task_node;
	/* Remote-injection lookup identity, bound once on first local use. */
	struct pvsched_vcpu_key vcpu;
	/* Target-reported location; admission changes never rewrite this fact. */
	enum pvsched_runner_position position;
	/* Ordinary service admission, opened only by successful publication. */
	bool active;
	/* Task exit was observed, so scheduler restoration is unsafe. */
	bool exited;
	/* A missing or changed vCPU key permanently failed this binding. */
	bool binding_failed;
	/* Automatic resource teardown requested by a locked factual trigger. */
	bool teardown_requested;
	/* Final session close forbids any later cleanup-work enqueue. */
	bool cleanup_enqueue_closed;
	/* Whether a target-local hook has supplied a mode fact. */
	bool target_mode_valid;
	/* Last target-local mode flags used to validate remote callbacks. */
	u32 target_mode_flags;
	/* Whether task_node is currently reachable through the task hash. */
	bool task_hashed;
	/* Whether vcpu.node is currently reachable through the vCPU hash. */
	bool vcpu_hashed;
};

enum pvsched_attachment_visit_kind {
	PVSCHED_ATTACHMENT_VISIT_ORDINARY,
	PVSCHED_ATTACHMENT_VISIT_CLEANUP,
	PVSCHED_ATTACHMENT_VISIT_BINDING_FAILED,
};

typedef void (*pvsched_attachment_visit_fn)(struct pvsched_attachment *attachment,
					     enum pvsched_attachment_visit_kind kind,
					     void *data);
typedef void (*pvsched_attachment_commit_fn)(struct pvsched_attachment *attachment,
					      void *data);

/* Pin identity and prepare private state without making it visible to hooks. */
int pvsched_attachment_init(struct pvsched_attachment *attachment,
			    struct task_struct *task);
/*
 * Insert, revalidate, and open admission as the final commit step.  @commit
 * runs under state_lock after active becomes true and must not sleep.  It may
 * be NULL only while private setup has no guest-visible state to publish.
 */
int pvsched_attachment_publish(struct pvsched_attachment *attachment,
			       pvsched_attachment_commit_fn commit, void *data);

/*
 * Called by target-local hooks. The visitor runs with state_lock held and
 * must use a locked runner helper rather than re-entering a public runner API.
 */
bool pvsched_attachment_local_visit(const void *vcpu_key,
				    enum pvsched_runner_position position,
				    u32 mode_flags,
				    pvsched_attachment_visit_fn visit,
				    void *data);

/* Exit latches independently of ordinary owner-mm/TGID admission. */
bool pvsched_attachment_mark_exited(struct task_struct *task,
				    pvsched_attachment_visit_fn visit, void *data);

/*
 * Called by injection hooks. RCU spans the visit, and state_lock supplies the
 * final gate. The visitor must not attempt to acquire state_lock again.
 */
unsigned int pvsched_attachment_remote_visit(const void *vcpu_key,
					     u32 target_mode_flags,
					      pvsched_attachment_visit_fn visit,
					      void *data);

/* Close admission when state_lock is already held. */
void pvsched_attachment_disable_locked(struct pvsched_attachment *attachment);
/* Close admission from a caller that does not hold state_lock. */
void pvsched_attachment_disable(struct pvsched_attachment *attachment);
/* Remove both lookup nodes; callers must drain readers before reuse/free. */
void pvsched_attachment_unhash(struct pvsched_attachment *attachment);
/* Wait for readers of previously unhashed attachment nodes. */
void pvsched_attachment_drain(void);
/* Release identity references after unhashing and an RCU reader drain. */
void pvsched_attachment_release(struct pvsched_attachment *attachment);

#if IS_ENABLED(CONFIG_KUNIT)
void pvsched_attachment_owner_snapshot(struct task_struct *task,
				       struct mm_struct **mm,
				       struct pid **tgid,
				       bool *exiting);
#endif

#endif /* _VIRT_PVSCHED_ATTACHMENT_H */
