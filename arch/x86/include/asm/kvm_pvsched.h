/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_KVM_PVSCHED_H
#define _ASM_X86_KVM_PVSCHED_H

#include <linux/bits.h>
#include <linux/kconfig.h>
#include <linux/tracepoint-defs.h>
#include <linux/types.h>

struct kvm_vcpu;

/* Factual, combinable execution-state flags; these do not select a policy. */
enum kvm_pvsched_mode_flag {
	KVM_PVSCHED_MODE_NESTED		= BIT(0),
	KVM_PVSCHED_MODE_PROTECTED	= BIT(1),
	KVM_PVSCHED_MODE_SVM_AVIC	= BIT(2),
	KVM_PVSCHED_MODE_SVM_VNMI	= BIT(3),
	KVM_PVSCHED_MODE_VMX_SW_RMODE	= BIT(4),
};

#if IS_ENABLED(CONFIG_PVSCHED)
DECLARE_TRACEPOINT(kvm_pvsched_vmentry_tp);

void kvm_pvsched_run_enter(struct kvm_vcpu *vcpu);
void kvm_pvsched_run_leave(struct kvm_vcpu *vcpu, int ret);
void __kvm_pvsched_vmentry(struct kvm_vcpu *vcpu);
void kvm_pvsched_vmentry_cancel(struct kvm_vcpu *vcpu);
void kvm_pvsched_vmexit_irqoff(struct kvm_vcpu *vcpu);
void kvm_pvsched_vmexit(struct kvm_vcpu *vcpu);
void kvm_pvsched_vcpu_inject_intr(struct kvm_vcpu *vcpu);

/* Keep the disabled case a static branch in the vendor's entry path. */
static __always_inline void kvm_pvsched_vmentry(struct kvm_vcpu *vcpu)
{
	if (tracepoint_enabled(kvm_pvsched_vmentry_tp))
		__kvm_pvsched_vmentry(vcpu);
}
#else
static inline void kvm_pvsched_run_enter(struct kvm_vcpu *vcpu) { }
static inline void kvm_pvsched_run_leave(struct kvm_vcpu *vcpu, int ret) { }
static inline void kvm_pvsched_vmentry(struct kvm_vcpu *vcpu) { }
static inline void kvm_pvsched_vmentry_cancel(struct kvm_vcpu *vcpu) { }
static inline void kvm_pvsched_vmexit_irqoff(struct kvm_vcpu *vcpu) { }
static inline void kvm_pvsched_vmexit(struct kvm_vcpu *vcpu) { }
static inline void kvm_pvsched_vcpu_inject_intr(struct kvm_vcpu *vcpu) { }
#endif

#endif /* _ASM_X86_KVM_PVSCHED_H */
