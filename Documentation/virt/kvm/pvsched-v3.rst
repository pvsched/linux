.. SPDX-License-Identifier: GPL-2.0

====================================
Paravirtualized scheduling (pvsched)
====================================

:Author: Vineeth Pillai
:Status: ABI, inactive control, request/accounting cores, scheduler bridge,
         and factual KVM run/inner hooks

Overview
========

A virtual CPU is scheduled twice: the guest scheduler chooses a guest task,
and the host scheduler chooses when the virtual CPU thread runs.  The host
normally cannot see the latency requirements of the guest task.  Under host
load, that mismatch can add latency even when the guest made the right local
scheduling decision.

pvsched is a cooperative mechanism through which a guest publishes scheduling
intent in one shared page per virtual CPU.  A host framework can use that intent
to select temporary scheduling parameters for the corresponding virtual CPU
thread.  The framework, policy implementations, KVM event sources, transport,
and lifecycle interfaces are deliberately separate from the shared-page ABI.

The ABI version 1 shared-page representation, deterministic budget arithmetic,
built-in request selection, an inactive runner-identity control plane, the
restricted scheduler bridge, and factual KVM run and inner-loop hooks are
defined at this stage.  There is no shared-page attachment, transport binding,
or active pvsched runtime yet.

Architecture
============

The planned design has four components:

* A guest core publishes current and pending task intent and aggregate kernel
  critical-section state.
* A VMM transport negotiates a host-authorized policy, protocol, and enforcement
  mode, and carries per-vCPU page addresses across the guest/host boundary.
* A host framework owns binding lifetime, the built-in default policy, and the
  accounting and enforcement used by framework-managed bindings.
* Optional privileged kernel or BPF policies may customize event decisions.
  Enforcement ownership is fixed when a binding is created: framework-managed
  policies return requests for framework service, while explicitly selected
  policy-managed bindings own their scheduling and throttling behavior.

x86 KVM provides factual architecture-run and inner guest-entry/exit events,
but does not own policy selection, shared-page negotiation, or boost
accounting.  The scheduler bridge and KVM hooks are prerequisites only: no
active runtime currently binds them to the shared-page ABI.

Budget accounting core
======================

The host includes a deterministic internal core for the two framework-managed
boost debts.  Internal constants provide a 2 ms default critical-section limit
and a 500 ms default generic-boost limit.  No runtime controls for these
defaults exist at this stage.  The generic budget covers every charged boost
interval: guest task execution, guest critical-section execution, and an
explicit custom VMM boost.  Critical-section execution additionally drains the
CS budget.  Uncharged time recovers each debt independently at a 1:1 rate.
Additions saturate instead of wrapping, recovery floors at zero, and a throttle
latch clears only when its debt reaches zero.

The core consumes explicit durations classified by actual applied execution.
An interval containing a classification transition must be split in time order.
It stores no clock or timestamp and provides no locking, timer, eligibility,
scheduler, or enforcement mechanism.  Future serialized account code must
measure and order intervals, preserve account lifetime, and act on the latches.
The core is exercised through a separate KUnit test configuration; it is not
linked into an active pvsched runtime at this stage.

Built-in request selection
==========================

The selector expects a snapshot of the complete default guest area that its
caller validated; it validates configuration and baseline even when
throttled.  It maps NORMAL and BATCH to host NORMAL with the guest nice
value, IDLE to host IDLE, FIFO and RR to capped host FIFO, and guest DEADLINE
to host FIFO at a separately configured priority.  It never requests host
DEADLINE.  Current and valid pending task descriptions are compared after
mapping, with the current description retained on a tie.

Generic throttling suppresses CS elevation and clamps each above-baseline task
candidate to the saved NORMAL baseline.  The usual task rule is then applied:
pending replaces the clamped current request only when pending is strictly more
urgent.  Thus a current below-baseline NORMAL nice or IDLE request survives,
but a stale, less-urgent pending hint cannot deboost below current's clamped
intent.  Without generic throttling, an active, eligible critical section
selects the configured CS FIFO request; a throttled CS request reveals the
independently selected task request.  There is no unconditional baseline
priority floor.  Invalid input produces no selection.

