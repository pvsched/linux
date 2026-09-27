/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_PVSCHED_H
#define _UAPI_LINUX_PVSCHED_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PVSCHED_CONTROL_VERSION		1
#define PVSCHED_IOCTL_TYPE		0xb9

/* Control structs are native-endian, fixed-size, and contain no pointers. */
struct pvsched_info {
	__u32 control_version;
	__u32 flags;
	__u32 max_runners_per_session;
	__u32 max_runners_global;
	__u32 max_sessions_global;
	__u32 max_shm_pages_per_session;
	__u32 max_shm_pages_global;
	__u32 reserved;
};

struct pvsched_create_runner {
	/* Input: flags, runner_id, and reserved must be zero; tid is positive. */
	__u32 flags;
	__s32 tid;
	__aligned_u64 runner_id;
	__aligned_u64 reserved[2];
};

enum pvsched_runner_state {
	/* A pid association exists; this does not guarantee a live task. */
	PVSCHED_RUNNER_INACTIVE = 0,
	/* No task was associated with the pid when QUERY sampled it. */
	PVSCHED_RUNNER_EXITED = 1,
	/* An enabled shared-memory attachment admits runtime service. */
	PVSCHED_RUNNER_ACTIVE = 2,
};

#define PVSCHED_QUERY_RUNNER_LAST_FAULT_VALID	(1U << 0)

struct pvsched_query_runner {
	/* Input: runner_id is nonzero; every other field is zero. */
	__aligned_u64 runner_id;
	__u32 flags;
	__u32 state;
	__s32 last_fault_errno;
	__u32 reserved0;
	__aligned_u64 reserved1;
};

struct pvsched_attach_shm {
	/* user_addr is a fixed-width user address, not a C pointer. */
	__aligned_u64 runner_id;
	__aligned_u64 user_addr;
	__aligned_u64 size;
	__u32 flags;
	__u32 negotiation_status;
};

struct pvsched_detach_shm {
	__aligned_u64 runner_id;
	__aligned_u64 reserved[3];
};

/*
 * On any ioctl error, including a late EEXIST, output is unspecified.  The
 * one exception is ATTACH_SHM's EPROTO: negotiation completed and was
 * rejected, negotiation_status holds the reason, and no attachment remains.
 */

#define PVSCHED_GET_INFO \
	_IOR(PVSCHED_IOCTL_TYPE, 0, struct pvsched_info)
#define PVSCHED_CREATE_RUNNER \
	_IOWR(PVSCHED_IOCTL_TYPE, 1, struct pvsched_create_runner)
#define PVSCHED_QUERY_RUNNER \
	_IOWR(PVSCHED_IOCTL_TYPE, 2, struct pvsched_query_runner)
#define PVSCHED_ATTACH_SHM \
	_IOWR(PVSCHED_IOCTL_TYPE, 3, struct pvsched_attach_shm)
#define PVSCHED_DETACH_SHM \
	_IOW(PVSCHED_IOCTL_TYPE, 4, struct pvsched_detach_shm)

/* Shared-page ABI version and fixed wire sizes. */
#define PVSCHED_ABI_VERSION		1
#define PVSCHED_VCPU_STRIDE		4096
#define PVSCHED_NAME_MAX		32
#define PVSCHED_COMMON_HEADER_SIZE	64
#define PVSCHED_GUEST_AREA_SIZE		64
#define PVSCHED_HOST_AREA_SIZE		64
#define PVSCHED_FIXED_SIZE		(PVSCHED_COMMON_HEADER_SIZE + \
					 PVSCHED_GUEST_AREA_SIZE + \
					 PVSCHED_HOST_AREA_SIZE)
#define PVSCHED_EXTENSION_SIZE		(PVSCHED_VCPU_STRIDE - \
					 PVSCHED_FIXED_SIZE)

#define PVSCHED_DEFAULT_POLICY_NAME	"default"
#define PVSCHED_DEFAULT_POLICY_VERSION	1

/*
 * Each vCPU page contains a common negotiation header, a guest area, common
 * host feedback, and extension storage.  A custom policy can use the default
 * guest area or define its own.  The framework publishes the
 * common applied state in FRAMEWORK mode; the policy publishes it in POLICY
 * mode.  The built-in default policy uses the default guest area, publishes
 * its interrupt ticket in the default host area, and writes the remaining
 * reserved host and extension storage as zero.  A custom policy defines the
 * ownership and validation of the custom storage it uses.
 */

enum pvsched_status {
	PVSCHED_STATUS_DISABLED			= 0,
	PVSCHED_STATUS_ENABLED			= 1,
	PVSCHED_STATUS_UNKNOWN_POLICY		= 2,
	PVSCHED_STATUS_ABI_MISMATCH		= 3,
	PVSCHED_STATUS_POLICY_VERSION_MISMATCH	= 4,
	PVSCHED_STATUS_PROTOCOL_MISMATCH	= 5,
	PVSCHED_STATUS_MODE_MISMATCH		= 6,
};

enum pvsched_protocol {
	PVSCHED_PROTOCOL_DEFAULT	= 0,
	PVSCHED_PROTOCOL_CUSTOM		= 1,
};

