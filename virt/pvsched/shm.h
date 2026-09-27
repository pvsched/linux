/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _VIRT_PVSCHED_SHM_H
#define _VIRT_PVSCHED_SHM_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/types.h>

struct page;

/*
 * One attachment's pinned shared page.  It owns the long-term pin, the
 * global page and pinned_vm charges, and the page's entry in the hash that
 * keeps one physical page from being attached twice.  Preparation and
 * release may sleep and must run without any raw lock held.  Once prepared,
 * addr is a stable direct-map address: the SHM bridge accesses it with
 * aligned 64-bit loads and stores inside attachment state_lock visits,
 * never after the attachment is unhashed and drained.
 */
struct pvsched_shm {
	struct hlist_node hash_node;
	struct mm_struct *mm;
	struct page *page;
	void *addr;
	bool mm_charged;
	bool service_charged;
	bool hashed;
};

int pvsched_shm_prepare(struct pvsched_shm *shm,
			struct mm_struct *mm, unsigned long user_addr,
			bool cap_ipc_lock, unsigned long memlock_limit_pages);
void pvsched_shm_release(struct pvsched_shm *shm);

#if IS_ENABLED(CONFIG_KUNIT)
extern atomic_t pvsched_shm_pages;
int pvsched_shm_vma_admit(struct vm_area_struct *vma,
			  unsigned long start, unsigned long end);
int pvsched_shm_mapping_admit(struct mm_struct *mm,
			      unsigned long start, unsigned long end);
long pvsched_shm_pin_page(struct mm_struct *mm, unsigned long user_addr,
			  struct page **page);
void pvsched_shm_unpin_page(struct page *page, bool dirty);
#endif

#endif /* _VIRT_PVSCHED_SHM_H */