The selector neither accesses shared memory nor applies scheduling, updates
accounting, or retains a previous decision.  Its explicit priority controls
have no runtime configuration interface yet.  Later integration must define
how invalid runtime snapshots are handled and must account actual successfully
applied execution rather than a selected request.

Inactive runner control
=======================

Opening ``/dev/pvsched`` with read/write access creates a session.  The opener
and every ioctl require ``CAP_SYS_NICE`` in the initial
user namespace.  ``GET_INFO`` reports the control version and resource limits;
userspace reads the limits from it, as the UAPI header does not define them.
``CREATE_RUNNER`` resolves a caller-owned thread ID once and retains its
``struct pid`` identity under a session-local, non-reused runner ID.
``QUERY_RUNNER`` reports whether that identity currently has an associated
task.  A retained pid prevents numeric PID reuse from retargeting a runner but
does not keep a task alive.

``QUERY_POLICY`` lists the registered host policies; a session must select one
with ``SET_POLICY``, once, before its first ``CREATE_RUNNER``, which otherwise
fails with ``EINVAL``.

The session retains both the opening TGID and mm identity.  A TGID can survive
exec, while an mm identifies the original address space; conversely,
``CLONE_VM`` can share an mm across distinct thread groups.  Therefore every
ioctl must match both.  The mm reference prevents reuse of the
``mm_struct`` without retaining its userspace mappings.  ``O_CLOEXEC`` is
recommended VMM hygiene, not an ownership mechanism.

Authorization is checked before command dispatch.  An unauthorized caller
therefore receives ``EPERM`` even for an unknown command; an authorized caller
receives ``ENOTTY`` for an unknown command.

Runner identities remain inactive: creation captures no scheduling baseline,
attaches no shared page, and cannot boost or otherwise change the target.
Exit is sampled rather than latched, and stale registrations consume quota
until final session close.  A thread-ID association can change during exec;
future runtime activation must revalidate its owner and task context rather
than treating a retained pid as an immutable task identity.  Final close
removes every runner owned by the session.

Session and runner counts are bounded globally and per session.  Global counts
include reservations for opens and creates still in flight, so concurrent
operations cannot transiently exceed the advertised limits.  Failed operations
return their reservations; published runners retain quota until final close.

x86 KVM factual boundaries
==========================

x86 KVM calls ``kvm_pvsched_run_enter`` and ``kvm_pvsched_run_leave`` wrappers
around ``kvm_arch_vcpu_ioctl_run()``.  The underlying hooks are bare
tracepoints declared in ``include/trace/events/kvm_pvsched.h``; their symbols
carry the ``_tp`` suffix, and x86 KVM defines and exports them.  Generic
KVM has already validated the ioctl argument and updated the runner pid, and
x86 KVM has completed MMU post-initialization, before entry.  Leave occurs on every
architecture-run path after entry and reports the signed result plus a raw
exit-reason snapshot.  Entry also reports the current mode flags described
below.  The exit reason is useful when the result is zero, but remains
userspace-writable and untrusted and does not prove guest entry.  Consumers
must ignore it when the result is negative.

Generic failures, including invalid arguments, and MMU post-initialization
failures publish neither event.  A complete pair does not prove that guest
entry occurred: immediate exit, signal interruption, and later architecture
failures can all produce a pair.  Callbacks run synchronously with the vCPU
mutex held, must not sleep, and may use the vCPU and current task pointers only
for the duration of the callback.

These events delimit the outer VMM/KVM execution boundary, not hardware VM
entry and exit.  Hardware VM exits are often handled inside KVM without a
return to the VMM, while KVM_RUN can return without a hardware entry or exit.
Future policy handling will restore the default-policy baseline at RUN_LEAVE.
A custom policy may instead request a scheduling boost for VMM I/O handling;
RUN_ENTER occurs before vCPU loading, blocking, and userspace-I/O completion so
that policy can reconsider the request early.  Guest-execution budget
accounting will begin at the late VMENTRY-attempt boundary, not at RUN_ENTER,
and will conservatively include a failed hardware entry.  Failures rejected
before RUN_ENTER also remain outside these hooks.  KVM only reports the factual
boundaries; pvsched owns policy and must independently cap or restore a future
framework-managed VMM boost if KVM_RUN is rejected before RUN_ENTER or is never
called again.  None of that policy, accounting, or scheduling behavior is
implemented by these factual events.

