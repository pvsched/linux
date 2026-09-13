/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_KVM_PVSCHED_H
#define _ASM_X86_KVM_PVSCHED_H

#include <linux/kconfig.h>

struct kvm_vcpu;

#if IS_ENABLED(CONFIG_PVSCHED)
void kvm_pvsched_run_enter(struct kvm_vcpu *vcpu);
void kvm_pvsched_run_leave(struct kvm_vcpu *vcpu, int ret);
#else
static inline void kvm_pvsched_run_enter(struct kvm_vcpu *vcpu) { }
static inline void kvm_pvsched_run_leave(struct kvm_vcpu *vcpu, int ret) { }
#endif

#endif /* _ASM_X86_KVM_PVSCHED_H */
