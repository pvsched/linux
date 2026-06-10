.. SPDX-License-Identifier: GPL-2.0

==========================================
Paravirt Scheduling (pvsched) v3 — Design
==========================================

:Authors: Vineeth Pillai
:Status: Planning / implementation in progress (Linux 7.1 base)

.. contents::
   :local:

Problem
=======

A KVM guest suffers from *double scheduling*. The host scheduler manages the
vCPU threads (normally as CFS/FAIR tasks) without knowing what the guest is
running on them, while the guest scheduler picks tasks for a vCPU without
knowing where — or whether — that vCPU is physically running. When the host is
loaded, a latency-sensitive guest task is at the mercy of the host scheduler's
fairness and can be delayed arbitrarily. This hurts latency, power and resource
utilization.

Solution
========

Cooperative scheduling through shared memory. The guest publishes, per vCPU, its
current scheduling intent (whether it is in a kernel critical section, and the
requested policy / nice / rt_prio of the task it is about to run). The host
reads this and boosts or deboosts the corresponding vCPU thread accordingly
(via ``sched_setattr``). A misbehaving vCPU that stays boosted too long is
throttled after a timeout, so host workloads can still compete fairly.

History
=======

v1 (`LWN <https://lwn.net/Articles/955145/>`_)
    KVM did the handshake, the policy and the scheduling decisions, driven by
    hypercall/MSR. Rejected upstream: too much logic in KVM.

v2 (`LKML <https://lwn.net/Articles/968242/>`_, kernel 6.8.2)
    A ``virt/pvsched/`` framework in the host kernel. KVM did the handshake and
    called back (VMENTER / VMEXIT / HALT / INTR_INJ) into a registered driver
    (kernel module or BPF ``struct_ops``). Policy lived in the driver. KVM
    maintainers still did not want the handshake in the kernel.

v3 (this document)
    The handshake and negotiation move **out of KVM into the VMM (userspace)**.
    KVM gains only a few **bare tracepoints**; a host-kernel **framework**
    (``virt/pvsched/``) attaches to them and dispatches each event to a
    registered **policy** -- a kernel module or a BPF ``struct_ops`` -- which
    makes the boost/throttle decisions. (v2's KVM-resident callbacks become
    tracepoints; the policy stays a pluggable module/struct_ops as in v2.) The
    KVM dependency is reduced to "mostly the addition of a few tracepoints".

Architecture
============

Three moving parts:

Guest pvsched driver
    A guest kernel-mode driver (BPF later) that binds a pvsched virtual PCI
    device, allocates a shared page (an array of per-vCPU structures), writes
    the page's guest physical address (GPA) into a device BAR, and registers to
    guest tracepoints. Today it attaches ``sched_switch`` (to publish the next
    task's policy / nice / rt_prio into ``guest_area``) and
    ``{irq,softirq}_{entry,exit}`` (to set the HARDIRQ / SOFTIRQ
    in-kernel-critical-section bits). Further intent sources --
    ``sched_wakeup*``, ``exit_to_user``, and the NMI and PREEMPT_DISABLED
    kern_cs bits -- are planned (NMIs have no bracketing tracepoint; preempt
    on/off needs a debug Kconfig).

VMM pvsched device (out of tree)
    Exposes the PCI device to the guest, owns the handshake, converts the
    guest's shared-memory GPA to a host virtual address, and *names* the
    policy the VM should use (a device property; policies themselves are
    loaded host-side into the framework's registry by the management plane --
    the VMM stays deprivileged). Runs as a separate sandboxed process. The
    kernel owns the ABI (the page contract and the transport-binding header),
    not the VMM code.

Host framework and policy
    The **framework** (``virt/pvsched/pvsched.c``, ``CONFIG_PARAVIRT_SCHED_HOST``,
    a module) is the piece that attaches to the four ``kvm_pvsched_*``
    tracepoints, and owns the per-vCPU machinery: the ``/dev/pvsched``
    registration ioctls, the pinned shared-page mapping, the task-keyed binding
    table, and the boost time cap. It dispatches each event to the **bound
    policy**. A **policy** implements ``struct pvsched_policy_ops``
    (``vmentry`` / ``vmexit`` / ``halt`` / ``inject`` callbacks) and registers
    via ``pvsched_register_policy()`` as a kernel module, or as a BPF
    ``struct_ops`` (``bpf_pvsched_policy_ops``, precedent: tcp congestion-control
    ops). Its callbacks boost on halt and interrupt injection, translate the
    guest-requested parameters to the vCPU thread on vmexit, and publish status
    in ``host_area``; the framework's cap throttles a vCPU boosted too long. The
    policy never touches tracepoints itself -- the framework does, and calls it.

Shared memory ABI
=================

ABI version 1 (``include/uapi/linux/pvsched.h``). The guest allocates one page
per vCPU it wants paravirt-scheduled and hands the host each page's
guest-physical address via a discovery transport (see "Discovery transport").
Each page has three sections::

    #define PVSCHED_ABI_VERSION 1
    #define PVSCHED_VCPU_STRIDE 4096   /* one page per vCPU; ABI constant, not PAGE_SIZE */

    struct pvsched_header {            /* @0,  64B  -- negotiation */
        __u32 abi_version, policy_version;   char policy_name[32];   /* guest-written */
        __u32 status, host_abi_version;          /* host-written */   __u32 reserved[4];
    };
    struct pvsched_guest_area { __u8 kern_cs, sched_policy; __s8 nice; __u8 rt_prio; ... }; /* @64, 32B */
    struct pvsched_host_area  { __u8 vcpu_status, sched_policy; __s8 nice; __u8 rt_prio; ... }; /* @96, 32B */
    union  pvsched_vcpu_page  { struct { header; guest_area; host_area; }; __u8 reserved[PVSCHED_VCPU_STRIDE]; };

The **header** is the negotiation: the guest advertises the ABI version and the
``policy_name`` + ``policy_version`` it wants; the host writes ``status``
(``enum pvsched_status``: DISABLED / ENABLED / UNKNOWN_POLICY / ABI_MISMATCH /
POLICY_VERSION_MISMATCH) after matching the request against its registered
policies. The match is checked in order -- abi (``abi_version`` sits at offset 0
so it is readable across ABI revisions), then policy name, then policy version
-- each with its own status. ``status == ENABLED`` is the bit the guest's
scheduler hooks gate on; they are no-ops until the host enables them, so a guest
with the feature runs unchanged on a non-pvsched host.

``guest_area`` is updated by the guest from scheduler events; ``host_area`` is
written by the host, and carries publishing *hints* the policy sets for the
guest core -- e.g. ``PVSCHED_HINT_KICK_DEBOOST``: when a published boost-class
request drops to non-boost, the guest induces an immediate exit (a write to
the transport's kick register; the trap itself is the event) so the host can
deboost without waiting for a natural vmexit. Boosts are taken lazily, but
releases are signalled promptly. ``kern_cs`` bits: NMI / HARDIRQ / SOFTIRQ /
PREEMPT_DISABLED. ``vcpu_status``: VMENTERED / VMEXITED / HALTED / INTR_INJECTED
(base in the low nibble, interrupt injection OR'd into the high nibble).

Each vCPU gets a full page (a fixed 4096-byte stride -- a constant, not the
kernel ``PAGE_SIZE``, since guest and host may differ): no false sharing, and
room for the areas to grow. Incompatible changes bump ``PVSCHED_ABI_VERSION``.

Discovery transport
===================

How the per-vCPU page addresses travel from guest to host is deliberately kept
out of the core ABI: ``uapi/linux/pvsched.h`` defines only the page contract
and the VMM-side registration ioctl, both transport-independent (mirroring
virtio's core-versus-transport split). A transport binding delivers the
guest's set of ``{vCPU id, page GPA}`` entries to the VMM and defines the
point at which the host-written ``status`` becomes visible to the guest.

The first binding -- ``uapi/linux/pvsched_pci.h``, added with the guest driver
-- is a minimal PCI device::

    BAR0 (control): doorbell + advertised policy + kick registers (u32).
    BAR1 (table):   struct pvsched_pci_entry { __u64 apic_id, gpa; }[];  /* one page */

(``uapi/linux/pvsched_pci.h`` is the authoritative layout -- register offsets,
``struct pvsched_pci_entry``, PCI ids; the sketch above is illustrative.)

The device advertises the policy name/version this VM should request
(read-only BAR0 registers, fed from VMM configuration, e.g.
``-device pvsched,policy=demo``); the guest echoes the advertisement into the
page headers, subject to its admin's acceptance list
(``pvsched_guest.allow=``, default accept-all). The guest fills each opted-in
vCPU's page header and its BAR1 ``{apic_id, gpa}`` entry, then writes the number
of valid entries to the doorbell. That MMIO write traps to the VMM, which
translates each GPA and calls the pvsched framework to register the page (keyed
by the vCPU thread). The
page is an opaque handle the VMM only couriers: it is guest<->host memory the
framework pins and reaches through its own kernel mapping, never the VMM itself.
The framework validates the header and writes back ``status`` (the per-vCPU
ioctl also returns ``-ENOENT`` when no policy matches, or ``-EEXIST`` on a
double register; see ``uapi/linux/pvsched.h`` for the exact return codes). The
trap being synchronous, the guest reads each header's ``status`` on return, no
polling needed. A future virtio or platform-device binding can replace this without
touching the page contract; only the status-visibility point moves (e.g. a
completion notification instead of the retiring doorbell write).

Implementation phases (Not strictly in the chronological order)
=====================

Phase 0 — ABI & scaffolding
    ``include/uapi/linux/pvsched.h`` (versioned), ``CONFIG_PARAVIRT_SCHED_HOST``,
    reuse ``virt/pvsched/`` for host-side kfunc/helper code.

Phase 1 — KVM tracepoints (chosen event source)
    Add ``tp_btf``-attachable **bare tracepoints** (``DECLARE_TRACE``, defined in
    ``include/trace/events/kvm.h``), each passing ``struct kvm_vcpu *`` for BTF
    access: ``kvm_pvsched_vmentry`` and ``kvm_pvsched_vmexit`` in
    ``vcpu_enter_guest`` and ``kvm_pvsched_vcpu_halt`` in the ``kvm_vcpu_block``
    halt path, plus ``kvm_pvsched_vcpu_inject_intr`` in ``__apic_accept_irq``
    (x86) when an interrupt is accepted for the vCPU. ``kvm_pvsched_vmentry``
    fires *after* the final ``kvm_vcpu_exit_request()`` bail-out, i.e. only once
    guest entry is committed, so the policy publishes ``host_area`` exactly once
    right before the guest runs and an aborted entry never runs it (it would
    fire without a matching vmexit and could consume state the guest never
    sees). That point is IRQs-disabled, so the vmentry handler must be cheap
    (a map lookup and a small ``host_area`` write).
    ``kvm_pvsched_vmexit`` fires after ``guest_timing_exit_irqoff``.

    Bare tracepoints are deliberately **not exposed in tracefs** -- they are an
    internal attach point for the host policy, not a user-facing tracing ABI --
    but remain attachable from BPF via ``tp_btf/<name>_tp`` (e.g.
    ``tp_btf/kvm_pvsched_vmentry_tp``), exactly as the scheduler's own
    ``pelt_*``/``sched_*`` bare tracepoints work. They are always compiled in; a
    tracepoint with no attached probe is a patched-out static branch, so there
    is zero overhead when unused (no Kconfig gate).

    These hooks live in x86 KVM for a fast initial dev cycle. A later change
    will make pvsched arch-agnostic by moving the call sites (vmentry/vmexit/
    halt and the interrupt path) out of arch code, converting the interrupt hook
    to a reason-carrying ``kvm_vcpu_kick()`` so non-x86 architectures are
    covered.

Phase 2 — Scheduler kfunc for vCPU priority
    ``pvsched_set_params(pid, policy, nice, rt_prio)`` (in
    ``virt/pvsched/pvsched.c``) sets a vCPU thread's scheduling parameters from
    a pvsched policy. It is exported for policy modules and registered as a
    kfunc for BPF ``struct_ops`` policies (precedent: the tcp
    congestion-control functions, exported for CC modules and registered as
    struct_ops kfuncs). It resolves the pid via ``find_get_task_by_vpid()`` and
    applies the parameters with ``sched_setattr_pi_nocheck(p, &attr, pi=false)``.

    The ``pi=false`` is what makes it callable from every pvsched tracepoint
    context, including the IRQ context of ``kvm_pvsched_vcpu_inject_intr``:
    ``__sched_setscheduler`` only ``BUG_ON``\s on ``pi && in_interrupt()``, and
    its one sleeping path (``cpuset_lock()``) is ``SCHED_DEADLINE``-only, so the
    kfunc rejects ``SCHED_DEADLINE`` and is otherwise atomic-safe (it only takes
    the rq/pi raw spinlocks). Deferral (``irq_work``/``task_work``) was
    considered and rejected: boosting a *preempted* vCPU so the host scheduler
    runs it must be applied synchronously by the (already running) caller —
    deferring to the target is circular.

    The boost is bounded by a per-task **cap** -- a lock-free ``NONE`` /
    ``BOOSTED`` / ``THROTTLED`` state machine plus a hard-IRQ ``hrtimer``
    (``HRTIMER_MODE_REL_HARD``, so the boosted task cannot starve it). The first
    boost arms the timer on the ``NONE -> BOOSTED`` edge; at expiry the task is
    forced to baseline and enters ``THROTTLED``, refusing further boosts until a
    cooldown elapses (``THROTTLED -> NONE``). ``pvsched_set_params`` is
    **apply-then-claim**: it applies the boost, then claims the state with a
    ``cmpxchg``; if a concurrent cap-timer throttle won the race (the claim
    returns ``THROTTLED``), it undoes the boost -- and that undo stays *after*
    the apply, so it is the last write to the task's priority and the throttle
    still wins. The cap durations are the module parameters
    ``pvsched_max_boost_ns`` / ``pvsched_throttle_ns`` (1 s each by default).

Phase 3 — Shared memory access (framework-pinned kernel mapping)
    At registration -- a sleepable ioctl issued by the VMM itself -- the
    framework pins the vCPU's shared page (``pin_user_pages_fast``,
    ``FOLL_WRITE | FOLL_LONGTERM``, so the pin targets the caller's, i.e. the
    VMM's, mm) and stores the page's kernel mapping in the per-vCPU state.
    Policy callbacks receive that mapping and use plain loads/stores: residency
    is guaranteed (no faults), access is cheaper than per-event uaccess, and --
    decisively -- it works from **any** callback context. In particular the
    injection event, which runs in the injector's context where user-memory
    helpers are unusable, can publish ``INTR_INJECTED`` directly instead of
    deferring it to the vCPU's next vmentry. The page is unpinned whenever the
    binding drops (unregister, vCPU task exit, module unload), after the RCU
    grace period that quiesces in-flight dispatch. A ``sched_process_exit`` probe
    is the safety net for a vCPU thread that exits without an
    ``UNREGISTER_VCPU``: it unhashes the entry under the lock and defers the
    sleepable teardown (grace period, cap-timer cancel, unpin, free) to a
    workqueue.

    Pre-upstream notes: the pin should be charged against the VMM's memlock
    limit (io_uring-style accounting), and fd-backed guest memory
    (``guest_memfd``) cannot be pinned this way.

    (A policy that bypassed the framework and attached ``tp_btf`` directly would
    instead need ``bpf_probe_read_user`` / ``bpf_probe_write_user`` with
    VMM-supplied user VAs, plus a deferred-publish trick for the injection event
    -- which is why the in-tree path goes through the framework's pinned mapping
    instead.)

Phase 4 — Demo policy + tests
    The in-tree **demo policy** (``virt/pvsched/pvsched_demo.c``,
    ``CONFIG_PVSCHED_DEMO``, a module) implements ``pvsched_policy_ops`` end to
    end -- boost on inject/halt, 1:1 param translation on vmexit, the framework
    cap -- and exposes event/outcome counters at ``debugfs pvsched_demo/stats``
    for tests to assert against. A BPF twin, ``bpfdemo``, gives the same
    behaviour as a ``struct_ops`` policy. (An earlier standalone ``tp_btf`` BPF
    program with a mock shared page was the bring-up harness, since retired in
    favour of the in-tree module and struct_ops policies.) Host-side tests drive
    a stand-in vCPU with a tiny in-process KVM guest -- no VMM or guest-driver
    dependency.

Phase 5 — Guest core + transport
    Split, mirroring the host's core/transport layering. A built-in **core**
    (``virt/pvsched/pvsched_guest.c``, ``CONFIG_PARAVIRT_SCHED_GUEST=y`` --
    built-in because it attaches unexported core tracepoints) owns the per-vCPU
    pages, the negotiation, and the publishing probes, and exposes a small
    prepare / commit / teardown API (``include/linux/pvsched_guest.h``). A
    separate **transport** module (``pvsched_guest_pci.c``,
    ``CONFIG_PVSCHED_GUEST_PCI``) binds the PCI device and couriers the
    ``{apic_id, gpa}`` set and the host-written statuses between the core and the
    VMM. The PCI binding is ``uapi/linux/pvsched_pci.h`` (BAR layout, doorbell,
    ``pvsched_pci_entry``, PCI ids).

Phase 6 — VMM pvsched device (out of tree) + end to end
    The PCI device + BAR ABI in the VMM, GPA -> HVA translation, and a per-vCPU
    ``/dev/pvsched`` registration driven by the doorbell (the VMM is a courier
    passing an opaque page handle; it never dereferences the page). Reproduce the
    cyclictest idle/busy-host numbers.

Cross-cutting concerns
======================

* Security: /dev/pvsched registration requires ``CAP_SYS_NICE`` (on top of the
  root-only device node) -- it delegates to the host policy the authority to
  raise vCPU threads' priority, which that capability already governs; the
  throttle is the safety valve against a rogue or buggy guest; admin caps bound
  the maximum boost; the feature is opt-in per VM.
* Upstream sequencing: Phases 1-3 are generic and reviewable on their own and
  should be submitted as an independent series before the full feature.
* Baseline restoration: throttle / unregister currently reset the vCPU thread to
  ``SCHED_NORMAL`` / nice 0, which is wrong for a VMM that runs vCPU threads at a
  non-default priority. The fix -- snapshot the thread's parameters at
  registration and restore those (as v2 did) -- is a pre-upstream TODO.
* Key risks: (a) calling ``__sched_setscheduler`` from atomic context;
  (b) pinned shared-page lifetime vs binding teardown; (c) verifier acceptance of the
  shared-memory kfuncs; (d) ``tp_btf`` argument stability across KVM changes.