The inner hooks report these additional facts:

* ``kvm_pvsched_vmentry`` reports an entry attempt at the latest vendor point
  that needs no hardware-state rollback.  It carries fresh mode flags and
  target-side interrupt eligibility, which the vendor module reports through
  its ``pvsched_interrupt_ready`` operation.  The callback cannot veto entry
  or change ``vcpu->mode``.  On both VMX and SVM, the interval from VMENTRY
  to its terminal IRQ-off fact includes the bounded LAPIC timer-advance
  wait; VMX also includes PT/perf preparation.  The wait is capped at 5
  microseconds.
* ``kvm_pvsched_vmexit_irqoff`` reports a future pinned-timer cancellation
  opportunity after host PKRU has been restored and before host interrupts are
  enabled for every vendor return.  It does not close or settle an accounting
  window.  A VMEXIT fact does not prove that hardware entry occurred because
  vendor code can return without entering the guest; synthetic VMX and SVM
  returns use the same terminal path even if no VMENTRY fact occurred.
* ``kvm_pvsched_vmexit`` reports the slow reconciliation checkpoint after
  host interrupts are enabled and before preemption is enabled.  A future
  consumer closes or settles its guest accounting window at this IRQ-on fact.
  The fact also reports whether the exit is a non-nested ``HLT`` exit,
  whether or not KVM has handled it yet, so a consumer can keep a halting
  vCPU boosted until it blocks.
  ``kvm_pvsched_vmentry_cancel`` reports the equivalent checkpoint for a
  common request cancellation before KVM calls the vendor.  Such a
  cancellation has no matching IRQ-off event because there was no vendor
  return.
* ``kvm_pvsched_vcpu_halt`` and ``kvm_pvsched_vcpu_unhalt`` bracket an actual
  x86 halted-vCPU call to ``schedule()``.  Wait-for-SIPI and other generic KVM
  blocking states do not emit them.
* ``kvm_pvsched_vcpu_inject_intr`` reports an accepted fixed/lowest-priority
  LAPIC interrupt or queued NMI.  It is a wake opportunity, not proof that the
  guest handled the interrupt.  It also reports whether the injecting context
  is a vCPU that may still have guest state loaded (any mode except
  ``OUTSIDE_GUEST_MODE``), so a consumer can avoid scheduler work there.

VMENTRY facts are not paired one-for-one with terminal facts.  A fast loop can
publish multiple VMENTRY attempts before one terminal pair.  Conversely, a
common cancellation before vendor entry publishes only the IRQ-on
VMENTRY_CANCEL fact.  A future pvsched consumer must therefore retain its own
guest-window state; a repeated VMENTRY keeps the open window, while the IRQ-on
terminal fact closes a window opened by an earlier fast attempt.  The IRQ-off
fact only gives that consumer an opportunity to cancel its pinned cap timer.

The mode-sensitive entry, exit, halt and injection hooks report fresh internal
mode flags.  RUN_LEAVE retains its result and exit-reason fields.  The flags
are factual and combinable; they do not select a policy or a single execution
mode.  They identify nested execution, confidential or encrypted-memory
execution, configured SVM AVIC, configured SVM virtual NMI and VMX software
real mode.  The early and remote paths use only state that is safe to sample
without reading a remote VMCS.  VMENTRY additionally examines prepared
hardware injection state on the target CPU.  The flags remain facts for a
pvsched consumer to act on; KVM does not enforce pvsched policy.  SVM
reports PROTECTED for SEV, SEV-ES and SNP; VMX reports it for TDX, including a
debug TDX guest.

