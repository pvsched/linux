// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/hugetlb.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/shmem_fs.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include <uapi/linux/pvsched.h>

#include "internal.h"
#include "shm.h"

#define PVSCHED_SHM_HASH_BITS 8

/* Pinned pages keyed by struct page *: excludes attaching one page twice. */
static DEFINE_HASHTABLE(pvsched_shm_hash, PVSCHED_SHM_HASH_BITS);
static DEFINE_SPINLOCK(pvsched_shm_hash_lock);
/* Pages pinned across all sessions, against PVSCHED_MAX_SHM_PAGES_GLOBAL. */
VISIBLE_IF_KUNIT atomic_t pvsched_shm_pages = ATOMIC_INIT(0);
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_pages);

/*
 * An address the caller cannot write is a fault; a writable mapping of an
 * unsupported kind (I/O, PFN, DAX or non-shmem file) is not.  Anonymous,
 * shmem and hugetlb memory are admitted: a VMM may back guest RAM with any of
 * them, and the pin then holds the exact 4 KiB subpage of a hugepage.
 */
VISIBLE_IF_KUNIT int
pvsched_shm_vma_admit(struct vm_area_struct *vma,
		      unsigned long start, unsigned long end)
{
	if (!vma || start < vma->vm_start || end > vma->vm_end ||
	    !(vma->vm_flags & VM_WRITE))
		return -EFAULT;
	if ((vma->vm_flags & (VM_IO | VM_PFNMAP | VM_MIXEDMAP)) ||
	    vma_is_dax(vma))
		return -EOPNOTSUPP;

	/* shmem_file() is module-safe; vma_is_shmem() is not exported. */
	if (vma_is_anonymous(vma) || is_vm_hugetlb_page(vma) ||
	    (vma->vm_file && shmem_file(vma->vm_file)))
		return 0;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_vma_admit);

VISIBLE_IF_KUNIT int
pvsched_shm_mapping_admit(struct mm_struct *mm,
			  unsigned long start, unsigned long end)
{
	KUNIT_STATIC_STUB_REDIRECT(pvsched_shm_mapping_admit, mm, start, end);
	return pvsched_shm_vma_admit(vma_lookup(mm, start), start, end);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_mapping_admit);

VISIBLE_IF_KUNIT long
pvsched_shm_pin_page(struct mm_struct *mm, unsigned long user_addr,
		     struct page **page)
{
	KUNIT_STATIC_STUB_REDIRECT(pvsched_shm_pin_page, mm, user_addr, page);
	/*
	 * NULL deliberately forbids FOLL_UNLOCKABLE.  Remote GUP therefore keeps
	 * the caller's mmap read lock held, preserving the admitted VMA identity
	 * and backing provenance throughout pinning.  The pin then preserves the
	 * returned page identity; it does not freeze later page-cache mappings.
	 */
	return pin_user_pages_remote(mm, user_addr, 1,
				     FOLL_WRITE | FOLL_LONGTERM, page, NULL);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_pin_page);

VISIBLE_IF_KUNIT void pvsched_shm_unpin_page(struct page *page, bool dirty)
{
	KUNIT_STATIC_STUB_REDIRECT(pvsched_shm_unpin_page, page, dirty);
	unpin_user_pages_dirty_lock(&page, 1, dirty);
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_unpin_page);

/*
 * A session holds at most one page per runner, so the per-session runner
 * limit also bounds its pinned pages; only the global limit is counted here.
 */
static_assert(PVSCHED_MAX_RUNNERS_PER_SESSION <=
	      PVSCHED_MAX_SHM_PAGES_PER_SESSION);
/* One attachment is one page: the stride is the x86 4 KiB page. */
static_assert(PAGE_SIZE == PVSCHED_VCPU_STRIDE);

static int pvsched_shm_charge(struct pvsched_shm *shm, struct mm_struct *mm,
			      bool cap_ipc_lock,
			      unsigned long memlock_limit_pages)
{
	u64 pinned;

	if (!atomic_add_unless(&pvsched_shm_pages, 1,
			       PVSCHED_MAX_SHM_PAGES_GLOBAL))
		return -ENOSPC;
	shm->service_charged = true;

	pinned = atomic64_add_return(1, &mm->pinned_vm);
	mmgrab(mm);
	shm->mm = mm;
	shm->mm_charged = true;
	if (!cap_ipc_lock && pinned > memlock_limit_pages)
		return -ENOMEM;
	return 0;
}

static int pvsched_shm_hash_add(struct pvsched_shm *shm)
{
	struct pvsched_shm *other;

	spin_lock(&pvsched_shm_hash_lock);
	hash_for_each_possible(pvsched_shm_hash, other, hash_node,
			       (unsigned long)shm->page) {
		if (other->page == shm->page) {
			spin_unlock(&pvsched_shm_hash_lock);
			return -EEXIST;
		}
	}
	hash_add(pvsched_shm_hash, &shm->hash_node,
		 (unsigned long)shm->page);
	shm->hashed = true;
	spin_unlock(&pvsched_shm_hash_lock);
	return 0;
}

/*
 * The ATTACH ioctl already checked that @user_addr is stride-aligned and
 * that the page it starts does not wrap the address space.
 */
int pvsched_shm_prepare(struct pvsched_shm *shm,
			struct mm_struct *mm, unsigned long user_addr,
			bool cap_ipc_lock, unsigned long memlock_limit_pages)
{
	unsigned long end = user_addr + PVSCHED_VCPU_STRIDE;
	long pinned;
	int ret;

	memset(shm, 0, sizeof(*shm));

	ret = pvsched_shm_charge(shm, mm, cap_ipc_lock, memlock_limit_pages);
	if (ret)
		goto fail;

	mmap_read_lock(mm);
	ret = pvsched_shm_mapping_admit(mm, user_addr, end);
	if (ret)
		goto unlock;
	pinned = pvsched_shm_pin_page(mm, user_addr, &shm->page);
	if (pinned != 1) {
		ret = pinned < 0 ? pinned : -EFAULT;
		goto unlock;
	}
	/*
	 * Admission refused PFN, mixed, I/O and DAX mappings, and a long-term
	 * pin migrates coherent device memory or fails, so the pinned page is
	 * ordinary RAM.  x86-64 has no highmem: its direct-map address is set.
	 */
	if (WARN_ON_ONCE(is_zone_device_page(shm->page))) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	shm->addr = page_address(shm->page);
	ret = pvsched_shm_hash_add(shm);
unlock:
	mmap_read_unlock(mm);
	if (!ret)
		return 0;
fail:
	pvsched_shm_release(shm);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_prepare);

void pvsched_shm_release(struct pvsched_shm *shm)
{
	if (!shm)
		return;
	if (shm->hashed) {
		spin_lock(&pvsched_shm_hash_lock);
		hash_del(&shm->hash_node);
		spin_unlock(&pvsched_shm_hash_lock);
		shm->hashed = false;
	}
	if (shm->page) {
		pvsched_shm_unpin_page(shm->page, true);
		shm->page = NULL;
	}
	if (shm->mm_charged) {
		atomic64_dec(&shm->mm->pinned_vm);
		mmdrop(shm->mm);
		shm->mm_charged = false;
	}
	if (shm->service_charged) {
		atomic_dec(&pvsched_shm_pages);
		shm->service_charged = false;
	}
	shm->addr = NULL;
	shm->mm = NULL;
}
EXPORT_SYMBOL_IF_KUNIT(pvsched_shm_release);
