/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Factual KVM execution boundaries for the pvsched host framework.
 *
 * These are bare tracepoints without trace-event output: they only carry
 * facts from an architecture's KVM run loop to pvsched, which owns every
 * policy decision.  x86 KVM defines and exports them.  See
 * Documentation/virt/kvm/pvsched-v3.rst.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM kvm_pvsched

#if !defined(_TRACE_KVM_PVSCHED_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KVM_PVSCHED_H

#include <linux/tracepoint.h>
#include <linux/types.h>

struct kvm_vcpu;

#if IS_ENABLED(CONFIG_PVSCHED)
DECLARE_TRACE(kvm_pvsched_run_enter,
	      TP_PROTO(struct kvm_vcpu *vcpu),
	      TP_ARGS(vcpu));

DECLARE_TRACE(kvm_pvsched_run_leave,
	      TP_PROTO(struct kvm_vcpu *vcpu, int ret, u32 exit_reason),
	      TP_ARGS(vcpu, ret, exit_reason));
#endif

#endif /* _TRACE_KVM_PVSCHED_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
