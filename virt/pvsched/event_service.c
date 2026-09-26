// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kconfig.h>
#include <linux/kvm_host.h>
#include <linux/mutex.h>
#include <linux/tracepoint.h>
#include <trace/events/sched.h>
#include <kunit/visibility.h>

#include <asm/kvm_pvsched.h>
#include <trace/events/kvm_pvsched.h>

#include "attachment.h"
#include "event_service.h"
#include "runner_runtime.h"

static DEFINE_MUTEX(pvsched_event_service_lock);
static unsigned int pvsched_event_service_count;

/*
 * SVM AVIC and virtual NMI deliver interrupts and NMIs without the software
 * injection that the interrupt handoff ticket follows.  Such a vCPU keeps
 * every other assistance, but gets no ticket; its interrupts rely on the
 * guest's published critical-section state, as a posted interrupt to a
 * running VMX guest does.
 */
#define PVSCHED_KVM_MODE_NO_TICKET \
	(KVM_PVSCHED_MODE_SVM_AVIC | KVM_PVSCHED_MODE_SVM_VNMI)

VISIBLE_IF_KUNIT u32 pvsched_mode_flags(u32 flags)
{
	u32 mode = 0;

	if (flags & KVM_PVSCHED_MODE_NESTED)
		mode |= PVSCHED_RUNNER_MODE_NESTED;
	if (flags & PVSCHED_KVM_MODE_NO_TICKET)
		mode |= PVSCHED_RUNNER_MODE_NO_TICKET;
	if (flags & ~(KVM_PVSCHED_MODE_NESTED | PVSCHED_KVM_MODE_NO_TICKET))
		mode |= PVSCHED_RUNNER_MODE_UNSUPPORTED;
	return mode;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_mode_flags);

static void pvsched_run_enter(void *unused, struct kvm_vcpu *vcpu, u32 flags)
{
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_RUN_ENTER,
				   pvsched_mode_flags(flags), false, false);
}

static void pvsched_run_leave(void *unused, struct kvm_vcpu *vcpu, int ret,
			      u32 exit_reason)
{
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_RUN_LEAVE, 0,
				   false, false);
}

static void pvsched_vmentry(void *unused, struct kvm_vcpu *vcpu, u32 flags,
			    bool interrupt_ready)
{
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_CANCEL,
				   pvsched_mode_flags(flags), true,
				   interrupt_ready);
}

static void pvsched_vmexit_irqoff(void *unused, struct kvm_vcpu *vcpu)
{
	pvsched_runner_local_guest_exit_irqoff(vcpu);
}

static void pvsched_vmexit(void *unused, struct kvm_vcpu *vcpu, u32 flags,
			   bool hlt_exit)
{
	u32 mode = pvsched_mode_flags(flags);

	if (hlt_exit)
		mode |= PVSCHED_RUNNER_MODE_HLT_EXIT;
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_VMEXIT, mode,
				   false, false);
}

static void pvsched_vmentry_cancel(void *unused, struct kvm_vcpu *vcpu,
				   u32 flags)
{
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_CANCEL,
				   pvsched_mode_flags(flags), false, false);
}

static void pvsched_halt(void *unused, struct kvm_vcpu *vcpu, u32 flags)
{
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_HALT,
				   pvsched_mode_flags(flags), false, false);
}

static void pvsched_unhalt(void *unused, struct kvm_vcpu *vcpu, u32 flags)
{
	pvsched_runner_local_event(vcpu, PVSCHED_RECONCILE_UNHALT,
				   pvsched_mode_flags(flags), false, false);
}

static void pvsched_inject(void *unused, struct kvm_vcpu *vcpu, u32 flags,
			   bool source_guest_mode)
{
	pvsched_runner_remote_inject(vcpu, pvsched_mode_flags(flags),
				     source_guest_mode);
}

static void pvsched_process_exit(void *unused, struct task_struct *task,
				 bool group_dead)
{
	pvsched_attachment_mark_exited(task, NULL, NULL);
}

struct pvsched_event_probe {
	struct tracepoint *tp;
	void *probe;
};

