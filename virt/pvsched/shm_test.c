// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/shmem_fs.h>
#include <uapi/linux/pvsched.h>

#include "internal.h"
#include "shm.h"

struct pvsched_shm_test_state {
	struct page *page;
	void *addr;
	long pin_ret;
	unsigned int mapping_calls;
	unsigned int pin_calls;
	unsigned int unpin_calls;
	int mapping_ret;
	bool unpin_dirty;
};

static int pvsched_shm_test_mapping(struct mm_struct *mm,
				    unsigned long start, unsigned long end)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_shm_test_state *state = test->priv;

	state->mapping_calls++;
	return state->mapping_ret;
}

static long pvsched_shm_test_pin(struct mm_struct *mm,
				 unsigned long user_addr,
				 struct page **page)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_shm_test_state *state = test->priv;

	state->pin_calls++;
	mmap_assert_locked(mm);
	if (state->pin_ret == 1)
		*page = state->page;
	return state->pin_ret;
}

static void pvsched_shm_test_unpin(struct page *page, bool dirty)
{
	struct kunit *test = kunit_get_current_test();
	struct pvsched_shm_test_state *state = test->priv;

	state->unpin_calls++;
	state->unpin_dirty = dirty;
}

static void pvsched_shm_test_free_page(void *addr)
{
	free_page((unsigned long)addr);
}

static int pvsched_shm_test_init(struct kunit *test)
{
	struct pvsched_shm_test_state *state;
	int ret;

	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state);
	/* The pin stub returns a real page, so its direct-map address holds. */
	state->addr = (void *)__get_free_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->addr);
	ret = kunit_add_action_or_reset(test, pvsched_shm_test_free_page,
					state->addr);
	KUNIT_ASSERT_EQ(test, ret, 0);
	state->page = virt_to_page(state->addr);
	state->pin_ret = 1;
	test->priv = state;
	kunit_activate_static_stub(test, pvsched_shm_mapping_admit,
				   pvsched_shm_test_mapping);
	kunit_activate_static_stub(test, pvsched_shm_pin_page,
				   pvsched_shm_test_pin);
	kunit_activate_static_stub(test, pvsched_shm_unpin_page,
				   pvsched_shm_test_unpin);
	return 0;
}

static struct mm_struct *pvsched_shm_test_mm(void)
{
	return current->mm ?: current->active_mm;
}

static int pvsched_shm_test_prepare(struct pvsched_shm *shm,
				    unsigned long addr, bool cap_ipc_lock,
				    unsigned long memlock_limit_pages)
{
	return pvsched_shm_prepare(shm, pvsched_shm_test_mm(), addr,
				   cap_ipc_lock, memlock_limit_pages);
}

static void pvsched_shm_vma_admission_test(struct kunit *test)
{
	static const struct vm_operations_struct file_ops;
	struct file *shmem;
	struct vm_area_struct anonymous = {
		.vm_start = 0x1000,
		.vm_end = 0x3000,
		.vm_flags = VM_READ | VM_WRITE,
	};
	struct vm_area_struct readonly = {
		.vm_start = 0x1000,
		.vm_end = 0x3000,
		.vm_flags = VM_READ,
	};
	struct vm_area_struct io = {
		.vm_start = 0x1000,
		.vm_end = 0x3000,
		.vm_flags = VM_READ | VM_WRITE | VM_IO,
	};
	struct vm_area_struct hugetlb = {
		.vm_start = 0x200000,
		.vm_end = 0x400000,
		.vm_flags = VM_READ | VM_WRITE | VM_HUGETLB,
		.vm_ops = &file_ops,
	};
	int huge_ret;
	struct vm_area_struct file = {
		.vm_start = 0x1000,
		.vm_end = 0x3000,
		.vm_flags = VM_READ | VM_WRITE,
		.vm_ops = &file_ops,
	};
	struct vm_area_struct shmem_vma = {
		.vm_start = 0x1000,
		.vm_end = 0x3000,
		.vm_flags = VM_READ | VM_WRITE,
		.vm_ops = &file_ops,
	};

	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&anonymous, 0x1000, 0x2000),
			0);
	/* Unwritable or uncovered addresses fault; unsupported kinds do not. */
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&readonly, 0x1000, 0x2000),
			-EFAULT);
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(NULL, 0x1000, 0x2000),
			-EFAULT);
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&io, 0x1000, 0x2000),
			-EOPNOTSUPP);
	/* Any 4 KiB subpage of a hugetlb mapping is admitted. */
	huge_ret = IS_ENABLED(CONFIG_HUGETLB_PAGE) ? 0 : -EOPNOTSUPP;
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&hugetlb, 0x201000,
						    0x202000), huge_ret);
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&hugetlb, 0x3ff000,
						    0x400000), huge_ret);
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&file, 0x1000, 0x2000),
			-EOPNOTSUPP);

	shmem = shmem_file_setup("pvsched-shm-test", PAGE_SIZE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(shmem));
	shmem_vma.vm_file = shmem;
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&shmem_vma, 0x1000, 0x2000),
			0);
	fput(shmem);
	KUNIT_EXPECT_EQ(test, pvsched_shm_vma_admit(&anonymous, 0, 0x1000),
			-EFAULT);
}

static void pvsched_shm_success_release_test(struct kunit *test)
{
	struct pvsched_shm_test_state *state = test->priv;
	struct pvsched_shm shm;
	struct mm_struct *mm = pvsched_shm_test_mm();
	u64 pinned = atomic64_read(&mm->pinned_vm);
	int pages = atomic_read(&pvsched_shm_pages);
	int ret;

	ret = pvsched_shm_test_prepare(&shm, 0x1000, false,
				       ULONG_MAX);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned + 1);
	KUNIT_EXPECT_PTR_EQ(test, shm.page, state->page);
	KUNIT_EXPECT_PTR_EQ(test, shm.addr, state->addr);
	pvsched_shm_release(&shm);
	KUNIT_EXPECT_FALSE(test, shm.service_charged);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	KUNIT_EXPECT_TRUE(test, state->unpin_dirty);
	/* A second release refunds nothing more. */
	pvsched_shm_release(&shm);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);
}

