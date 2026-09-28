// SPDX-License-Identifier: GPL-2.0-only
/*
 * Paravirtualized scheduling: guest PCI transport driver.
 *
 * The courier between the built-in guest core and the VMM's pvsched PCI
 * device.  It reads the policy the device advertises, publishes one
 * {apic_id, gpa} entry per page the core prepared through the device's BAR1
 * table and doorbell (see
 * uapi/linux/pvsched_pci.h), and lets the core act on the statuses the host
 * wrote: the doorbell trap is synchronous, so they are valid when the write
 * retires.  It also provides the core's kick op, a write to the kick register
 * whose trap is itself the event.  All kernel-internal work lives in the
 * core, so this driver can be a module.
 */

#define pr_fmt(fmt) "pvsched_guest_pci: " fmt

#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pvsched_guest.h>
#include <asm/smp.h>

#include <uapi/linux/pvsched.h>
#include <uapi/linux/pvsched_pci.h>

static void __iomem *pvsched_pci_ctrl;	/* BAR0 */

/*
 * Force a synchronous exit; the device ignores the value because the trap is
 * the event.  Called from the core's hooks, possibly with IRQs disabled.
 */
static void pvsched_pci_kick(void)
{
	void __iomem *ctrl = READ_ONCE(pvsched_pci_ctrl);

	if (ctrl)
		writel(0, ctrl + PVSCHED_PCI_REG_KICK);
}

static int pvsched_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct pvsched_pci_entry __iomem *table;
	char policy[PVSCHED_NAME_MAX];
	u32 policy_version, result;
	unsigned int cpu, nr = 0, i;
	void __iomem *ctrl;
	int ret;

	/* One pvsched device per VM; reject a second instance. */
	if (pvsched_pci_ctrl)
		return -EEXIST;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ctrl = pcim_iomap(pdev, PVSCHED_PCI_BAR_CTRL, 0);
	table = pcim_iomap(pdev, PVSCHED_PCI_BAR_TABLE, 0);
	if (!ctrl || !table)
		return -ENOMEM;

	/* The device advertises the policy this VM should request. */
	policy_version = readl(ctrl + PVSCHED_PCI_REG_POLICY_VERSION);
	for (i = 0; i < PVSCHED_NAME_MAX / sizeof(u32); i++)
		((u32 *)policy)[i] =
			readl(ctrl + PVSCHED_PCI_REG_POLICY_NAME +
			      i * sizeof(u32));
	policy[PVSCHED_NAME_MAX - 1] = '\0';

	/* The kick op may be used as soon as the core attaches its hooks. */
	WRITE_ONCE(pvsched_pci_ctrl, ctrl);

	ret = pvsched_guest_prepare(policy, policy_version, pvsched_pci_kick);
	if (ret == -EPERM) {
		/* Declined by the guest's acceptance list: bound but inert. */
		return 0;
	}
	if (ret)
		goto err;

	/* Walk possible CPUs: prepare's set may no longer all be present. */
	for_each_possible_cpu(cpu) {
		phys_addr_t gpa = pvsched_guest_page_gpa(cpu);

		/* Not a CPU whose page this prepare reset. */
		if (!gpa)
			continue;
		if (nr == PVSCHED_PCI_MAX_VCPUS) {
			pr_warn("more than %zu possible CPUs; the rest run unparavirtualized\n",
				PVSCHED_PCI_MAX_VCPUS);
			break;
		}
		writeq(cpu_physical_id(cpu), &table[nr].apic_id);
		writeq(gpa, &table[nr].gpa);
		nr++;
	}

	/* Ring: the trap is synchronous, statuses are valid on return. */
	writel(nr, ctrl + PVSCHED_PCI_REG_DOORBELL);
	result = readl(ctrl + PVSCHED_PCI_REG_RESULT);
	if (result)
		pr_warn("the VMM could not attach every page: error %u\n",
			result);

	ret = pvsched_guest_commit();
	if (ret < 0) {
		writel(0, ctrl + PVSCHED_PCI_REG_DOORBELL);
		goto err;
	}

	return 0;

err:
	WRITE_ONCE(pvsched_pci_ctrl, NULL);
	return ret;
}

/*
 * Also the shutdown op: kexec does not reset the device, and the next kernel
 * reuses the pages, so detach them before the host can write into that
 * kernel's memory.
 */
static void pvsched_pci_remove(struct pci_dev *pdev)
{
	/* Stop publishing and kicking, then detach the pages host-side. */
	pvsched_guest_teardown();
	writel(0, pvsched_pci_ctrl + PVSCHED_PCI_REG_DOORBELL);
	WRITE_ONCE(pvsched_pci_ctrl, NULL);
}

static const struct pci_device_id pvsched_pci_ids[] = {
	{ PCI_DEVICE(PVSCHED_PCI_VENDOR_ID, PVSCHED_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pvsched_pci_ids);

static struct pci_driver pvsched_pci_driver = {
	.name		= "pvsched_guest_pci",
	.id_table	= pvsched_pci_ids,
	.probe		= pvsched_pci_probe,
	.remove		= pvsched_pci_remove,
	.shutdown	= pvsched_pci_remove,
};
module_pci_driver(pvsched_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Paravirt scheduling guest PCI transport");
