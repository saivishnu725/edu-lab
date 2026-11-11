// SPDX-LICENSE-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/iopoll.h> 

#define EDU_BAR             0           // I/O memory, 1 MB in size. Users are supposed to communicate with the card through this memory
#define EDU_REG_ID          0x00        // specs says device version and identifier stays in this register
#define EDU_REG_LIVENESS    0x04        // write anything (32bit value) and get bitwise in
#define EDU_LIVENESS_INPUT  0x12345678  // random value
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8
#define EDU_REG_FACTORIAL  0x08
#define EDU_REG_STATUS     0x20
#define EDU_STATUS_BUSY    0x01


static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    // probe order/flow: enable -> hold from others -> map
    void __iomem *bar;
	u32 device_id;
	u32 liveness;
	int ret;
    u32 status;
    u32 factorial;

    // prepare the device for use
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device: %d\n", ret);
		return ret;
	}

    // claim BAR0 so another driver cannot claim the same resource
	ret = pci_request_region(pdev, EDU_BAR, "edu");
	if (ret) {
		dev_err(&pdev->dev, "failed to claim BAR0: %d\n", ret);
		goto disable_device;
	}

    // mapping that a kernel can use to access it
    // __iomem because it represents I/) memory, not an ordinary RAM pointer
	// use bar + offset for every register
    bar = pci_iomap(pdev, EDU_BAR, 0);
	if (!bar) {
		dev_err(&pdev->dev, "failed to map BAR0\n");
		ret = -ENOMEM;
		goto release_region;
	}

    // stores mapping with the device so edu_remove() can unmap it
	pci_set_drvdata(pdev, bar);

    // 0x00 (RO) identification
    // Value is in the form 0xRRrr00edu where: - RR – major version - rr – minor version
	device_id = ioread32(bar + EDU_REG_ID);
	dev_info(&pdev->dev, "identification: 0x%08x\n", device_id);

    // 0x04 (RW) card liveness check
    // It is a simple value inversion (~ C operator).
	iowrite32(EDU_LIVENESS_INPUT, bar + EDU_REG_LIVENESS);
	liveness = ioread32(bar + EDU_REG_LIVENESS);
	dev_info(&pdev->dev, "liveness: wrote 0x%08x, read 0x%08x\n",
		 EDU_LIVENESS_INPUT, liveness);


    // 0x08 (RW)factorial computation
    // The stored value is taken and factorial of it is put back here. This happens only after factorial bit in the status register (0x20 below) is cleared.
    iowrite32(5, bar + EDU_REG_FACTORIAL);

    // check status
	ret = readl_poll_timeout(bar + EDU_REG_STATUS, status,
				 !(status & EDU_STATUS_BUSY),
				 100, 1000000);
	if (ret) {
		dev_err(&pdev->dev, "factorial calculation timed out\n");
		goto unmap_bar;
	}

    // read the factorial
	factorial = ioread32(bar + EDU_REG_FACTORIAL);
	dev_info(&pdev->dev, "factorial: 5! = %u\n", factorial);

	return 0;

unmap_bar:
    pci_iounmap(pdev, bar);
release_region:
	pci_release_region(pdev, EDU_BAR);
disable_device:
	pci_disable_device(pdev);
	return ret;

    dev_info(&pdev->dev, "svnu: edu device found\n");
    return 0;
}

static void edu_remove(struct pci_dev *pdev) {
    // remove order/flow (opp to probe): unmap -> release -> disable
    void __iomem *bar = pci_get_drvdata(pdev);

	pci_iounmap(pdev, bar);
	pci_release_region(pdev, EDU_BAR);
	pci_disable_device(pdev);

    dev_info(&pdev->dev, "svnu: edu device removed\n");
}

static const struct pci_device_id edu_ids[] = {
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
    { }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static struct pci_driver edu_driver = {
    .name = "edu",
    .id_table = edu_ids,
    .probe = edu_probe,
    .remove = edu_remove,
};

module_pci_driver(edu_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("driver skeleton for the edu device");