static void pvsched_shm_alias_exclusion_test(struct kunit *test)
{
	struct pvsched_shm_test_state *state = test->priv;
	struct mm_struct *mm = pvsched_shm_test_mm();
	u64 pinned = atomic64_read(&mm->pinned_vm);
	int pages = atomic_read(&pvsched_shm_pages);
	struct pvsched_shm first, second;
	int ret;

	ret = pvsched_shm_test_prepare(&first, 0x1000, true, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = pvsched_shm_test_prepare(&second, 0x2000, true, 0);
	KUNIT_EXPECT_EQ(test, ret, -EEXIST);
	if (!ret)
		pvsched_shm_release(&second);
	/* The refused alias was pinned and charged; both are refunded. */
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 1U);
	KUNIT_EXPECT_TRUE(test, state->unpin_dirty);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages + 1);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned + 1);
	pvsched_shm_release(&first);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);
}

static void pvsched_shm_pin_under_mmap_lock_test(struct kunit *test)
{
	struct pvsched_shm_test_state *state = test->priv;
	struct pvsched_shm shm;
	int ret;

	ret = pvsched_shm_test_prepare(&shm, 0x1000, true, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, state->mapping_calls, 1U);
	KUNIT_EXPECT_EQ(test, state->pin_calls, 1U);
	pvsched_shm_release(&shm);
}

static void pvsched_shm_partial_failure_test(struct kunit *test)
{
	struct pvsched_shm_test_state *state = test->priv;
	struct pvsched_shm shm;
	struct mm_struct *mm = pvsched_shm_test_mm();
	u64 pinned = atomic64_read(&mm->pinned_vm);
	int pages = atomic_read(&pvsched_shm_pages);
	int ret;

	state->pin_ret = -EFAULT;
	ret = pvsched_shm_test_prepare(&shm, 0x1000, true, 0);
	KUNIT_EXPECT_EQ(test, ret, -EFAULT);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages);
	KUNIT_EXPECT_FALSE(test, shm.service_charged);
	KUNIT_EXPECT_EQ(test, state->unpin_calls, 0U);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);

	/* A refused mapping reports its own error and pins nothing. */
	state->pin_ret = 1;
	state->pin_calls = 0;
	state->mapping_ret = -EFAULT;
	ret = pvsched_shm_test_prepare(&shm, 0x1000, true, 0);
	KUNIT_EXPECT_EQ(test, ret, -EFAULT);
	KUNIT_EXPECT_EQ(test, state->pin_calls, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);
}

static void pvsched_shm_charge_failure_test(struct kunit *test)
{
	struct pvsched_shm_test_state *state = test->priv;
	struct pvsched_shm shm;
	struct mm_struct *mm = pvsched_shm_test_mm();
	u64 pinned = atomic64_read(&mm->pinned_vm);
	int pages = atomic_read(&pvsched_shm_pages);
	int ret;

	/* Without CAP_IPC_LOCK, the memlock limit refuses before pinning. */
	ret = pvsched_shm_test_prepare(&shm, 0x1000, false, pinned);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_EQ(test, state->pin_calls, 0U);
	KUNIT_EXPECT_FALSE(test, shm.service_charged);
	/* The global page charged before memlock is refunded. */
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages), pages);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);
}

static void pvsched_shm_global_limit_test(struct kunit *test)
{
	struct pvsched_shm_test_state *state = test->priv;
	struct mm_struct *mm = pvsched_shm_test_mm();
	u64 pinned = atomic64_read(&mm->pinned_vm);
	int pages = atomic_read(&pvsched_shm_pages);
	struct pvsched_shm shm;
	int ret;

	/*
	 * The global limit refuses before any pin or memlock charge.  Setting
	 * and restoring the shared count assumes nothing else pins meanwhile,
	 * which holds while no public ATTACH exists and suites run one by one.
	 */
	atomic_set(&pvsched_shm_pages, PVSCHED_MAX_SHM_PAGES_GLOBAL);
	ret = pvsched_shm_test_prepare(&shm, 0x1000, true, 0);
	KUNIT_EXPECT_EQ(test, ret, -ENOSPC);
	if (!ret)
		pvsched_shm_release(&shm);
	KUNIT_EXPECT_EQ(test, state->pin_calls, 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&pvsched_shm_pages),
			PVSCHED_MAX_SHM_PAGES_GLOBAL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&mm->pinned_vm), pinned);
	KUNIT_EXPECT_FALSE(test, shm.service_charged);
	atomic_set(&pvsched_shm_pages, pages);
}

static struct kunit_case pvsched_shm_test_cases[] = {
	KUNIT_CASE(pvsched_shm_vma_admission_test),
	KUNIT_CASE(pvsched_shm_success_release_test),
	KUNIT_CASE(pvsched_shm_alias_exclusion_test),
	KUNIT_CASE(pvsched_shm_pin_under_mmap_lock_test),
	KUNIT_CASE(pvsched_shm_partial_failure_test),
	KUNIT_CASE(pvsched_shm_charge_failure_test),
	KUNIT_CASE(pvsched_shm_global_limit_test),
	{}
};

static struct kunit_suite pvsched_shm_test_suite = {
	.name = "pvsched-shm",
	.init = pvsched_shm_test_init,
	.test_cases = pvsched_shm_test_cases,
};

kunit_test_suite(pvsched_shm_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