``interrupt_ready`` means that the next hardware attempt has a prepared
external interrupt or NMI opportunity.  VMX also recognizes an eligible L1
virtual-interrupt-delivery candidate after PIR synchronization, including an
older IRR candidate, when interrupt-window exiting is disabled and IF,
interrupt shadow and APIC priority allow it.
Nested, protected, VMX software-real-mode and configured SVM AVIC/vNMI cases
report false.  This is neither interrupt-handler entry nor delivery proof.

There is no special VMENTRY hook inside the TDX run path.  The VMM must reject
pvsched for confidential guests, and RUN_ENTER plus every fresh boost-source
fact reports PROTECTED so a consumer can fail closed.  Attachment itself must
not boost.  A temporary mid-run active attachment before the next factual hook
can therefore remain at baseline; that attachment sequence is unsupported.

Posted-interrupt coverage is deliberately incomplete.  An interrupt posted to
an already-running guest normally relies on the guest critical-section signal;
the host accepts the small race in which the guest exits before reaching its
instrumented handler.  Direct device or IOMMU posting to an already-runnable
vCPU that the host has descheduled may execute no KVM producer callback, so no
fact can accelerate that initial host runqueue wait.  Target-side eligibility
is still reported when the vCPU is selected and prepares to enter.

VMX software-assisted real mode reports the mode bit but does not report an
IRQ/NMI handoff opportunity.  Real mode with unrestricted guest remains in the
ordinary supported VMX path.  The software-assisted case is deferred because
its event injection is emulated outside the hardware-entry contract above.

All callbacks run synchronously, must not sleep and borrow the vCPU pointer
only for their duration.  They are not dispatched from host NMI context.

Shared-page ABI
===============

Each vCPU page is exactly 4096 bytes.  All multi-byte fields are little-endian.
Atomic wire words use
``__aligned_le64``; consumers must use an atomic 64-bit access appropriate to
their environment.  The C type requires eight-byte alignment; a later
transport must enforce 4096-byte alignment for the backing page.  The common
header, guest area, and host feedback begin at offsets 0, 64, and 128, so
each occupies a separate 64-byte cacheline when the backing page is properly
aligned.  Structures are not packed and contain no C bitfields.

Common header
-------------

Bytes 0 through 63 form a common negotiation envelope.  The guest requests an
ABI version, policy name and version, guest-area protocol, and enforcement mode.
All guest-written fields occupy bytes 0 through 47, followed by the
host-written response fields in bytes 48 through 59 and reserved bytes 60
through 63.

The policy name is nonempty, NUL-terminated within its 32-byte field, and
zero-padded after the terminator.  ``accepted_mode`` is ignored unless status
is ``PVSCHED_STATUS_ENABLED``.

The host reports negotiation status, its ABI version, and the accepted mode.
Guest fields are requests, not authority to select an unbounded enforcement
mode.  A transport must report failures that occur before the host can safely
pin and update the page.

The built-in policy identity is ``default`` version 1.  It requires the default
guest-area protocol and framework-managed enforcement.  Custom policies declare
their guest-area protocol when they are registered.  A guest may only request
the policy its VMM selected for the session: any other name is
``UNKNOWN_POLICY``, and another version ``POLICY_VERSION_MISMATCH``.  After
the framework's own checks, the policy's optional ``negotiate`` callback may
refuse the request with ``DISABLED``.
An enabled status is meaningful only after all requested values match the
host-authorized selection.

Default guest area
------------------

For the default guest-area protocol, bytes 64 through 127 contain three
independently owned words:

``task_intent``
  The guest scheduler publishes the current task description, an optional
  pending-task urgency hint, and a pending-valid bit as one 64-bit value.  Each
  task description contains an 8-bit Linux ``SCHED_*`` class, an 8-bit signed
  two's-complement nice value, and an 8-bit real-time priority.  The scheduling
  class is task state and is distinct from the negotiated pvsched policy.
  Task-intent writers will be serialized by the target runqueue lock.

``cs_state``
  The local guest CPU publishes aggregate NMI, hardirq, softirq, and
  preemption-disabled state.  These bits summarize actual nesting; they are not
  nesting counters.  This word is separate so task-intent writers cannot
  overwrite critical-section state.

