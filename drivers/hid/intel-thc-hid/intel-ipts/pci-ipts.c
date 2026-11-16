// SPDX-License-Identifier: GPL-2.0
/*
 * PCI glue for the Intel Touch Host Controller in IPTS mode. Ports that
 * the HIDSPI driver already claims are deliberately left out of the ID table.
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#include "ipts.h"

static const struct pci_device_id thc_ipts_pci_ids[] = {
	/* Lakefield */
	{PCI_DEVICE_DATA(INTEL, THC_LKF_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_LKF_PORT2, NULL)},
	/* Tiger Lake */
	{PCI_DEVICE_DATA(INTEL, THC_TGL_LP_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_TGL_LP_PORT2, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_TGL_H_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_TGL_H_PORT2, NULL)},
	/* Alder Lake */
	{PCI_DEVICE_DATA(INTEL, THC_ADL_S_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_ADL_S_PORT2, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_ADL_P_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_ADL_P_PORT2, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_ADL_M_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_ADL_M_PORT2, NULL)},
	/* Raptor Lake */
	{PCI_DEVICE_DATA(INTEL, THC_RPL_S_PORT1, NULL)},
	{PCI_DEVICE_DATA(INTEL, THC_RPL_S_PORT2, NULL)},
	{}
};
MODULE_DEVICE_TABLE(pci, thc_ipts_pci_ids);

static int thc_ipts_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	struct thc_ipts *idev;
	void __iomem *mem;
	unsigned long flags;
	int ret;

	idev = devm_kzalloc_obj(&pci->dev, *idev);
	if (!idev)
		return -ENOMEM;

	idev->dev = &pci->dev;
	pci_set_drvdata(pci, idev);
	INIT_WORK(&idev->recover_work, thc_ipts_recover);

	ret = pcim_enable_device(pci);
	if (ret) {
		dev_err(idev->dev,
			"cannot enable the PCI function, error %d\n", ret);
		return ret;
	}

	pci_set_master(pci);

	mem = pcim_iomap_region(pci, 0, KBUILD_MODNAME);
	ret = PTR_ERR_OR_ZERO(mem);
	if (ret) {
		dev_err(idev->dev, "cannot map the register space, error %d\n",
			ret);
		return ret;
	}

	ret = dma_set_mask_and_coherent(idev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(idev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(idev->dev, "no usable DMA configuration, error %d\n",
			ret);
		return ret;
	}

	idev->thc = thc_dev_init(idev->dev, mem);
	if (IS_ERR(idev->thc))
		return PTR_ERR(idev->thc);

	/*
	 * Firmware may leave the global enable set, and the line is shared and
	 * level triggered: mask it until a handler is in place.
	 */
	thc_interrupt_enable(idev->thc, false);

	/*
	 * Interrupt remapping rejects the MSI these ports ask for, so take the
	 * shared legacy pin; MSI is for a platform that routes no pin.
	 */
	flags = IRQF_SHARED | IRQF_COND_ONESHOT;
	ret = pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_INTX);
	if (ret < 0) {
		dev_warn(idev->dev, "no interrupt pin, falling back to MSI\n");
		ret = pci_alloc_irq_vectors(pci, 1, 1,
					    PCI_IRQ_MSI | PCI_IRQ_MSIX);
		/* A message interrupt is not shared and nothing else can hold
		 * it: neither flag applies.
		 */
		flags = 0;
	}
	if (ret < 0) {
		dev_err(idev->dev, "no interrupt vector, error %d\n", ret);
		return ret;
	}

	idev->irq = pci_irq_vector(pci, 0);
	if (idev->irq < 0) {
		dev_err(idev->dev, "no interrupt number, error %d\n",
			idev->irq);
		return idev->irq;
	}

	/* Oneshot only if a sharer has it: the hardirq masks the port. */
	ret = devm_request_threaded_irq(idev->dev, idev->irq, thc_ipts_hardirq,
					thc_ipts_thread, flags, KBUILD_MODNAME,
					idev);
	if (ret) {
		dev_err(idev->dev, "cannot get the interrupt, error %d\n", ret);
		return ret;
	}

	ret = thc_ipts_port_start(idev);
	if (ret) {
		thc_ipts_port_stop(idev);
		devm_free_irq(idev->dev, idev->irq, idev);
		return ret;
	}

	dev_info(idev->dev, "controller %04x:%04x rev %u\n",
		 idev->vendor, idev->product, le32_to_cpu(idev->cfg.revision));
	dev_info(idev->dev, "spi cfg 0x%08x, rx %zu tx %zu bytes\n",
		 le32_to_cpu(idev->cfg.spi_cfg), idev->max_rx, idev->max_tx);

	return 0;
}

static void thc_ipts_remove(struct pci_dev *pci)
{
	struct thc_ipts *idev = pci_get_drvdata(pci);

	thc_ipts_port_stop(idev);

	/* The handler has to go before the vector it is bound to is freed;
	 * the vectors themselves belong to pcim_enable_device().
	 */
	devm_free_irq(idev->dev, idev->irq, idev);
	pci_clear_master(pci);
}

static void thc_ipts_shutdown(struct pci_dev *pci)
{
	struct thc_ipts *idev = pci_get_drvdata(pci);

	if (idev)
		thc_ipts_port_stop(idev);
}

static struct pci_driver thc_ipts_driver = {
	.name		= "intel-ipts",
	.id_table	= thc_ipts_pci_ids,
	.probe		= thc_ipts_probe,
	.remove		= thc_ipts_remove,
	.shutdown	= thc_ipts_shutdown,
	/* Bring-up waits for the controller, up to a few seconds. */
	.driver.probe_type = PROBE_PREFER_ASYNCHRONOUS,
};
module_pci_driver(thc_ipts_driver);

MODULE_AUTHOR("Emfox Zhou <emfox@debian.org>");
MODULE_DESCRIPTION("IPTS transport for the Intel Touch Host Controller");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("INTEL_THC");
