// SPDX-LICENSE-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/iopoll.h> 
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/dma-mapping.h>

#define EDU_BAR             0           // I/O memory, 1 MB in size. Users are supposed to communicate with the card through this memory
#define EDU_REG_ID          0x00        // specs says device version and identifier stays in this register
#define EDU_REG_LIVENESS    0x04        // write anything (32bit value) and get bitwise in
#define EDU_LIVENESS_INPUT  0x12345678  // random value
#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8
#define EDU_REG_FACTORIAL  0x08
#define EDU_REG_STATUS     0x20
#define EDU_STATUS_BUSY    0x01
#define EDU_STATUS_IRQ_ENABLE  0x80  // ask device to interrupt when factorial finishes
#define EDU_REG_IRQ_STATUS     0x24  // tells us which interrupt was raised
#define EDU_REG_IRQ_ACK        0x64  // write interrupt bits here to acknowledge them
#define EDU_IRQ_FACTORIAL      0x01  // factorial completion interrupt bit
#define EDU_DMA_BUF_SIZE  64         // bytes in our first DMA buffer
#define EDU_REG_DMA_SRC    0x80      // source address
#define EDU_REG_DMA_DST    0x88      // destination address
#define EDU_REG_DMA_COUNT  0x90      // number of bytes
#define EDU_REG_DMA_CMD    0x98      // start and direction
#define EDU_DMA_START      0x01      // start transfer; stays set while busy
#define EDU_DMA_TO_RAM     0x02      // direction: device buffer -> guest RAM
#define EDU_DMA_DEVICE_BUF 0x40000   // address of edu's internal buffer

// data for one edu device, shared by probe, interrupt handler and remove
struct edu_device {
	void __iomem *bar;                 // mapped BAR0; use bar + offset to access registers
	struct completion factorial_done;  // lets probe wait for factorial to finish
	int irq;                           // Linux IRQ number assigned to the device
    void *dma_buf;                     // CPU pointer for accessing the buffer
	dma_addr_t dma_handle;             // DMA address to give to the device
    bool dma_stuck;                    // don't free the DMA buffer if a transfer timed out
};