``interrupt_ack``
  After publishing aggregate critical-section state, the guest echoes a
  nonzero interrupt ticket into this word to acknowledge the handoff.  The
  ticket is an opaque generation: the guest copies the complete raw word and
  does not interpret or byte-swap it.  Zero means no acknowledgment, and a
  fresh attachment requires this word to be zero.

``struct pvsched_prio_desc`` and the named members of
``union pvsched_task_intent`` provide a byte-oriented view of a local snapshot.
Shared-memory readers and writers access only the complete aligned ``raw`` word
atomically, never individual members in the shared page.  ``raw`` already has
little-endian wire representation and must not be byte-swapped before using the
member view.  Byte 6 contains the pending-valid flag in bit 0 and the idle
flag in bit 1; byte 7 and the remaining flag bits are reserved and zero.  The
guest sets the idle flag when the current task is its idle task, so the host
can tell a guest heading to a halt from one running an ordinary task.

A pending descriptor must be zero when pending-valid is clear.  Reserved bits
and bytes are zero.  Producers must validate values before encoding them;
masking an out-of-range value does not make it valid.  The default guest area
accepts NORMAL and BATCH with nice -20 through 19 and zero RT priority, FIFO
and RR with nice zero and RT priority 1 through 99, and IDLE and DEADLINE with
zero nice and RT priority.  EXT and unknown policies are invalid for this
protocol version.

A custom guest-area protocol owns bytes 64 through 127 and interprets them
according to its policy identity and version.

Common host feedback
--------------------

Bytes 128 through 191 are common host feedback.  ``applied_state`` atomically
reports the kind of boost the host successfully applied, a deboost
notification hint, and framework CS/total-throttle flags.  It is written by the
framework in framework-managed mode and by the policy in policy-managed mode.
The writer publishes this coherent snapshot immediately before a committed
VMENTRY.  It describes the applied state for that entry, not live asynchronous
state between entries.  Both framework throttle flags are zero in
policy-managed mode.

``boost`` is ``PVSCHED_BOOST_BASELINE`` (not elevated, or a deboost),
``PVSCHED_BOOST_TASK`` (elevated for guest task intent) or
``PVSCHED_BOOST_CS`` (elevated for a critical section or a host reason).  The
policy's name and version fix what each means; the host's own configuration,
such as ``cs_rt_prio``, is not published.

``union pvsched_applied_state`` provides the same kind of local byte-oriented
snapshot view as task intent: byte 3 contains hints, byte 4 flags and byte 5
the boost, and bytes 0 through 2, 6 and 7 are reserved and zero.
Shared-memory users access only its complete aligned ``raw`` word
atomically.

For the default protocol, bytes 136 through 143 contain
``interrupt_ticket``.  Zero means no ticket; a nonzero value is an opaque
host-owned generation that requests the guest acknowledgment described
above.  The ticket is an interrupt-handoff token, not proof that FIFO was
applied or that a handler ran.  Bytes 144 through 191 are reserved and zero.

The default-protocol view overlays the first eight bytes of the 56-byte custom
host region after ``applied_state``.  A custom protocol retains ownership and
validation of all 56 bytes and need not interpret them as a ticket.

Interrupt acceptance, preparation, wakeup, guest entry, and handler service are
different events.  The ABI does not claim that any host status proves interrupt
service.

Extension storage
-----------------

Bytes 192 through 4095 are policy-specific extension storage.  The default
guest-area protocol requires them to be zero.  A custom policy defines their
ownership and validation.  The common header and ``applied_state`` never become
opaque policy storage; the custom guest area, custom host-feedback bytes,
and extension storage are the policy-defined regions.  No vCPU-placement ABI
is reserved speculatively.

Runtime scope
=============

Later changes may add transport and registration interfaces, KVM run-boundary
and entry/exit events, persistent per-vCPU accounts, scheduler application,
two-budget enforcement, guest publication hooks, optional policies, and tests.
Those changes must define their own locking, lifetime, validation, and failure
contracts.  The representation in ``include/uapi/linux/pvsched.h`` alone does
not provide atomic publication, input validation, scheduling, throttling, or
migration support.
