.. SPDX-License-Identifier: GPL-2.0

====================================
Paravirtualized scheduling (pvsched)
====================================

:Author: Vineeth Pillai
:Status: ABI definition; runtime support is not yet implemented

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

Only the ABI version 1 shared-page representation is defined at this stage.
There is no registration ioctl, KVM hook, transport binding, scheduler
interface, or runtime pvsched implementation yet.

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

KVM will provide events, but will not own policy selection, shared-page
negotiation, or boost accounting.  Runtime interfaces and event ordering remain
future implementation work and are not implied by the ABI definitions.

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
