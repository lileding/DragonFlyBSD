/*-
 * SPDX-License-Identifier: Dual BSD/GPL
 *
 * Minimal Linux MSI-X consumer for the dfvmm direct-doorbell test.
 */
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pci.h>

#define DFVMM_PCIE_VENDOR_ID		0x1b36
#define DFVMM_PCIE_MSIX_DEVICE_ID	0xdf03
#define DFVMM_PCIE_KICK_OFFSET		0x1000
#define DFVMM_PCIE_STATUS_OFFSET	0x1004
#define DFVMM_PCIE_KICK_VALUE		0x6b69636bU
#define DFVMM_PCIE_STATUS_VALUE		0x646f6e65U

struct dfvmm_pcie_msix {
	void __iomem	*own_mut_bar;
	struct completion own_mut_completion;
	int		mut_irq;
};

static irqreturn_t dfvmm_pcie_msix_irq(int irq, void *arg);
static int dfvmm_pcie_msix_probe(struct pci_dev *pdev,
    const struct pci_device_id *id);
static void dfvmm_pcie_msix_remove(struct pci_dev *pdev);

static const struct pci_device_id dfvmm_pcie_msix_ids[] = {
	{ PCI_DEVICE(DFVMM_PCIE_VENDOR_ID, DFVMM_PCIE_MSIX_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, dfvmm_pcie_msix_ids);

static struct pci_driver dfvmm_pcie_msix_driver = {
	.name = "dfvmm_pcie_msix",
	.id_table = dfvmm_pcie_msix_ids,
	.probe = dfvmm_pcie_msix_probe,
	.remove = dfvmm_pcie_msix_remove,
};
module_pci_driver(dfvmm_pcie_msix_driver);

static irqreturn_t
dfvmm_pcie_msix_irq(int irq, void *arg)
{
	struct dfvmm_pcie_msix *state = arg;
	u32 status;

	(void)irq;
	status = readl(state->own_mut_bar + DFVMM_PCIE_STATUS_OFFSET);
	if (status != DFVMM_PCIE_STATUS_VALUE)
		return IRQ_NONE;
	pr_info("DFVMM_PCIE_MSIX_IRQ_OK status=%08x\n", status);
	complete(&state->own_mut_completion);
	return IRQ_HANDLED;
}

static int
dfvmm_pcie_msix_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct dfvmm_pcie_msix *state;
	long timeout;
	int error;

	(void)id;
	state = devm_kzalloc(&pdev->dev, sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return -ENOMEM;
	error = pci_enable_device_mem(pdev);
	if (error != 0)
		return error;
	error = pci_request_region(pdev, 0, "dfvmm_pcie_msix");
	if (error != 0)
		goto fail_disable;
	state->own_mut_bar = pci_iomap(pdev, 0, 0);
	if (state->own_mut_bar == NULL) {
		error = -ENOMEM;
		goto fail_region;
	}
	error = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (error < 0)
		goto fail_iounmap;
	state->mut_irq = pci_irq_vector(pdev, 0);
	init_completion(&state->own_mut_completion);
	error = request_irq(state->mut_irq, dfvmm_pcie_msix_irq, 0,
	    "dfvmm_pcie_msix", state);
	if (error != 0)
		goto fail_vectors;
	pci_set_drvdata(pdev, state);
	writel(DFVMM_PCIE_KICK_VALUE,
	    state->own_mut_bar + DFVMM_PCIE_KICK_OFFSET);
	timeout = wait_for_completion_timeout(&state->own_mut_completion,
	    msecs_to_jiffies(5000));
	if (timeout != 0) {
		pr_info("DFVMM_PCIE_MSIX_PROBE_OK irq=%d\n", state->mut_irq);
		return 0;
	}
	pr_err("DFVMM_PCIE_MSIX_IRQ_TIMEOUT irq=%d\n", state->mut_irq);
	free_irq(state->mut_irq, state);
	pci_set_drvdata(pdev, NULL);
fail_vectors:
	pci_free_irq_vectors(pdev);
fail_iounmap:
	pci_iounmap(pdev, state->own_mut_bar);
fail_region:
	pci_release_region(pdev, 0);
fail_disable:
	pci_disable_device(pdev);
	return error != 0 ? error : -ETIMEDOUT;
}

static void
dfvmm_pcie_msix_remove(struct pci_dev *pdev)
{
	struct dfvmm_pcie_msix *state = pci_get_drvdata(pdev);

	if (state == NULL)
		return;
	free_irq(state->mut_irq, state);
	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, state->own_mut_bar);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
}

MODULE_DESCRIPTION("dfvmm direct BAR and MSI-X test consumer");
MODULE_LICENSE("Dual BSD/GPL");