/* Registered in this order and unregistered in the reverse order. */
static const struct pvsched_event_probe pvsched_event_probes[] = {
	{ &__tracepoint_kvm_pvsched_run_enter_tp, pvsched_run_enter },
	{ &__tracepoint_kvm_pvsched_run_leave_tp, pvsched_run_leave },
	{ &__tracepoint_kvm_pvsched_vmentry_tp, pvsched_vmentry },
	{ &__tracepoint_kvm_pvsched_vmexit_irqoff_tp, pvsched_vmexit_irqoff },
	{ &__tracepoint_kvm_pvsched_vmexit_tp, pvsched_vmexit },
	{ &__tracepoint_kvm_pvsched_vmentry_cancel_tp, pvsched_vmentry_cancel },
	{ &__tracepoint_kvm_pvsched_vcpu_halt_tp, pvsched_halt },
	{ &__tracepoint_kvm_pvsched_vcpu_unhalt_tp, pvsched_unhalt },
	{ &__tracepoint_kvm_pvsched_vcpu_inject_intr_tp, pvsched_inject },
	{ &__tracepoint_sched_process_exit, pvsched_process_exit },
};

/* The table loses the probe types; check each probe against its tracepoint. */
static void __maybe_unused pvsched_event_probe_types(void)
{
	check_trace_callback_type_kvm_pvsched_run_enter_tp(pvsched_run_enter);
	check_trace_callback_type_kvm_pvsched_run_leave_tp(pvsched_run_leave);
	check_trace_callback_type_kvm_pvsched_vmentry_tp(pvsched_vmentry);
	check_trace_callback_type_kvm_pvsched_vmexit_irqoff_tp(pvsched_vmexit_irqoff);
	check_trace_callback_type_kvm_pvsched_vmexit_tp(pvsched_vmexit);
	check_trace_callback_type_kvm_pvsched_vmentry_cancel_tp(pvsched_vmentry_cancel);
	check_trace_callback_type_kvm_pvsched_vcpu_halt_tp(pvsched_halt);
	check_trace_callback_type_kvm_pvsched_vcpu_unhalt_tp(pvsched_unhalt);
	check_trace_callback_type_kvm_pvsched_vcpu_inject_intr_tp(pvsched_inject);
	check_trace_callback_type_sched_process_exit(pvsched_process_exit);
}

static void pvsched_unregister_probes(unsigned int count)
{
	while (count--)
		tracepoint_probe_unregister(pvsched_event_probes[count].tp,
					    pvsched_event_probes[count].probe,
					    NULL);
}

static int pvsched_register_events(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(pvsched_event_probes); i++) {
		ret = tracepoint_probe_register(pvsched_event_probes[i].tp,
						pvsched_event_probes[i].probe,
						NULL);
		if (!ret)
			continue;
		if (i) {
			pvsched_unregister_probes(i);
			tracepoint_synchronize_unregister();
		}
		return ret;
	}
	return 0;
}

int pvsched_event_service_get(void)
{
	int ret = 0;

	/* Lock order: session mutex -> this mutex -> tracepoint machinery. */
	mutex_lock(&pvsched_event_service_lock);
	if (!pvsched_event_service_count) {
		ret = pvsched_register_events();
		if (ret)
			goto unlock;
	}
	pvsched_event_service_count++;
unlock:
	mutex_unlock(&pvsched_event_service_lock);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_event_service_get);

void pvsched_event_service_put(void)
{
	mutex_lock(&pvsched_event_service_lock);
	if (WARN_ON_ONCE(!pvsched_event_service_count))
		goto unlock;
	if (!--pvsched_event_service_count) {
		pvsched_unregister_probes(ARRAY_SIZE(pvsched_event_probes));
		tracepoint_synchronize_unregister();
	}
unlock:
	mutex_unlock(&pvsched_event_service_lock);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_event_service_put);

#if IS_ENABLED(CONFIG_KUNIT)
unsigned int pvsched_event_service_holders(void)
{
	unsigned int count;

	mutex_lock(&pvsched_event_service_lock);
	count = pvsched_event_service_count;
	mutex_unlock(&pvsched_event_service_lock);
	return count;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_event_service_holders);
#endif
