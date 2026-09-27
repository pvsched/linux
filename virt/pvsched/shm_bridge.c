// SPDX-License-Identifier: GPL-2.0-only

#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/string.h>
#include <kunit/visibility.h>
#include <asm/barrier.h>

#include "shm_bridge.h"

void pvsched_shm_bridge_snapshot(const struct pvsched_shm *shm,
				 struct pvsched_default_guest_area *guest)
{
	const union pvsched_vcpu_page *page = shm->addr;

	/* Ack acquire-orders the associated CS and task-intent word samples. */
	guest->interrupt_ack =
		smp_load_acquire(&page->guest_area.default_area.interrupt_ack);
	guest->cs_state = READ_ONCE(page->guest_area.default_area.cs_state);
	guest->task_intent.raw =
		READ_ONCE(page->guest_area.default_area.task_intent.raw);
	memcpy(guest->reserved, page->guest_area.default_area.reserved,
	       sizeof(guest->reserved));
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_bridge_snapshot);

void pvsched_shm_bridge_publish_vmentry(const struct pvsched_shm *shm,
					 const struct pvsched_host_area *host)
{
	union pvsched_vcpu_page *page = shm->addr;

	/* The ABI promises coherent words, not an atomic ticket/state pair. */
	WRITE_ONCE(page->host_area.applied_state.raw, host->applied_state.raw);
	WRITE_ONCE(page->host_area.default_area.interrupt_ticket,
		   host->default_area.interrupt_ticket);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_bridge_publish_vmentry);

void pvsched_shm_bridge_read_negotiation(const struct pvsched_shm *shm,
					  struct pvsched_negotiation_request *request,
					  struct pvsched_default_guest_area *guest)
{
	const union pvsched_vcpu_page *page = shm->addr;
	const struct pvsched_header *header = &page->header;

	/* One bounded snapshot; negotiation never rereads the page. */
	request->abi_version = le32_to_cpu(READ_ONCE(header->abi_version));
	request->policy_version = le32_to_cpu(READ_ONCE(header->policy_version));
	memcpy(request->policy_name, header->policy_name,
	       sizeof(request->policy_name));
	request->protocol_id = le32_to_cpu(READ_ONCE(header->protocol_id));
	request->requested_mode = le32_to_cpu(READ_ONCE(header->requested_mode));
	pvsched_shm_bridge_snapshot(shm, guest);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_bridge_read_negotiation);

void pvsched_shm_bridge_publish_response(const struct pvsched_shm *shm,
					  enum pvsched_status status)
{
	union pvsched_vcpu_page *page = shm->addr;
	struct pvsched_header *header = &page->header;

	/*
	 * Host-owned storage is rewritten rather than trusted: zero feedback,
	 * ticket, reserved host bytes and extension storage, then fill the
	 * response.  The status store is last and releases all of it.
	 */
	memset(&page->host_area, 0, sizeof(page->host_area));
	memset(page->extension, 0, sizeof(page->extension));
	WRITE_ONCE(header->host_abi_version, cpu_to_le32(PVSCHED_ABI_VERSION));
	WRITE_ONCE(header->accepted_mode, cpu_to_le32(PVSCHED_MODE_FRAMEWORK));
	WRITE_ONCE(header->reserved, 0);
	smp_store_release(&header->status, cpu_to_le32(status));
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_bridge_publish_response);

void pvsched_shm_bridge_publish_status(const struct pvsched_shm *shm,
				       enum pvsched_status status)
{
	union pvsched_vcpu_page *page = shm->addr;

	smp_store_release(&page->header.status, cpu_to_le32(status));
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_bridge_publish_status);