// kernel calls this fn in an interrupt thread
static irqreturn_t edu_irq_thread(int irq, void *data)
{
	struct edu_device *edu = data;
	u32 status;

	// find out which interrupt the device raised
	status = ioread32(edu->bar + EDU_REG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	// tell the device we handled the interrupt
	iowrite32(status, edu->bar + EDU_REG_IRQ_ACK);

	if (status & EDU_IRQ_FACTORIAL)
		complete(&edu->factorial_done);  // wake the waiting probe

	return IRQ_HANDLED;
}

static int edu_dma_transfer(struct edu_device *edu, bool to_ram) {
	u32 command;
	u32 status;

	// select source and destination for the requested direction
	if (to_ram) {
		iowrite32(EDU_DMA_DEVICE_BUF, edu->bar + EDU_REG_DMA_SRC);
		iowrite32((u32)edu->dma_handle, edu->bar + EDU_REG_DMA_DST);
		command = EDU_DMA_START | EDU_DMA_TO_RAM;
	} else {
		iowrite32((u32)edu->dma_handle, edu->bar + EDU_REG_DMA_SRC);
		iowrite32(EDU_DMA_DEVICE_BUF, edu->bar + EDU_REG_DMA_DST);
		command = EDU_DMA_START;
	}

	iowrite32(EDU_DMA_BUF_SIZE, edu->bar + EDU_REG_DMA_COUNT);

	// start DMA, then wait until the device clears the start bit
	iowrite32(command, edu->bar + EDU_REG_DMA_CMD);
	return readl_poll_timeout(edu->bar + EDU_REG_DMA_CMD, status,
				  !(status & EDU_DMA_START),
				  1000, 1000000);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    // probe order/flow: enable -> hold from others -> map
    struct edu_device *edu;
	u32 device_id;
	u32 liveness;
	int ret;
    u32 factorial;
    u8 *buf;
    int i;

    // allocate data for this device; automatically freed when probe fails or device is removed
	edu = devm_kzalloc(&pdev->dev, sizeof(*edu), GFP_KERNEL);
	if (!edu) return -ENOMEM;

	// prepare the completion before the handler or waiting code can use it
	init_completion(&edu->factorial_done);

    // prepare the device for use
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device: %d\n", ret);
		return ret;
	}

    // allow the device to initiate PCI transactions, including MSI writes
	pci_set_master(pdev);

    // edu supports 28-bit DMA addresses by default
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28));
	if (ret) {
        // specsheet:
        // For educational purposes, the device supports only 28 bits (256 MiB) by default
        // Students shall set dma_mask for the device in the OS driver properly
		dev_err(&pdev->dev, "28-bit DMA is not available: %d\n", ret);
		goto disable_device;
	}

    // allocate memory accessible to both the CPU and the device
	edu->dma_buf = dma_alloc_coherent(&pdev->dev, EDU_DMA_BUF_SIZE,
					  &edu->dma_handle, GFP_KERNEL);
	if (!edu->dma_buf) {
		dev_err(&pdev->dev, "failed to allocate DMA buffer\n");
		ret = -ENOMEM;
		goto disable_device;
	}

	dev_info(&pdev->dev, "allocated %d-byte DMA buffer, DMA address: %pad\n",
		 EDU_DMA_BUF_SIZE, &edu->dma_handle);

    // claim BAR0 so another driver cannot claim the same resource
	ret = pci_request_region(pdev, EDU_BAR, "edu");
	if (ret) {
		dev_err(&pdev->dev, "failed to claim BAR0: %d\n", ret);
		goto free_dma;
	}

    // mapping that a kernel can use to access it
    // __iomem because it represents I/) memory, not an ordinary RAM pointer
	// use bar + offset for every register
    edu->bar = pci_iomap(pdev, EDU_BAR, 0);
	if (!edu->bar) {
		dev_err(&pdev->dev, "failed to map BAR0\n");
		ret = -ENOMEM;
		goto release_region;
	}

    // stores mapping with the device so edu_remove() can unmap it
	pci_set_drvdata(pdev, edu);

    // allocate one MSI interrupt vector
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to allocate MSI: %d\n", ret);
		goto unmap_bar;
	}

	edu->irq = pci_irq_vector(pdev, 0);

	// register a threaded handler for that interrupt
	ret = request_threaded_irq(edu->irq, NULL, edu_irq_thread,
				   IRQF_ONESHOT, "edu", edu);
	if (ret) {
		dev_err(&pdev->dev, "failed to request IRQ: %d\n", ret);
		goto free_vectors;
	}

    // 0x00 (RO) identification
    // Value is in the form 0xRRrr00edu where: - RR – major version - rr – minor version
	device_id = ioread32(edu->bar + EDU_REG_ID);
	dev_info(&pdev->dev, "identification: 0x%08x\n", device_id);

    // 0x04 (RW) card liveness check
    // It is a simple value inversion (~ C operator).
	iowrite32(EDU_LIVENESS_INPUT, edu->bar + EDU_REG_LIVENESS);
	liveness = ioread32(edu->bar + EDU_REG_LIVENESS);
	dev_info(&pdev->dev, "liveness: wrote 0x%08x, read 0x%08x\n",
		 EDU_LIVENESS_INPUT, liveness);


    // 0x08 (RW)factorial computation
    // The stored value is taken and factorial of it is put back here. This happens only after factorial bit in the status register (0x20 below) is cleared.
    /*
    iowrite32(5, edu->bar + EDU_REG_FACTORIAL);
    // check status
	ret = readl_poll_timeout(edu->bar + EDU_REG_STATUS, status,
				 !(status & EDU_STATUS_BUSY),
				 100, 1000000);
	if (ret) {
		dev_err(&pdev->dev, "factorial calculation timed out\n");
		goto unmap_bar;
	}

    // read the factorial
	factorial = ioread32(edu->bar + EDU_REG_FACTORIAL);
	dev_info(&pdev->dev, "factorial: 5! = %u\n", factorial);
    */
    // enable the device's factorial completion interrupt
	iowrite32(EDU_STATUS_IRQ_ENABLE, edu->bar + EDU_REG_STATUS);

	// start the calculation
	iowrite32(5, edu->bar + EDU_REG_FACTORIAL);

	// sleep until the interrupt handler signals us, or one second passes
	if (!wait_for_completion_timeout(&edu->factorial_done,
					 msecs_to_jiffies(1000))) {
		dev_err(&pdev->dev, "factorial interrupt timed out\n");
		ret = -ETIMEDOUT;
		goto free_irq;
	}

    // read the factorial after waking up
	factorial = ioread32(edu->bar + EDU_REG_FACTORIAL);
	dev_info(&pdev->dev, "factorial: 5! = %u (MSI)\n", factorial);

    // ifll guest RAM with a known pattern
    buf = edu->dma_buf;
    for(i = 0; i < EDU_DMA_BUF_SIZE; i++)   buf[i] = i;

    // copy guest RAM -> edu's internal buffer
	ret = edu_dma_transfer(edu, false);
	if (ret)
		goto dma_timeout;

	// clear guest RAM so the next transfer must restore the pattern
	memset(buf, 0, EDU_DMA_BUF_SIZE);

	// copy edu's internal buffer -> guest RAM
	ret = edu_dma_transfer(edu, true);
	if (ret)
		goto dma_timeout;

	// verify every byte came back correctly
	for (i = 0; i < EDU_DMA_BUF_SIZE; i++) {
		if (buf[i] != i) {
			dev_err(&pdev->dev, "DMA mismatch at byte %d\n", i);
			ret = -EIO;
			goto free_irq;
		}
	}
	dev_info(&pdev->dev, "DMA round trip: %d bytes verified\n",
		 EDU_DMA_BUF_SIZE);

    return 0; // sucess: keep the resources until remove is called

dma_timeout:
    // transfer might still be using the buffer; retain it until guest reboot
    edu->dma_stuck = true;
    dev_err(&pdev->dev, "DMA timed out; reboot guest before retrying\n");
    goto free_irq;
free_irq:
	// stop requesting factorial interrupts before freeing the handler
	iowrite32(0, edu->bar + EDU_REG_STATUS);
	free_irq(edu->irq, edu);
free_vectors:
	pci_free_irq_vectors(pdev);
unmap_bar:
    pci_iounmap(pdev, edu->bar);
release_region:
	pci_release_region(pdev, EDU_BAR);
free_dma:
    if (!edu->dma_stuck)
        dma_free_coherent(&pdev->dev, EDU_DMA_BUF_SIZE, edu->dma_buf, edu->dma_handle);
disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void edu_remove(struct pci_dev *pdev) {
    // remove order/flow (opp to probe): unmap -> release -> disable

    // retrieve the data we stored in probe
	struct edu_device *edu = pci_get_drvdata(pdev);
    
    // disable device interrupts, then free the handler and MSI vector
	iowrite32(0, edu->bar + EDU_REG_STATUS);
	free_irq(edu->irq, edu);
	pci_free_irq_vectors(pdev);
	
    pci_iounmap(pdev, edu->bar);
	pci_release_region(pdev, EDU_BAR);
    dma_free_coherent(&pdev->dev, EDU_DMA_BUF_SIZE, edu->dma_buf, edu->dma_handle);
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