enum pvsched_mode {
	PVSCHED_MODE_FRAMEWORK	= 0,
	PVSCHED_MODE_POLICY	= 1,
};

/* Common negotiation envelope, bytes 0..63. */
struct pvsched_header {
	/* Guest requests. */
	__le32 abi_version;
	__le32 policy_version;
	/* Nonempty, NUL-terminated, and zero-padded after the terminator. */
	char policy_name[PVSCHED_NAME_MAX];
	__le32 protocol_id;
	__le32 requested_mode;

	/* Host response. */
	__le32 status;
	__le32 host_abi_version;
	/* Ignored unless status is PVSCHED_STATUS_ENABLED. */
	__le32 accepted_mode;
	__le32 reserved;
};

/* One locally accessible task description in task-intent/applied-state snapshots. */
struct pvsched_prio_desc {
	__u8 sched_policy;
	__s8 nice;
	__u8 rt_prio;
};

/*
 * Producers must validate scheduling tuples before encoding them; see
 * Documentation/virt/kvm/pvsched-v3.rst.  All reserved fields and bits in
 * this ABI are written as zero.
 */
#define PVSCHED_INTENT_FLAG_PENDING_VALID (1U << 0)
/* current_task is the guest CPU's idle task: nothing is runnable there. */
#define PVSCHED_INTENT_FLAG_IDLE (1U << 1)
#define PVSCHED_INTENT_FLAGS_VALID \
	(PVSCHED_INTENT_FLAG_PENDING_VALID | PVSCHED_INTENT_FLAG_IDLE)

/*
 * The named members provide a local snapshot view only.  Shared-memory users
 * read or store the complete aligned raw word atomically and must not access
 * individual members in the shared page.  raw is already little-endian; do
 * not convert it before inspecting this byte view.
 */
union pvsched_task_intent {
	struct {
		struct pvsched_prio_desc current_task;
		struct pvsched_prio_desc pending_task;
		__u8 flags;
		__u8 reserved;
	};
	__aligned_le64 raw;
};

enum pvsched_cs_state {
	PVSCHED_CS_NMI			= 1ULL << 0,
	PVSCHED_CS_HARDIRQ		= 1ULL << 1,
	PVSCHED_CS_SOFTIRQ		= 1ULL << 2,
	PVSCHED_CS_PREEMPT_DISABLED	= 1ULL << 3,
};

#define PVSCHED_CS_VALID_MASK		0xfULL
#define PVSCHED_CS_RESERVED_MASK	(~PVSCHED_CS_VALID_MASK)

/* Default guest area, bytes 64..127. */
struct pvsched_default_guest_area {
	union pvsched_task_intent task_intent;
	__aligned_le64 cs_state;
	/* Atomically echo the opaque raw ticket after aggregate CS; zero is none. */
	__aligned_le64 interrupt_ack;
	__u8 reserved[40];
};

union pvsched_guest_area {
	struct pvsched_default_guest_area default_area;
	__u8 custom[PVSCHED_GUEST_AREA_SIZE];
};

enum pvsched_hint {
	PVSCHED_HINT_KICK_DEBOOST	= 1U << 0,
};

enum pvsched_applied_flag {
	PVSCHED_APPLIED_CS_THROTTLED	= 1U << 0,
	PVSCHED_APPLIED_TOTAL_THROTTLED	= 1U << 1,
};

/*
 * As with task_intent, named members are a local snapshot view only.  Shared
 * memory users access the complete aligned raw word atomically.
 */
union pvsched_applied_state {
	struct {
		struct pvsched_prio_desc task;
		__u8 hints;
		__u8 flags;
		__u8 reserved[3];
	};
	__aligned_le64 raw;
};

/*
 * Common host feedback, bytes 128..191.  The host publishes applied_state as
 * a coherent snapshot immediately before committed VMENTRY; it is not a live
 * asynchronous view.  The framework writes it in FRAMEWORK mode and the
 * policy writes it in POLICY mode.  Framework throttle flags are zero in
 * POLICY mode.  The default protocol uses the next word for interrupt_ticket;
 * a custom protocol owns all 56 bytes after applied_state.
 */
struct pvsched_host_area {
	union pvsched_applied_state applied_state;
	union {
		struct {
			/* Default protocol only; zero means no outstanding ticket. */
			__aligned_le64 interrupt_ticket;
			__u8 reserved[48];
		} default_area;
		__u8 custom[56];
	};
};

/*
 * One page per vCPU.  Default and custom guest-area protocols share the common
 * header and host feedback.  The custom guest area is the 64 bytes at
 * offset 64; policy-specific extension storage begins at byte 192.  The C
 * type has 8-byte alignment, while its backing page must be 4096-byte aligned.
 * The sections at offsets 0, 64, and 128 then occupy distinct 64-byte
 * cachelines.
 */
union pvsched_vcpu_page {
	struct {
		struct pvsched_header header;
		union pvsched_guest_area guest_area;
		struct pvsched_host_area host_area;
		__u8 extension[PVSCHED_EXTENSION_SIZE];
	};
	__u8 raw[PVSCHED_VCPU_STRIDE];
};

#endif /* _UAPI_LINUX_PVSCHED_H */
