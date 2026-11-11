// SPDX-LICENSE-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    dev_info(&pdev->dev, "svnu: edu device found\n");
    return 0;
}

static void edu_remove(struct pci_dev *pdev) {
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
